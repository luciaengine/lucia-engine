#include "FixedScalePlatform.h"
#include "SolidTga.h"
#include "UIPixelReadback.h"
#include "UIRgTestHarness.h"

#include "Assets/AssetManager.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Rendering/Core/PassPhase.h"
#include "UI/Parsers/CSSParser.h"
#include "UI/Parsers/XMLParser.h"
#include "UI/Registration/ElementRegistration.h"
#include "UI/UIElement.h"
#include "UI/UIManager.h"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

using namespace GameEngine;
using namespace GameEngine::Rendering;

namespace
{

// The raster image's one color, as authored (sRGB bytes).
constexpr uint8_t kRed = 40;
constexpr uint8_t kGreen = 160;
constexpr uint8_t kBlue = 220;
constexpr uint16_t kImageSize = 64;

constexpr const char* kSceneViewName = "scene_view";

// The raster element comes first in tree order, so a full regeneration
// registers its image before the render-graph element reserves a slot.
constexpr const char* kLayout = R"(<uielement id='root'><uielement id='logo' /><uielement id='viewport' /></uielement>)";
constexpr const char* kCss = R"(
#root { display: flex; width: 256px; height: 128px; }
#logo { width: 64px; height: 64px; background-image: url("logo.tga"); }
#viewport { width: 128px; height: 64px; background-image: engine(scene_view); }
#viewport.wide { width: 160px; }
)";

TextureDesc MakeTargetDesc(const char* name)
{
    TextureDesc desc{};
    desc.width = 256;
    desc.height = 128;
    desc.mipLevels = 1;
    desc.arrayLayers = 1;
    desc.sampleCount = 1;
    desc.format = static_cast<uint32_t>(TextureFormat::R16G16B16A16_FLOAT);
    desc.usage = static_cast<uint32_t>(TextureUsage::RenderTarget) |
                 static_cast<uint32_t>(TextureUsage::ShaderResource);
    desc.debugName = name;
    return desc;
}

// A raster image element and a render-graph element in one tree. The image is
// not preloaded: the UI's own load path runs, as it does for an editor icon.
struct DeferredSlotFixture
{
    JobSystem::WorkStealingThreadPool Pool{2};
    AssetManager Assets;
    GUID Guid;
    UITesting::FixedScalePlatform Platform{1.0f};
    std::unique_ptr<UIManager> Ui;
    std::unique_ptr<UiRgHarness> Rg;
    UIElement* Viewport = nullptr;

    bool Build(IDevice* device, const char* name)
    {
        UIRegistration::RegisterBuiltInControls();
        const std::filesystem::path root = std::filesystem::temp_directory_path() / name;
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        const std::filesystem::path image = root / "logo.tga";
        UITesting::WriteSolidTga(image, kImageSize, kImageSize, kRed, kGreen, kBlue);
        if (!Assets.Initialize(root, &Pool) || !Assets.GetRegistry().RegisterAsset(image))
            return false;
        Guid = Assets.GetRegistry().GetAssetGUID(image);
        if (Guid.IsNull())
            return false;

        std::unique_ptr<UIElement> rootElement;
        if (!UIParsing::XMLParser::ParseLayoutFromString(kLayout, rootElement))
            return false;
        Ui = std::make_unique<UIManager>(device, &Assets);
        Ui->SetPlatform(&Platform);
        Ui->SetLayoutSizeOverride(256, 128);
        Ui->SetRoot(std::move(rootElement));
        Stylesheet sheet{};
        if (!UIParsing::CSSParser::ParseStylesFromString(kCss, sheet))
            return false;
        Ui->AddStylesheet(std::make_shared<const Stylesheet>(std::move(sheet)));
        Ui->SetExternalTextureRG(kSceneViewName, 128, 64, UI::UITextureSpace::SrgbAuthored(),
                                 TextureFormat::RGBA8_SRGB);
        Rg = std::make_unique<UiRgHarness>(device);
        Viewport = Ui->GetRootElement()->FindById(HashStringId("viewport"));
        return Viewport != nullptr;
    }

    // Waits for the asset load the first paint started, without a UI update:
    // the upload it queued on the UI dispatcher stays queued.
    bool WaitForAssetLoad()
    {
        constexpr auto kLoadTimeout = std::chrono::seconds(5);
        constexpr auto kPollInterval = std::chrono::milliseconds(1);
        const auto deadline = std::chrono::steady_clock::now() + kLoadTimeout;
        while (!Assets.GetAsset(Guid))
        {
            if (std::chrono::steady_clock::now() >= deadline)
                return false;
            std::this_thread::sleep_for(kPollInterval);
        }
        return true;
    }
};

