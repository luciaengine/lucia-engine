#include <gtest/gtest.h>

#include <filesystem>
#include <functional>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "InspectorRegistry.h"
#include "Inspectors/RenderPipeline/RenderPipelineInspector.h"

#include "AssetCore/AssetEvents.h"
#include "Assets/AssetManager.h"
#include "Assets/RenderPipelineAsset.h"
#include "Core/Engine.h"
#include "Engine/Rendering/Pipeline/Nodes/ShadowMapNode.h"
#include "UI/Controls/Dropdown.h"
#include "UI/Controls/FloatField.h"
#include "UI/Controls/Toggle.h"
#include "UI/UIElement.h"
#include "UI/UIManager.h"
#include "UndoRedo/UndoRedoService.h"

#include <nlohmann/json.hpp>

#include "TestTempDir.h"

using namespace GameEngine;

namespace
{
static void WriteTextFile(const std::filesystem::path& p, const std::string& text)
{
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary);
    ASSERT_TRUE(out.is_open()) << p.string();
    out << text;
}

static std::string ReadTextFile(const std::filesystem::path& p)
{
    std::ifstream in(p, std::ios::binary);
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}

static std::string MakeShadowMapPipeline(const std::string& shadowPassFields)
{
    return R"JSON({ "schemaVersion": 2, "pipelineName": "ShadowPipe", "passes": [ { "id": "Shadows", "type": "ShadowMap")JSON" +
           shadowPassFields + R"JSON( } ], "outputs": { "FinalColor": "View.Resolve" } })JSON";
}

// The inspector reads the engine's asset manager, so the suite starts the engine once.
class RenderPipelineInspectorTest : public ::testing::Test
{
  protected:
    static void SetUpTestSuite()
    {
        EngineCore& engine = EngineCore::GetInstance();
        if (engine.IsInitialized())
            return;
        ApplicationConfig config{};
        config.AssetDirectory = ".";
        config.WorkspaceDirectory = ".";
        config.EnableEditor = true;
        ASSERT_TRUE(engine.Initialize(config));
    }
};

// The inspector of one pipeline, mounted in a UI manager. An edit runs inside the dispatch guard and
// the rebuild it posts runs after, as in the editor.
struct MountedPipelineInspector
{
    explicit MountedPipelineInspector(RenderPipelineAsset& pipe, Editor::UndoRedoService* undo = nullptr)
    {
        auto root = std::make_unique<UIElement>("div");
        Root = root.get();
        Manager.SetRoot(std::move(root));
        RegisterRenderPipelineInspector();
        InspectorFn* fn = InspectorRegistry::Get().TryGetAssetInspector(AssetType::RenderPipeline);
        EXPECT_NE(fn, nullptr);
        InspectorContext ctx{};
        ctx.Parent = Root;
        ctx.Object = &pipe;
        ctx.AssetReloadedCallbacks = &ReloadCallbacks;
        ctx.Undo = undo;
        if (fn)
            (*fn)(ctx);
    }

    template <typename Edit>
    void EditAsUser(Edit&& edit)
    {
        UIElement::SetInEventDispatch(true);
        edit();
        UIElement::SetInEventDispatch(false);
        Manager.DrainDeferredActionsOnce();
    }

    // True when every reload callback keeps the tree, as the Inspector panel asks after a reload.
    bool KeepsTreeAfterReload() const
    {
        if (ReloadCallbacks.empty())
            return false;
        for (const auto& callback : ReloadCallbacks)
            if (!callback())
                return false;
        return true;
    }

    UIManager Manager{nullptr};
    UIElement* Root = nullptr;
    std::vector<std::function<bool()>> ReloadCallbacks;
};
} // namespace

