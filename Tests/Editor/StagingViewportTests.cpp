// The staging viewport: several viewports in one panel tree, each over its own pooled view and
// texture; a held output shows after the view writes a new frame; a hidden host holds no view.

#include <gtest/gtest.h>

#include "Editor/RenderGraphTickRegistry.h"
#include "InspectorLayoutFixture.h"
#include "StagedTestPaths.h"
#include "UIRgTestHarness.h"
#include "UI/Controls/StagingViewport.h"

#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/StagingViewPool.h"
#include "Rendering/Common/Utils.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Passes/TonemapPass.h"
#include "UI/StyleProperties.h"
#include "UI/UIManager.h"

#include <cmath>
#include <filesystem>
#include <memory>
#include <string>

namespace
{
using namespace GameEngine;
using Editor::StagingViewport;
using Engine::Renderer::StagingViewDesc;

constexpr float kFrameSeconds = 1.0f / 60.0f;
constexpr float kViewportSize = 200.0f;

// The output's tonemap pass loads its shader package from the staged test tree.
std::filesystem::path StagedShaderPath(const std::filesystem::path& relativePath)
{
    const std::filesystem::path staged = TestPaths::StagedRoot() / relativePath;
    std::error_code error;
    return std::filesystem::exists(staged, error) ? staged : std::filesystem::path();
}

std::unique_ptr<UIElement> MakeSizedSlot()
{
    auto slot = std::make_unique<UIElement>();
    slot->Style()
        .Set(Style::Display, DisplayMode::Flex)
        .Set(Style::Width, StyleLength::Px(kViewportSize))
        .Set(Style::Height, StyleLength::Px(kViewportSize));
    return slot;
}

class StagingViewportTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        auto panel = std::make_unique<UIElement>();
        panel->Style().Set(Style::Display, DisplayMode::Flex).Set(Style::FlexDir, FlexDirection::Row);
        m_Panel = panel.get();
        m_Panel->AddChild(MakeSizedSlot());
        m_Panel->AddChild(MakeSizedSlot());
        m_Fixture.ExtraSheets = {"Assets/UI/controls/StagingViewport/StagingViewport.css"};
        if (!m_Fixture.Build(std::move(panel)))
            GTEST_SKIP() << m_Fixture.Diagnostic;
        ASSERT_TRUE(m_Services.Initialize(m_Fixture.Device.get()));
        m_Pool = std::make_unique<Engine::Renderer::StagingViewPool>(m_Services);