// Declares a producer that clears a texture and publishes it under the
// render-graph element's name for this frame.
void PublishSceneView(UIManager& ui, RenderGraph::RGFrame& frame)
{
    const RenderGraph::RGTexture scene = frame.CreateTexture("Test.SceneView", MakeTargetDesc("Test.SceneView"));
    frame.AddPass(
        "Test.SceneView", PassPhase::kWorldRender,
        [scene](RenderGraph::RGPassBuilder& p)
        {
            RenderGraph::RGAttachmentOps ops{};
            ops.Load = RenderGraph::RGLoadOp::Clear;
            ops.Clear.Color[0] = 0.0f;
            ops.Clear.Color[1] = 1.0f;
            ops.Clear.Color[2] = 0.0f;
            ops.Clear.Color[3] = 1.0f;
            p.AttachColor(0, scene, ops);
        },
        [](RenderGraph::RGContext&) {});
    ui.PublishExternalTextureRG(kSceneViewName, frame, scene);
}

// Declares one UI pass into its own target, kept alive past culling so its
// execute step runs.
void DeclareUi(UIManager& ui, RenderGraph::RGFrame& frame, const char* targetName)
{
    const RenderGraph::RGTexture target = frame.CreateTexture(targetName, MakeTargetDesc(targetName));
    ASSERT_TRUE(ui.RenderRG(frame, target, UI::UITargetSpace::EncodedSrgb()));
    frame.AddPass(
        "Test.KeepUi", PassPhase::kFinalize,
        [target](RenderGraph::RGPassBuilder& p)
        {
            p.Read(target, RenderGraph::RGTextureRead::Sampled);
            p.PreventCulling();
        },
        [](RenderGraph::RGContext&) {});
}

bool NearImageColor(const UITesting::Rgb& pixel)
{
    constexpr int kTolerance = 3;
    return std::abs(pixel.R - kRed) <= kTolerance && std::abs(pixel.G - kGreen) <= kTolerance &&
           std::abs(pixel.B - kBlue) <= kTolerance;
}

} // namespace

// A UI pass declared before a regeneration still executes after it. The
// regeneration releases the render-graph element's slot, the raster image
// registers into that slot, and the render-graph element reserves the next
// one. The earlier pass must not write the released slot: the image draws.
TEST(UIDeferredTextureSlot, ReleasedSlotKeepsTheRasterThatReusesIt)
{
    IDevice* device = SharedHeadlessDevice();
    if (!device)
        GTEST_SKIP() << "No device";
    DeferredSlotFixture fx;
    ASSERT_TRUE(fx.Build(device, "ui_deferred_texture_slot"));

    fx.Ui->Update(1.0f / 60.0f, /*interactive=*/true);
    {
        RenderGraph::RGFrame frame(fx.Rg->Dev, &fx.Rg->Persistent, &fx.Rg->Transient, &fx.Rg->Ring);
        frame.BeginFrame(++fx.Rg->FrameIndex);
        PublishSceneView(*fx.Ui, frame);
        DeclareUi(*fx.Ui, frame, "Test.UiFirst");
        frame.Execute();
        device->WaitForIdle();
    }
    ASSERT_TRUE(fx.WaitForAssetLoad());

    {
        RenderGraph::RGFrame frame(fx.Rg->Dev, &fx.Rg->Persistent, &fx.Rg->Transient, &fx.Rg->Ring);
        frame.BeginFrame(++fx.Rg->FrameIndex);
        PublishSceneView(*fx.Ui, frame);
        DeclareUi(*fx.Ui, frame, "Test.UiBeforeRegen");
        fx.Viewport->AddClass("wide");
        fx.Ui->Update(1.0f / 60.0f, /*interactive=*/true);
        DeclareUi(*fx.Ui, frame, "Test.UiAfterRegen");
        frame.Execute();
        device->WaitForIdle();
    }

    const std::vector<uint8_t> pixels = UITesting::RenderUiToBytes(*fx.Ui, UI::UITargetSpace::EncodedSrgb());
    ASSERT_FALSE(pixels.empty());
    UITesting::Rgb centre;
    std::string why;
    ASSERT_TRUE(UITesting::UniformPatch(pixels, 32, 32, centre, why)) << why;
    EXPECT_TRUE(NearImageColor(centre)) << "drawn " << centre.R << "," << centre.G << "," << centre.B;
}