TEST_F(RenderPipelineInspectorTest, BuildsUiForComputeAndFullscreenPasses)
{
    RegisterRenderPipelineInspector();
    InspectorFn* fn = InspectorRegistry::Get().TryGetAssetInspector(AssetType::RenderPipeline);
    ASSERT_NE(fn, nullptr);

    const TestUtils::ScopedTempDir tmpDir{TestUtils::MakeUniqueTempDirectory("ge_editor_tests")};
    const std::filesystem::path pipePath = tmpDir.Path() / "TestPipe.rendergraph";

    const std::string json = R"JSON(
{
  "schemaVersion": 2,
  "pipelineName": "TestPipe",
  "resources": {
    "ClusterParams": {
      "kind": "Buffer",
      "scope": "PerView",
      "memoryUsage": "Upload",
      "usage": ["ConstantBuffer"],
      "flags": ["PersistentlyMapped"],
      "size": {
        "bytes": 16,
        "expression": "clustersX * depthSlices * 4",
        "variables": {
          "clustersX": "ceil(renderWidth / 16)",
          "depthSlices": 24
        }
      }
    },
    "ClusterDebug": {
      "kind": "Texture",
      "scope": "PerView",
      "format": "RGBA8_UNORM",
      "usage": ["RenderTarget", "ShaderResource"],
      "extent": { "scale": [0.5, 0.5] }
    }
  },
  "passes": [
    {
      "id": "Cull",
      "type": "ComputeShader",
      "enabled": true,
      "shaderPkg": "",
      "dispatch": { "x": "ceil(renderWidth / 16)", "y": "ceil(renderHeight / 16)", "z": 1 },
      "inputs": { "DepthTex": "View.Depth" },
      "inputUsages": { "DepthTex": "DepthRead" },
      "buffers": { "ClusterParams": "ClusterParams" },
      "bufferUsages": { "ClusterParams": "Sampled" }
    },
    {
      "id": "Overlay",
      "type": "FullscreenShader",
      "enabled": true,
      "shaderPkg": "",
      "alphaBlend": true,
      "output": "View.Resolve",
      "inputs": { "ColorIn": "View.Color" },
      "buffers": { "ClusterParams": "ClusterParams" },
      "bufferUsages": { "ClusterParams": "StorageRead" }
    }
  ],
  "outputs": { "FinalColor": "View.Resolve" }
}
)JSON";

    WriteTextFile(pipePath, json);

    RenderPipelineAsset pipe(GUID::Generate(), pipePath);
    ASSERT_TRUE(pipe.Load());
    ASSERT_TRUE(pipe.GetErrors().empty());

    UIElement root("div");
    InspectorContext ctx{};
    ctx.Parent = &root;
    ctx.Object = &pipe;

    // Must not crash.
    (*fn)(ctx);

    // Basic sanity: inspector added content.
    EXPECT_GT(root.GetChildren().size(), 0u);

    // Verify a few key controls exist (IDs are set in the inspector implementation).
    EXPECT_NE(root.FindById("rp-pass-0-type"_sid), nullptr);
    EXPECT_NE(root.FindById("rp-pass-0-dispatch-x"_sid), nullptr);
    EXPECT_NE(root.FindById("rp-pass-0-inputs-add"_sid), nullptr);
    EXPECT_NE(root.FindById("rp-pass-1-type"_sid), nullptr);
    EXPECT_NE(root.FindById("rp-pass-1-alphaBlend"_sid), nullptr);
    EXPECT_NE(root.FindById("rp-pass-1-output"_sid), nullptr);

    // Verify resource editor controls exist.
    EXPECT_NE(root.FindById("rp-res-add"_sid), nullptr);
    EXPECT_NE(root.FindById("rp-res-ClusterParams-kind"_sid), nullptr);
    EXPECT_NE(root.FindById("rp-res-ClusterParams-scope"_sid), nullptr);
    EXPECT_NE(root.FindById("rp-res-ClusterParams-size-bytes"_sid), nullptr);
    EXPECT_NE(root.FindById("rp-res-ClusterParams-memoryUsage"_sid), nullptr);
    EXPECT_NE(root.FindById("rp-res-ClusterParams-usage-add"_sid), nullptr);
    EXPECT_NE(root.FindById("rp-res-ClusterParams-flags-add"_sid), nullptr);
    EXPECT_NE(root.FindById("rp-res-ClusterParams-size-vars-add"_sid), nullptr);
    EXPECT_NE(root.FindById("rp-res-ClusterParams-size-var-clustersX-key"_sid), nullptr);
    EXPECT_NE(root.FindById("rp-res-ClusterParams-size-var-clustersX-type"_sid), nullptr);
    EXPECT_NE(root.FindById("rp-res-ClusterParams-size-var-clustersX-string"_sid), nullptr);
    EXPECT_NE(root.FindById("rp-res-ClusterParams-size-var-depthSlices-number"_sid), nullptr);

    EXPECT_NE(root.FindById("rp-res-ClusterDebug-kind"_sid), nullptr);
    EXPECT_NE(root.FindById("rp-res-ClusterDebug-format"_sid), nullptr);
    EXPECT_NE(root.FindById("rp-res-ClusterDebug-usage-add"_sid), nullptr);
    EXPECT_NE(root.FindById("rp-res-ClusterDebug-extent-mode"_sid), nullptr);
    EXPECT_NE(root.FindById("rp-res-ClusterDebug-extent-scale-x"_sid), nullptr);
    EXPECT_NE(root.FindById("rp-res-ClusterDebug-extent-scale-y"_sid), nullptr);
}