        auto left = std::make_unique<StagingViewport>(*m_Pool, m_Ticks, StagingViewDesc{64, 64, "Staging.Left"});
        auto right = std::make_unique<StagingViewport>(*m_Pool, m_Ticks, StagingViewDesc{64, 64, "Staging.Right"});
        m_Left = left.get();
        m_Right = right.get();
        m_Panel->GetChildren()[0]->AddChild(std::move(left));
        m_Panel->GetChildren()[1]->AddChild(std::move(right));
        for (int i = 0; i < 3; ++i)
            m_Fixture.Ui->Update(kFrameSeconds, /*interactive=*/true);
    }

    void TearDown() override
    {
        if (m_Pool)
        {
            m_Ticks.ReleaseAll();
            m_Fixture.Ui.reset();
            m_Pool.reset();
            m_Services.Shutdown();
        }
    }

    static std::string ImageTexture(const StagingViewport& viewport)
    {
        UIElement* image = const_cast<StagingViewport&>(viewport).FindById("staging-viewport-image"_sid);
        if (!image)
            return {};
        const auto source = image->Style().Get(Style::BackgroundImage);
        return source ? source->Value : std::string{};
    }

    // Runs one render graph tick. With `writeFrame` the view's owner writes a new output first, as the
    // Asset Preview's thumbnail lane does. The frame is declared and never executed.
    void Tick(UiRgHarness& harness, StagingViewport& viewport, bool writeFrame)
    {
        namespace RenderGraph = Rendering::RenderGraph;
        RenderGraph::RGFrame frame(harness.Dev, &harness.Persistent, &harness.Transient, &harness.Ring);
        frame.BeginFrame(++harness.FrameIndex);
        if (writeFrame)
        {
            Rendering::TextureDesc desc{};
            desc.width = 64;
            desc.height = 64;
            desc.depth = 1;
            desc.mipLevels = 1;
            desc.arrayLayers = 1;
            desc.sampleCount = 1;
            desc.format = static_cast<uint32_t>(Rendering::TextureFormat::R16G16B16A16_FLOAT);
            desc.usage = static_cast<uint32_t>(Rendering::TextureUsage::RenderTarget) |
                         static_cast<uint32_t>(Rendering::TextureUsage::ShaderResource);
            const RenderGraph::RGTexture source = frame.CreateTexture("Staging.Source", desc);
            ASSERT_TRUE(viewport.View().DeclareOutput(frame, source, Rendering::Passes::TonemapParams{}));
        }
        m_Ticks.TickAll(1, m_Fixture.Ui.get(), frame);
        m_Fixture.Ui->Update(kFrameSeconds, /*interactive=*/true);
    }

    static bool ImageVisible(const StagingViewport& viewport)
    {
        const UIElement* image = const_cast<StagingViewport&>(viewport).FindById("staging-viewport-image"_sid);
        return image && image->GetResolvedStyle().Layout.DisplayMode != DisplayMode::None;
    }

    InspectorLayoutTesting::InspectorLayoutFixture m_Fixture;
    Engine::Renderer::RenderServices m_Services;
    std::unique_ptr<Engine::Renderer::StagingViewPool> m_Pool;
    Editor::RenderGraphTickRegistry m_Ticks;
    UIElement* m_Panel = nullptr;
    StagingViewport* m_Left = nullptr;
    StagingViewport* m_Right = nullptr;
};

TEST_F(StagingViewportTest, EachViewportShowsItsOwnViewsTexture)
{
    EXPECT_NE(m_Left->View().ViewId(), m_Right->View().ViewId());
    const std::string left = ImageTexture(*m_Left);
    const std::string right = ImageTexture(*m_Right);
    EXPECT_EQ(left, m_Left->View().UiTextureName());
    EXPECT_EQ(right, m_Right->View().UiTextureName());
    EXPECT_FALSE(left.empty());
    EXPECT_NE(left, right) << "two viewports in one panel tree must not share a texture";
    EXPECT_GT(m_Left->GetLayoutWidth(), 0.0f) << "the viewport fills its slot";
}

TEST_F(StagingViewportTest, AHeldOutputShowsOnlyAfterTheViewWritesANewFrame)
{
    StagingViewport* viewport = m_Left;
    UiRgHarness harness(m_Fixture.Device.get());
    const Rendering::Utils::ShaderPathResolverFunc previousResolver = Rendering::Utils::GetShaderPathResolver();
    Rendering::Utils::SetShaderPathResolver(&StagedShaderPath);

    viewport->SetOutputShown(false);
    viewport->ShowAfterNextFrame();
    // Two ticks: the second follows the layout that the hide produced.
    Tick(harness, *viewport, /*writeFrame=*/false);
    Tick(harness, *viewport, /*writeFrame=*/false);
    EXPECT_FALSE(viewport->IsOutputShown()) << "a tick without a new frame keeps the previous subject hidden";
    EXPECT_FALSE(ImageVisible(*viewport));
    const float scale = m_Fixture.Ui->GetContentScale();
    EXPECT_EQ(viewport->View().RenderWidth(), static_cast<uint32_t>(std::lround(kViewportSize * scale)))
        << "a hidden output leaves the control's box, so the view renders at the laid-out size";

    Tick(harness, *viewport, /*writeFrame=*/true);
    EXPECT_TRUE(viewport->IsOutputShown()) << "the view's new frame shows the output";
    EXPECT_TRUE(ImageVisible(*viewport));
    Rendering::Utils::SetShaderPathResolver(previousResolver);
}

