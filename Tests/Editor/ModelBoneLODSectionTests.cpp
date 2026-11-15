#include "Inspectors/BoneLOD/ModelBoneLODSection.h"
#include "Assets/AssetManager.h"
#include "Assets/BoneLOD/BoneLODAssetSettings.h"
#include "Assets/ModelAsset.h"
#include "Core/Application.h"
#include "Core/Engine.h"
#include "Scripting/ScriptsConfig.h"
#include "TestTempDir.h"
#include "ECSModules/Rendering/SkeletonStore.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/IntField.h"
#include "UI/Parsers/CSSParser.h"
#include "UI/Registration/ElementRegistration.h"
#include "UI/StyleProperties.h"
#include "UI/UIManager.h"
#include "UIRgTestHarness.h"
#include "UndoRedo/UndoRedoService.h"

#include <fstream>
#include <sstream>
#include <vector>

#include <gtest/gtest.h>

using namespace GameEngine;

namespace
{
IntField* FindBudget(UIElement& parent)
{
    if (auto* field = dynamic_cast<IntField*>(&parent))
        return field;
    for (const auto& child : parent.GetChildren())
    {
        if (auto* field = FindBudget(*child))
            return field;
    }
    return nullptr;
}

// The inspector's default width and the narrowest bone name area that still reads as a name.
constexpr uint32 kInspectorWidthPx = 189u;
constexpr uint32 kInspectorHeightPx = 1200u;
constexpr float kMinimumNameWidthPx = 40.0f;
constexpr float kLayoutTolerancePx = 0.5f;
constexpr uint32 kChainBoneCount = 8u;
constexpr int kSettleFrames = 3;
constexpr float kFrameSeconds = 1.0f / 60.0f;

// The theme sheets in theme.css import order, then the section's own sheet.
constexpr const char* kSheets[] = {
    "Assets/UI/theme/tokens.css",
    "Assets/UI/theme/core.css",
    "Assets/UI/theme/views.css",
    "Assets/UI/theme/widgets.css",
    "Assets/UI/inspectors/BoneLOD/ModelBoneLODSection.css",
};

std::string ReadEditorFile(const std::string& relativePath)
{
    std::ifstream in(std::filesystem::path(GE_EDITOR_SOURCE_DIR) / relativePath, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

// One bone per level, so the deepest row carries the largest indent.
Animation::SkeletonData MakeChainSkeleton()
{
    Animation::SkeletonData skeleton;
    skeleton.BoneCount = kChainBoneCount;
    skeleton.RestLocalMatrix.assign(kChainBoneCount * 16u, 0.0f);
    skeleton.InverseBind.assign(kChainBoneCount * 16u, 0.0f);
    for (uint32 node = 0u; node < kChainBoneCount; ++node)
    {
        skeleton.Parent.push_back(static_cast<int32>(node) - 1);
        skeleton.BoneNames.push_back("SpineSegment" + std::to_string(node));
        for (uint32 diagonal = 0u; diagonal < 4u; ++diagonal)
        {
            skeleton.RestLocalMatrix[node * 16u + diagonal * 5u] = 1.0f;
            skeleton.InverseBind[node * 16u + diagonal * 5u] = 1.0f;
        }
    }
    skeleton.ComputeTopologicalSort();
    return skeleton;
}

UIElement* ChildWithClass(UIElement& parent, const char* className)
{
    for (const auto& child : parent.GetChildren())
    {
        if (child->HasClass(className))
            return child.get();
    }
    return nullptr;
}

void CollectBoneRows(UIElement& element, std::vector<UIElement*>& rows)
{
    if (element.HasClass("tree-item") && ChildWithClass(element, "bone-lod-pin"))
        rows.push_back(&element);
    for (const auto& child : element.GetChildren())
        CollectBoneRows(*child, rows);
}

class ModelBoneLODSectionTest : public testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        Root = TestUtils::MakeUniqueTempDirectory("bone_lod_inspector");
        std::filesystem::create_directories(Root / "Writable");
        std::filesystem::create_directories(Root / "ReadOnly");
        auto& engine = EngineCore::GetInstance();
        if (!engine.IsInitialized())
        {
            ApplicationConfig config;
            config.WorkspaceDirectory = Root.string();
            config.AssetDirectory = (Root / "Assets").string();
            ScriptsConfig scripts;
            scripts.disableClr = true;
            scripts.enableAutoProjectGeneration = false;
            engine.SetScriptsConfig(scripts);
            ASSERT_TRUE(engine.Initialize(config));
        }
        AssetSourceDesc source;
        source.Alias = "bonelodinspector";
        source.Root = Root / "Writable";
        source.AuthoritativeDbFile = source.Root / "AssetDatabase.assetdb";
        ASSERT_TRUE(engine.GetAssetManager().RegisterSource(source));
        source.Alias = "bonelodreadonly";
        source.Root = Root / "ReadOnly";
        source.AuthoritativeDbFile.clear();
        ASSERT_TRUE(engine.GetAssetManager().RegisterSource(source));
        UIRegistration::RegisterBuiltInControls();
    }

    SharedPtr<ModelAsset> MakeModel(bool writable = true)
    {
        const auto path = Root / (writable ? "Writable" : "ReadOnly") /
            (std::string(testing::UnitTest::GetInstance()->current_test_info()->name()) + ".obj");
        std::ofstream(path) << "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
        auto& assets = EngineCore::GetInstance().GetAssetManager();
        const auto guid = assets.ResolveAssetGuid(path);
        return MakeShared<ModelAsset>(guid, path);
    }

    static std::filesystem::path Root;
};

std::filesystem::path ModelBoneLODSectionTest::Root;
}