// A ShadowMap pass shows its cascade rows. A resolution chosen in the dropdown is saved to the
// pipeline file, and the saved pass is one the node accepts.
TEST_F(RenderPipelineInspectorTest, ShadowMapPassEditsTheCascadeResolutionInThePipelineFile)
{
    const TestUtils::ScopedTempDir tmpDir{TestUtils::MakeUniqueTempDirectory("ge_editor_tests")};
    const std::filesystem::path pipePath = tmpDir.Path() / "ShadowPipe.rendergraph";
    WriteTextFile(pipePath, MakeShadowMapPipeline(R"(, "resolution": 2048)"));
    RenderPipelineAsset pipe(GUID::Generate(), pipePath);
    ASSERT_TRUE(pipe.Load());

    MountedPipelineInspector inspector(pipe);
    UIElement& root = *inspector.Root;

    auto* resolution = dynamic_cast<Dropdown*>(root.FindById("rp-pass-0-resolution"_sid));
    ASSERT_NE(resolution, nullptr);
    EXPECT_EQ(resolution->GetSelectedValue(), "2048");
    EXPECT_EQ(resolution->GetSelectedLabel(), "2048");
    EXPECT_NE(dynamic_cast<FloatField*>(root.FindById("rp-pass-0-splitLambda"_sid)), nullptr);
    EXPECT_NE(dynamic_cast<Toggle*>(root.FindById("rp-pass-0-fitShadowDistanceToScene"_sid)), nullptr);
    EXPECT_NE(dynamic_cast<FloatField*>(root.FindById("rp-pass-0-maxShadowDistance"_sid)), nullptr);

    inspector.EditAsUser([resolution] { resolution->SetSelectedValue("4096"); });

    const nlohmann::json saved = nlohmann::json::parse(ReadTextFile(pipePath));
    const nlohmann::json& pass = saved["passes"][0];
    EXPECT_EQ(pass.value("resolution", 0u), 4096u);
    Engine::Renderer::Pipeline::Nodes::ShadowMapNode node;
    std::string error;
    EXPECT_TRUE(node.Initialize("Shadows", pass.dump(), &error)) << error;

    // The save rebuilds the inspector, so the row is found again.
    auto* splitLambda = dynamic_cast<FloatField*>(root.FindById("rp-pass-0-splitLambda"_sid));
    ASSERT_NE(splitLambda, nullptr);
    inspector.EditAsUser([splitLambda]
                         {
                             splitLambda->SetValue(0.2f);
                             splitLambda->NotifyValueChanged();
                         });
    const nlohmann::json resaved = nlohmann::json::parse(ReadTextFile(pipePath));
    EXPECT_EQ(resaved["passes"][0].value("splitLambda", 0.0), 0.2) << "a float saves as typed, not as its float expansion";
}