TEST_F(StagingViewportTest, TheViewTakesTheLastLaidOutSizeAndKeepsItWhileTheRectIsEmpty)
{
    UIElement& slot = *m_Panel->GetChildren()[0];
    slot.Style().Set(Style::Width, StyleLength::Px(120.0f));
    slot.Style().Set(Style::Width, StyleLength::Px(160.0f));
    m_Fixture.Ui->Update(kFrameSeconds, /*interactive=*/true);
    const uint32_t width = static_cast<uint32_t>(std::lround(160.0f * m_Fixture.Ui->GetContentScale()));
    EXPECT_EQ(m_Left->View().RenderWidth(), width) << "the last laid-out size wins";

    slot.Style().Set(Style::Width, StyleLength::Px(0.0f));
    m_Fixture.Ui->Update(kFrameSeconds, /*interactive=*/true);
    EXPECT_EQ(m_Left->View().RenderWidth(), width) << "an empty rect keeps the last size";
}

TEST_F(StagingViewportTest, AShownOutputKeepsTheLastFrameUntilTheViewWritesANewOne)
{
    StagingViewport* viewport = m_Left;
    UiRgHarness harness(m_Fixture.Device.get());
    const Rendering::Utils::ShaderPathResolverFunc previousResolver = Rendering::Utils::GetShaderPathResolver();
    Rendering::Utils::SetShaderPathResolver(&StagedShaderPath);
    Tick(harness, *viewport, /*writeFrame=*/true);
    viewport->SetOutputShown(true);

    // The host switches the subject: the previous subject's frame stays on screen.
    viewport->ShowAfterNextFrame();
    Tick(harness, *viewport, /*writeFrame=*/false);
    Tick(harness, *viewport, /*writeFrame=*/false);
    EXPECT_TRUE(viewport->IsOutputShown()) << "a subject switch never empties a shown output";
    EXPECT_TRUE(ImageVisible(*viewport));

    Tick(harness, *viewport, /*writeFrame=*/true);
    EXPECT_TRUE(viewport->IsOutputShown());
    EXPECT_TRUE(ImageVisible(*viewport));
    Rendering::Utils::SetShaderPathResolver(previousResolver);
}

TEST_F(StagingViewportTest, AHiddenHostHoldsNoViewAndAShownHostShowsOnlyTheNewViewsFrame)
{
    StagingViewport* viewport = m_Left;
    UiRgHarness harness(m_Fixture.Device.get());
    const Rendering::Utils::ShaderPathResolverFunc previousResolver = Rendering::Utils::GetShaderPathResolver();
    Rendering::Utils::SetShaderPathResolver(&StagedShaderPath);
    Tick(harness, *viewport, /*writeFrame=*/true);
    const std::string shownTexture = viewport->View().UiTextureName();

    viewport->SetHostVisible(false);
    EXPECT_FALSE(viewport->View().IsAlive()) << "a hidden host returns its view to the pool";
    EXPECT_FALSE(viewport->IsOutputShown());
    // The released entry goes to this view, so a shown host cannot take it back.
    const Engine::Renderer::StagingView other = m_Pool->Acquire(StagingViewDesc{64, 64, "Staging.Other"});
    EXPECT_EQ(other.UiTextureName(), shownTexture) << "the hidden host released its entry";
    Tick(harness, *viewport, /*writeFrame=*/false);

    viewport->SetHostVisible(true);
    ASSERT_TRUE(viewport->View().IsAlive()) << "a shown host acquires a view again";
    EXPECT_NE(viewport->View().UiTextureName(), shownTexture);
    EXPECT_FALSE(viewport->IsOutputShown()) << "the new view shows no frame before it renders one";
    viewport->ShowAfterNextFrame();
    Tick(harness, *viewport, /*writeFrame=*/false);
    EXPECT_FALSE(ImageVisible(*viewport));
    Tick(harness, *viewport, /*writeFrame=*/true);
    EXPECT_TRUE(viewport->IsOutputShown()) << "the new view's first frame shows the output";
    EXPECT_EQ(ImageTexture(*viewport), viewport->View().UiTextureName());
    Rendering::Utils::SetShaderPathResolver(previousResolver);
}
} // namespace