TEST_F(ModelBoneLODSectionTest, BudgetUndoSurvivesSectionAndCallbackOwnerDestruction)
{
    auto model = MakeModel();
    uint32 refreshCount = 0u;
    auto callbackOwner = MakeUnique<UIElement>();
    Editor::UndoRedoService undo;
    auto section = MakeUnique<Editor::ModelBoneLODSection>(*model, &undo,
        [owner = UIElement::MakeWeakRef(callbackOwner.get()), &refreshCount]
        {
            if (owner.Get())
                ++refreshCount;
        });
    auto* field = FindBudget(*section);
    ASSERT_NE(field, nullptr);
    field->SetValue(12);
    field->NotifyValueChanged();
    auto& registry = EngineCore::GetInstance().GetAssetManager().GetRegistry();
    EXPECT_EQ(BoneLODAssetSettings::Load(registry, model->GetPath()).BoneBudget, 12u);
    EXPECT_EQ(undo.GetUndoCount(), 1u);
    EXPECT_EQ(refreshCount, 1u);
    field->NotifyValueChanged();
    EXPECT_EQ(undo.GetUndoCount(), 1u);
    section.reset();
    undo.Undo();
    const auto restored = BoneLODAssetSettings::Load(registry, model->GetPath());
    EXPECT_EQ(restored.BoneBudget, 32u);
    EXPECT_TRUE(restored.Error.empty());
    EXPECT_EQ(refreshCount, 2u);
    undo.Redo();
    EXPECT_EQ(BoneLODAssetSettings::Load(registry, model->GetPath()).BoneBudget, 12u);
    EXPECT_EQ(refreshCount, 3u);
    callbackOwner.reset();
    undo.Undo();
    EXPECT_EQ(BoneLODAssetSettings::Load(registry, model->GetPath()).BoneBudget, 32u);
    undo.Redo();
    EXPECT_EQ(BoneLODAssetSettings::Load(registry, model->GetPath()).BoneBudget, 12u);
    EXPECT_EQ(refreshCount, 3u);
}

TEST_F(ModelBoneLODSectionTest, InvalidSettingsResetCanUndoBackToTheAuthoredValue)
{
    auto model = MakeModel();
    auto& registry = EngineCore::GetInstance().GetAssetManager().GetRegistry();
    ASSERT_TRUE(registry.SetMetaValue(model->GetPath(), BoneLODAssetSettings::kMetadataKey, "{broken"));
    Editor::UndoRedoService undo;
    Editor::ModelBoneLODSection section(*model, &undo, {});
    auto* reset = dynamic_cast<Button*>(section.FindById("BoneLODReset"_sid));
    ASSERT_NE(reset, nullptr);
    ASSERT_TRUE(reset->IsEnabledInHierarchy());
    UIEvent event;
    event.Id = kEventButtonClick;
    event.Target = reset;
    event.CurrentTarget = reset;
    reset->DispatchEvent(event);
    EXPECT_TRUE(BoneLODAssetSettings::Load(registry, model->GetPath()).Error.empty());
    undo.Undo();
    std::string metadata;
    ASSERT_TRUE(registry.TryGetMetaValue(model->GetPath(), BoneLODAssetSettings::kMetadataKey, metadata));
    EXPECT_EQ(metadata, "{broken");
    EXPECT_FALSE(BoneLODAssetSettings::Load(registry, model->GetPath()).Error.empty());
}