// A cascade count chosen in the dropdown is saved to the pipeline file, and the saved pass is one the
// node accepts. A count the node refuses stays selected and marked.
TEST_F(RenderPipelineInspectorTest, ShadowMapPassEditsTheCascadeCountInThePipelineFile)
{
    const TestUtils::ScopedTempDir tmpDir{TestUtils::MakeUniqueTempDirectory("ge_editor_tests")};
    const std::filesystem::path pipePath = tmpDir.Path() / "ShadowPipe.rendergraph";
    WriteTextFile(pipePath, MakeShadowMapPipeline(R"(, "cascades": 4)"));
    RenderPipelineAsset pipe(GUID::Generate(), pipePath);
    ASSERT_TRUE(pipe.Load());

    MountedPipelineInspector inspector(pipe);
    UIElement& root = *inspector.Root;

    auto* cascades = dynamic_cast<Dropdown*>(root.FindById("rp-pass-0-cascades"_sid));
    ASSERT_NE(cascades, nullptr);
    EXPECT_EQ(cascades->GetSelectedValue(), "4");

    inspector.EditAsUser([cascades] { cascades->SetSelectedValue("2"); });

    const nlohmann::json pass = nlohmann::json::parse(ReadTextFile(pipePath))["passes"][0];
    EXPECT_EQ(pass.value("cascades", 0u), 2u);
    Engine::Renderer::Pipeline::Nodes::ShadowMapNode node;
    std::string error;
    EXPECT_TRUE(node.Initialize("Shadows", pass.dump(), &error)) << error;

    const std::filesystem::path invalidPath = tmpDir.Path() / "InvalidShadowPipe.rendergraph";
    WriteTextFile(invalidPath, MakeShadowMapPipeline(R"(, "cascades": 0)"));
    RenderPipelineAsset invalidPipe(GUID::Generate(), invalidPath);
    ASSERT_TRUE(invalidPipe.Load());
    MountedPipelineInspector invalid(invalidPipe);
    auto* invalidCascades = dynamic_cast<Dropdown*>(invalid.Root->FindById("rp-pass-0-cascades"_sid));
    ASSERT_NE(invalidCascades, nullptr);
    EXPECT_EQ(invalidCascades->GetSelectedValue(), "0");
    EXPECT_EQ(invalidCascades->GetSelectedLabel(), "0 (invalid)");
}

// An edit of one pass rebuilds that pass and leaves every other row of the tree in place. The save
// changes only the edited key: building the tree adds no key to a pass.
TEST_F(RenderPipelineInspectorTest, AnEditOfAPassRebuildsOnlyThatPass)
{
    const TestUtils::ScopedTempDir tmpDir{TestUtils::MakeUniqueTempDirectory("ge_editor_tests")};
    const std::filesystem::path pipePath = tmpDir.Path() / "TwoPasses.rendergraph";
    WriteTextFile(pipePath, R"JSON({ "schemaVersion": 2, "pipelineName": "TwoPasses", "passes": [
        { "id": "Compute", "type": "ComputeShader", "shaderPkg": "Shaders/none.shaderpkg" },
        { "id": "Shadows", "type": "ShadowMap", "resolution": 2048 } ],
        "outputs": { "FinalColor": "View.Resolve" } })JSON");
    RenderPipelineAsset pipe(GUID::Generate(), pipePath);
    ASSERT_TRUE(pipe.Load());
    const nlohmann::json computeBefore = nlohmann::json::parse(ReadTextFile(pipePath))["passes"][0];

    MountedPipelineInspector inspector(pipe);
    UIElement& root = *inspector.Root;
    UIElement* computeId = root.FindById("rp-pass-0-id"_sid);
    UIElement* pipelineName = root.FindById("rp-foldout-settings"_sid);
    auto* resolution = dynamic_cast<Dropdown*>(root.FindById("rp-pass-1-resolution"_sid));
    ASSERT_NE(computeId, nullptr);
    ASSERT_NE(pipelineName, nullptr);
    ASSERT_NE(resolution, nullptr);
    // A rebuilt element does not keep this class, whatever address it gets.
    computeId->AddClass("test-built-before-edit");
    pipelineName->AddClass("test-built-before-edit");
    resolution->AddClass("test-built-before-edit");

    inspector.EditAsUser([resolution] { resolution->SetSelectedValue("4096"); });

    EXPECT_TRUE(root.FindById("rp-pass-0-id"_sid)->HasClass("test-built-before-edit")) << "the other pass was rebuilt";
    EXPECT_TRUE(root.FindById("rp-foldout-settings"_sid)->HasClass("test-built-before-edit")) << "the tree was rebuilt";
    auto* rebuilt = dynamic_cast<Dropdown*>(root.FindById("rp-pass-1-resolution"_sid));
    ASSERT_NE(rebuilt, nullptr);
    EXPECT_FALSE(rebuilt->HasClass("test-built-before-edit")) << "the edited pass was not rebuilt";
    EXPECT_EQ(rebuilt->GetSelectedValue(), "4096");

    const nlohmann::json saved = nlohmann::json::parse(ReadTextFile(pipePath));
    EXPECT_EQ(saved["passes"][1].value("resolution", 0u), 4096u);
    EXPECT_EQ(saved["passes"][0], computeBefore) << "the save added keys to a pass it did not edit";
}

