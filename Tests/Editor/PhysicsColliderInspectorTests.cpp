// The Physics Collider inspector's Physics Material row: it edits the collider's material asset with
// undo, and while a material is assigned the inline friction rows read as inactive and say why.
#include <gtest/gtest.h>

#include "InspectorRegistry.h"
#include "Inspectors/Physics/PhysicsColliderInspector.h"

#include "AssetCore/GUID.h"
#include "Core/Application.h"
#include "Core/Engine.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "EditorChangeNotifications.h"
#include "PhysicsECS/Components/PhysicsCollider.h"
#include "UI/AssetField.h"
#include "UI/Controls/InspectorNotice.h"
#include "UI/Controls/Label.h"
#include "UI/UIElement.h"
#include "UndoRedo/UndoRedoService.h"

#include <string>

using namespace GameEngine;
using Components::PhysicsCollider;

namespace
{

// The inspector row holding the label `text`, or null.
UIElement* RowOfLabel(UIElement& element, const std::string& text)
{
    if (const auto* label = dynamic_cast<const Label*>(&element); label && label->GetText() == text)
    {
        for (UIElement* up = label->GetParent(); up; up = up->GetParent())
            if (up->HasClass("inspector-row"))
                return up;
        return nullptr;
    }
    for (const auto& child : element.GetChildren())
        if (UIElement* row = RowOfLabel(*child, text))
            return row;
    return nullptr;
}

template <typename T>
T* FindFirst(UIElement& element)
{
    if (auto* found = dynamic_cast<T*>(&element))
        return found;
    for (const auto& child : element.GetChildren())
        if (T* found = FindFirst<T>(*child))
            return found;
    return nullptr;
}

class PhysicsColliderInspectorTest : public ::testing::Test
{
protected:
    // The Physics Material row is an asset picker, which reads the engine's asset registry.
    static void SetUpTestSuite()
    {
        EngineCore& engine = EngineCore::GetInstance();
        if (!engine.IsInitialized())
        {
            ApplicationConfig config{};
            config.AssetDirectory = ".";
            config.WorkspaceDirectory = ".";
            config.EnableEditor = true;
            ASSERT_TRUE(engine.Initialize(config));
        }
    }

    void Build(const PhysicsCollider& collider)
    {
        RegisterPhysicsColliderInspectors();
        entity = world.CreateEntity();
        world.AddComponentImmediate(entity, collider);
        InspectorFn* inspector = InspectorRegistry::Get().TryGetComponentInspector<PhysicsCollider>();
        ASSERT_NE(inspector, nullptr);
        InspectorContext context;
        context.Parent = &parent;
        context.World = &world;
        context.Entity = entity;
        context.Undo = &undo;
        context.ChangeNotifications = &notifications;
        (*inspector)(context);
    }

    AssetField* MaterialField()
    {
        UIElement* row = RowOfLabel(parent, "Physics Material");
        return row ? FindFirst<AssetField>(*row) : nullptr;
    }

    bool NoticeShown()
    {
        auto* notice = FindFirst<EditorUI::InspectorNotice>(parent);
        return notice && !notice->HasClass("hidden");
    }

    bool FrictionRowEnabled()
    {
        UIElement* row = RowOfLabel(parent, "Friction");
        return row && row->IsEnabled();
    }

    ECS::World world;
    ECS::EntityHandle entity;
    Editor::UndoRedoService undo;
    Editor::EditorChangeNotifications notifications;
    UIElement parent;
};

} // namespace

TEST_F(PhysicsColliderInspectorTest, WithoutMaterialTheInlineRowsApply)
{
    Build(PhysicsCollider{});

    ASSERT_NE(MaterialField(), nullptr) << "the collider has no Physics Material row";
    EXPECT_TRUE(MaterialField()->GetValue().IsNull());
    EXPECT_FALSE(NoticeShown());
    EXPECT_TRUE(FrictionRowEnabled());
}

TEST_F(PhysicsColliderInspectorTest, AssignedMaterialTakesPrecedenceOverTheInlineRows)
{
    const GUID material = GUID::Generate();
    PhysicsCollider collider{};
    collider.materialAsset.Set(material);
    Build(collider);

    ASSERT_NE(MaterialField(), nullptr);
    EXPECT_EQ(MaterialField()->GetValue(), material);
    EXPECT_TRUE(NoticeShown());
    EXPECT_FALSE(FrictionRowEnabled());
}

// Clearing the row writes the component, re-enables the inline rows, and is one undo step that
// puts the material back.
TEST_F(PhysicsColliderInspectorTest, ClearingTheMaterialIsUndoable)
{
    const GUID material = GUID::Generate();
    PhysicsCollider collider{};
    collider.materialAsset.Set(material);
    Build(collider);
    ASSERT_NE(MaterialField(), nullptr);

    MaterialField()->Clear();

    EXPECT_TRUE(world.GetComponent<PhysicsCollider>(entity)->materialAsset.IsNull());
    EXPECT_FALSE(NoticeShown());
    EXPECT_TRUE(FrictionRowEnabled());
    ASSERT_EQ(undo.GetUndoCount(), 1u);
    EXPECT_STREQ(undo.PeekUndoName(), "Physics Collider Material");

    undo.Undo();
    EXPECT_EQ(world.GetComponent<PhysicsCollider>(entity)->materialAsset.ToGuid(), material);
}
