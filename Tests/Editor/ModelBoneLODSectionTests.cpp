#include "Inspectors/BoneLOD/ModelBoneLODSection.h"
#include "Assets/AssetManager.h"
#include "Assets/BoneLOD/BoneLODAssetSettings.h"
#include "Assets/ModelAsset.h"
#include "Core/Application.h"
#include "Core/Engine.h"
#include "Scripting/ScriptsConfig.h"
#include "TestTempDir.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/IntField.h"
#include "UI/Registration/ElementRegistration.h"
#include "UndoRedo/UndoRedoService.h"

#include <fstream>

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
    auto* reset = dynamic_cast<Button*>(section.FindById("BoneLODReset"));
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