// The reload that follows a save of the Inspector keeps the tree. A reload of other text, as after
// an undo or an edit outside the editor, asks the panel for a rebuild.
TEST_F(RenderPipelineInspectorTest, OnlyAReloadOfOtherTextRebuildsTheTree)
{
    const TestUtils::ScopedTempDir tmpDir{TestUtils::MakeUniqueTempDirectory("ge_editor_tests")};
    const std::filesystem::path pipePath = tmpDir.Path() / "ShadowPipe.rendergraph";
    WriteTextFile(pipePath, MakeShadowMapPipeline(R"(, "resolution": 2048)"));
    RenderPipelineAsset pipe(GUID::Generate(), pipePath);
    ASSERT_TRUE(pipe.Load());

    MountedPipelineInspector inspector(pipe);
    auto* resolution = dynamic_cast<Dropdown*>(inspector.Root->FindById("rp-pass-0-resolution"_sid));
    ASSERT_NE(resolution, nullptr);
    EXPECT_TRUE(inspector.KeepsTreeAfterReload());

    inspector.EditAsUser([resolution] { resolution->SetSelectedValue("4096"); });
    ASSERT_EQ(pipe.Reload(), ReloadOutcome::Reloaded);
    EXPECT_TRUE(inspector.KeepsTreeAfterReload()) << "the reload of the saved file rebuilds the tree";

    WriteTextFile(pipePath, MakeShadowMapPipeline(R"(, "resolution": 1024)"));
    ASSERT_EQ(pipe.Reload(), ReloadOutcome::Reloaded);
    EXPECT_FALSE(inspector.KeepsTreeAfterReload()) << "the tree keeps showing a document that is not loaded";
}