TEST_F(ModelBoneLODSectionTest, ReadOnlySourceDisablesEditsAndCreatesNoUndoStep)
{
    auto model = MakeModel(false);
    Editor::UndoRedoService undo;
    Editor::ModelBoneLODSection section(*model, &undo, {});
    auto* field = FindBudget(section);
    ASSERT_NE(field, nullptr);
    EXPECT_FALSE(field->IsEnabledInHierarchy());
    field->SetValue(12);
    field->NotifyValueChanged();
    EXPECT_EQ(undo.GetUndoCount(), 0u);
    const auto& registry = EngineCore::GetInstance().GetAssetManager().GetRegistry();
    EXPECT_EQ(BoneLODAssetSettings::Load(registry, model->GetPath()).BoneBudget, 32u);
}

TEST_F(ModelBoneLODSectionTest, EveryBoneNameStaysReadableBeforeItsKeepBoxAtTheDefaultInspectorWidth)
{
    auto device = MakeHeadlessDevice();
    if (!device)
        GTEST_SKIP() << "no headless graphics device";
    auto& skeletons = Engine::Renderer::SkeletonStore::Instance();
    const uint32 skeletonId = skeletons.CreateSkeleton(kChainBoneCount);
    ASSERT_NE(skeletonId, 0u);
    *skeletons.Get(skeletonId) = MakeChainSkeleton();
    auto model = MakeModel();
    model->SetSkeletonIdForTest(skeletonId);

    auto panel = MakeUnique<UIElement>();
    panel->Style().Set(Style::Width, StyleLength::Px(static_cast<float>(kInspectorWidthPx)));
    auto ownedSection = MakeUnique<Editor::ModelBoneLODSection>(*model, nullptr, std::function<void()>{});
    UIElement* section = ownedSection.get();
    panel->AddChild(std::move(ownedSection));
    UIManager ui(device.get());
    ui.SetLayoutSizeOverride(kInspectorWidthPx, kInspectorHeightPx);
    for (const char* sheetPath : kSheets)
    {
        Stylesheet sheet{};
        ASSERT_TRUE(UIParsing::CSSParser::ParseStylesFromString(ReadEditorFile(sheetPath), sheet)) << sheetPath;
        sheet.SourceName = sheetPath;
        ui.AddStylesheet(MakeShared<const Stylesheet>(std::move(sheet)));
    }
    ui.SetRoot(std::move(panel));
    for (int frame = 0; frame < kSettleFrames; ++frame)
        ui.Update(kFrameSeconds, true);

    std::vector<UIElement*> rows;
    CollectBoneRows(*section, rows);
    ASSERT_EQ(rows.size(), kChainBoneCount) << "every bone, the root included, needs a row with a Keep box";
    for (UIElement* row : rows)
    {
        UIElement* title = ChildWithClass(*row, "tree-title");
        UIElement* pin = ChildWithClass(*row, "bone-lod-pin");
        ASSERT_NE(title, nullptr);
        const std::string name = title->GetTextContent();
        EXPECT_GE(pin->GetLayoutX() + kLayoutTolerancePx, title->GetLayoutX() + title->GetLayoutWidth())
            << name << ": the Keep box is not after the bone name";
        EXPECT_LE(pin->GetLayoutX() + pin->GetLayoutWidth(), row->GetLayoutX() + row->GetLayoutWidth() + kLayoutTolerancePx)
            << name << ": the Keep box runs past the row";
        EXPECT_GE(title->GetLayoutWidth(), kMinimumNameWidthPx) << name << ": the bone name has no room";
    }
}