// An undo and a redo announce the reload of the file they write, so the Inspector panel rebuilds the
// tree at once and does not wait for the file watcher to see the write.
TEST_F(RenderPipelineInspectorTest, AnUndoAndARedoAnnounceTheReloadOfThePipeline)
{
    const TestUtils::ScopedTempDir tmpDir{TestUtils::MakeUniqueTempDirectory("ge_editor_tests")};
    const std::filesystem::path pipePath = tmpDir.Path() / "ShadowPipe.rendergraph";
    WriteTextFile(pipePath, MakeShadowMapPipeline(R"(, "resolution": 2048)"));
    RenderPipelineAsset pipe(GUID::Generate(), pipePath);
    ASSERT_TRUE(pipe.Load());

    int reloadEvents = 0;
    AssetEventDispatcher& events = EngineCore::GetInstance().GetAssetManager().GetEventDispatcher();
    const auto callback = events.AddCallback(
        [&reloadEvents, guid = pipe.GetGUID()](const AssetEvent& event)
        { reloadEvents += event.EventType == AssetEventType::AssetReloaded && event.AssetGuid == guid ? 1 : 0; });

    Editor::UndoRedoService undo;
    MountedPipelineInspector inspector(pipe, &undo);
    auto* resolution = dynamic_cast<Dropdown*>(inspector.Root->FindById("rp-pass-0-resolution"_sid));
    ASSERT_NE(resolution, nullptr);
    inspector.EditAsUser([resolution] { resolution->SetSelectedValue("4096"); });
    ASSERT_TRUE(undo.CanUndo());
    EXPECT_EQ(reloadEvents, 0) << "the edit's own reload keeps the tree and needs no event";

    undo.Undo();
    EXPECT_EQ(reloadEvents, 1);
    EXPECT_FALSE(inspector.KeepsTreeAfterReload()) << "the tree still shows the edit that the undo removed";

    undo.Redo();
    EXPECT_EQ(reloadEvents, 2);
    events.RemoveCallback(callback);
}

// A save that cannot write the file keeps the fields of the loaded pass. The tree does not show an
// edit that the file does not hold, and the next reload keeps the tree.
TEST_F(RenderPipelineInspectorTest, AFailedSaveKeepsTheLoadedPassFields)
{
    const TestUtils::ScopedTempDir tmpDir{TestUtils::MakeUniqueTempDirectory("ge_editor_tests")};
    const std::filesystem::path pipePath = tmpDir.Path() / "ShadowPipe.rendergraph";
    WriteTextFile(pipePath, MakeShadowMapPipeline(R"(, "resolution": 2048)"));
    RenderPipelineAsset pipe(GUID::Generate(), pipePath);
    ASSERT_TRUE(pipe.Load());
    MountedPipelineInspector inspector(pipe);

    // A directory at the file path makes the write fail.
    std::filesystem::rename(pipePath, tmpDir.Path() / "before.rendergraph");
    ASSERT_TRUE(std::filesystem::create_directory(pipePath));
    auto* resolution = dynamic_cast<Dropdown*>(inspector.Root->FindById("rp-pass-0-resolution"_sid));
    ASSERT_NE(resolution, nullptr);
    inspector.EditAsUser([resolution] { resolution->SetSelectedValue("4096"); });

    resolution = dynamic_cast<Dropdown*>(inspector.Root->FindById("rp-pass-0-resolution"_sid));
    ASSERT_NE(resolution, nullptr);
    EXPECT_EQ(resolution->GetSelectedValue(), "2048");
    EXPECT_TRUE(inspector.KeepsTreeAfterReload());
}

// A resolution the node refuses stays selected and marked, so opening the inspector does not
// change the file.
TEST_F(RenderPipelineInspectorTest, ShadowMapPassKeepsAnUnsupportedResolutionSelectedAndMarked)
{
    const TestUtils::ScopedTempDir tmpDir{TestUtils::MakeUniqueTempDirectory("ge_editor_tests")};
    const std::filesystem::path pipePath = tmpDir.Path() / "ShadowPipe.rendergraph";
    WriteTextFile(pipePath, MakeShadowMapPipeline(R"(, "resolution": 5000, "fitShadowDistanceToScene": true)"));
    RenderPipelineAsset pipe(GUID::Generate(), pipePath);
    ASSERT_TRUE(pipe.Load());

    MountedPipelineInspector inspector(pipe);
    UIElement& root = *inspector.Root;

    auto* resolution = dynamic_cast<Dropdown*>(root.FindById("rp-pass-0-resolution"_sid));
    ASSERT_NE(resolution, nullptr);
    EXPECT_EQ(resolution->GetSelectedValue(), "5000");
    EXPECT_EQ(resolution->GetSelectedLabel(), "5000 (invalid)");
    EXPECT_EQ(root.FindById("rp-pass-0-maxShadowDistance"_sid), nullptr)
        << "the scene fit replaces the max shadow distance";
}
