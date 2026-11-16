#include <gtest/gtest.h>

#include "Components/Rendering/MeshRenderer.h"
#include "Components/Rendering/RenderSorting.h"
#include "Components/Rendering/SkinnedMeshRenderer.h"
#include "Components/Transform.h"
#include "Components/Hierarchy.h"
#include "Editor/Hierarchy/HierarchyLayerControls.h"
#include "InspectorRegistry.h"
#include "Panels/HierarchyCommandIds.h"
#include "ECS/World.h"
#include "ECS/ECSTemplates.h"
#include "Engine/Rendering/SortingLayers.h"
#include "Rendering/Sorting/RenderSortingEditor.h"
#include "Rendering/Sorting/SortingLayerSettings.h"
#include "Editor/Settings/SettingsStore.h"
#include "TestTempDir.h"
#include "UI/Controls/Dropdown.h"
#include "UI/Controls/CollapsibleInfoCard.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/IntField.h"
#include "UI/Controls/MaskField.h"
#include "UI/UIElement.h"
#include "UI/StyleProperties.h"
#include "UndoRedo/UndoRedoService.h"

namespace GameEngine::Editor
{
namespace
{
using Sorting = Components::RenderSorting;

class RenderSortingEditorTests : public ::testing::Test
{
protected:
    ECS::World World;
    UndoRedoService Undo;
    std::filesystem::path ProjectRoot;

    void SetUp() override
    {
        ProjectRoot = TestUtils::MakeUniqueTempDirectory("RenderSortingEditorTests");
        std::filesystem::create_directories(ProjectRoot);
        RegisterRenderSortingComponent(ECS::GetComponentTypeId<Components::MeshRenderer>());
        RegisterRenderSortingComponent(ECS::GetComponentTypeId<Components::SkinnedMeshRenderer>());
        std::string error;
        ASSERT_TRUE(Rendering::SortingLayers::SetCurrent(Rendering::SortingLayers::Defaults(), error));
    }

    void TearDown() override
    {
        std::error_code error;
        std::filesystem::remove_all(ProjectRoot, error);
    }

    ECS::EntityHandle Mesh()
    {
        const auto entity = World.CreateEntity();
        World.AddComponentImmediate(entity, Components::Transform{});
        World.AddComponentImmediate(entity, Components::MeshRenderer{});
        return entity;
    }
};

TEST_F(RenderSortingEditorTests, MultiSelectionUndoRestoresMissingComponentAndExistingOrder)
{
    const auto first = Mesh();
    const auto second = Mesh();
    Sorting initial{};
    initial.Layer = 10;
    initial.OrderInLayer = 17;
    initial.GroupChildren = true;
    World.AddComponentImmediate(second, initial);
    ApplyRenderSortingLayer(World, {first, second}, 30, &Undo, nullptr);
    ASSERT_TRUE(World.HasComponent<Sorting>(first));
    ASSERT_TRUE(World.HasComponent<Sorting>(second));
    EXPECT_EQ(World.GetComponent<Sorting>(first)->Layer, 30);
    EXPECT_EQ(World.GetComponent<Sorting>(second)->Layer, 30);
    EXPECT_EQ(World.GetComponent<Sorting>(second)->OrderInLayer, 17);
    Undo.Undo();
    EXPECT_FALSE(World.HasComponent<Sorting>(first));
    ASSERT_TRUE(World.HasComponent<Sorting>(second));
    EXPECT_EQ(World.GetComponent<Sorting>(second)->Layer, 10);
    EXPECT_TRUE(World.GetComponent<Sorting>(second)->GroupChildren);
    EXPECT_FALSE(Undo.CanUndo());
    Undo.Redo();
    EXPECT_EQ(World.GetComponent<Sorting>(first)->Layer, 30);
    EXPECT_EQ(World.GetComponent<Sorting>(second)->Layer, 30);
}

TEST_F(RenderSortingEditorTests, DefaultAssignmentAndUnsupportedEntitiesDoNotCreateUndo)
{
    const auto mesh = Mesh();
    const auto empty = World.CreateEntity();
    World.AddComponentImmediate(empty, Components::Transform{});
    ApplyRenderSortingLayer(World, {mesh, empty}, 0, &Undo, nullptr);
    EXPECT_FALSE(World.HasComponent<Sorting>(mesh));
    EXPECT_FALSE(World.HasComponent<Sorting>(empty));
    EXPECT_FALSE(Undo.CanUndo());
    ApplyRenderSortingLayer(World, {empty}, 10, &Undo, nullptr);
    EXPECT_FALSE(Undo.CanUndo());
}

TEST_F(RenderSortingEditorTests, SkinnedMeshAndSortingOnlyParentAreEligible)
{
    const auto skinned = World.CreateEntity();
    World.AddComponentImmediate(skinned, Components::SkinnedMeshRenderer{});
    const auto parent = World.CreateEntity();
    Sorting group{};
    group.GroupChildren = true;
    World.AddComponentImmediate(parent, group);
    ApplyRenderSortingLayer(World, {skinned, parent}, 20, &Undo, nullptr);
    ASSERT_TRUE(World.HasComponent<Sorting>(skinned));
    ASSERT_TRUE(World.HasComponent<Sorting>(parent));
    EXPECT_EQ(World.GetComponent<Sorting>(skinned)->Layer, 20);
    EXPECT_EQ(World.GetComponent<Sorting>(parent)->Layer, 20);
    EXPECT_TRUE(World.GetComponent<Sorting>(parent)->GroupChildren);
}

template <typename Picker>
Picker* FindChildWithClass(UIElement& row, const char* className)
{
    for (const auto& child : row.GetChildren())
        if (child->HasClass(className))
            return dynamic_cast<Picker*>(child.get());
    return nullptr;
}

// The Hierarchy row's sorting layer picker: a pooled row picks for the entity it shows now, by
// stable layer id, in one undo step, and the column refreshes when the layers or a sorting edit change.
TEST_F(RenderSortingEditorTests, PooledSortingPickerUsesTheCurrentEntityAndTheLayerId)
{
    auto layers = Rendering::SortingLayers::Defaults();
    layers.Layers.push_back({1234, "Meadow Canopy", 99});
    std::string error;
    ASSERT_TRUE(Rendering::SortingLayers::SetCurrent(layers, error));
    const auto first = Mesh();
    const auto second = Mesh();
    UIElement row;
    HierarchyLayerControls controls;
    ASSERT_TRUE(controls.HandleCommand(HierarchyCommandIds::kCmdShowSortingLayer));
    controls.Present(World, first, row, 350.0f, [first]() { return std::vector{first}; }, &Undo, nullptr);
    controls.Present(World, second, row, 350.0f, [second]() { return std::vector{second}; }, &Undo, nullptr);
    auto* picker = FindChildWithClass<Dropdown>(row, "hierarchy-sorting-layer-dropdown");
    ASSERT_NE(picker, nullptr);
    EXPECT_TRUE(row.HasClass("has-sorting-layer"));
    EXPECT_TRUE(row.HasClass("render-layer-narrow")) << "below 400 px the pickers take their short form";
    EXPECT_FALSE(row.HasClass("has-render-layer"));

    picker->SetSelectedIndex(static_cast<int>(layers.Layers.size()) - 1);
    EXPECT_FALSE(World.HasComponent<Sorting>(first));
    ASSERT_TRUE(World.HasComponent<Sorting>(second));
    EXPECT_EQ(World.GetComponent<Sorting>(second)->Layer, 1234);
    EXPECT_EQ(picker->GetSelectedValue(), "1234");
    Undo.Undo();
    EXPECT_FALSE(World.HasComponent<Sorting>(second)) << "the undo restores the absent component";

    EXPECT_TRUE(controls.NeedsRefresh(World));
    EXPECT_FALSE(controls.NeedsRefresh(World));
    Sorting edited{};
    edited.Layer = 10;
    World.AddComponentImmediate(second, edited);
    EXPECT_TRUE(controls.NeedsRefresh(World));
    ASSERT_TRUE(Rendering::SortingLayers::SetCurrent(Rendering::SortingLayers::Defaults(), error));
    EXPECT_TRUE(controls.NeedsRefresh(World));
}

// Both columns on a skinned mesh: the render layer picker shows the layers of its MeshRenderer (a skinned
// model's entity carries one) beside the sorting picker, and a row that shows no entity hides both.
TEST_F(RenderSortingEditorTests, BothLayerColumnsShareASkinnedRowAndAGhostRowHidesThem)
{
    const auto entity = Mesh();
    World.AddComponentImmediate(entity, Components::SkinnedMeshRenderer{});
    auto renderer = *World.GetComponent<Components::MeshRenderer>(entity);
    renderer.RenderLayers.Bits = 0x8u;
    World.AddComponentImmediate(entity, renderer);
    UIElement row;
    HierarchyLayerControls controls;
    ASSERT_TRUE(controls.HandleCommand(HierarchyCommandIds::kCmdShowRenderLayer));
    ASSERT_TRUE(controls.HandleCommand(HierarchyCommandIds::kCmdShowSortingLayer));
    controls.Present(World, entity, row, 600.0f, [entity]() { return std::vector{entity}; }, &Undo, nullptr);
    auto* renderPicker = FindChildWithClass<MaskField>(row, "hierarchy-render-layer-dropdown");
    ASSERT_NE(renderPicker, nullptr);
    EXPECT_EQ(renderPicker->GetValue(), 0x8u);
    EXPECT_NE(FindChildWithClass<Dropdown>(row, "hierarchy-sorting-layer-dropdown"), nullptr);
    EXPECT_TRUE(row.HasClass("has-render-layer"));
    EXPECT_TRUE(row.HasClass("has-sorting-layer"));
    EXPECT_FALSE(row.HasClass("render-layer-narrow"));

    controls.Hide(row);
    EXPECT_FALSE(row.HasClass("has-render-layer"));
    EXPECT_FALSE(row.HasClass("has-sorting-layer"));
}

TEST_F(RenderSortingEditorTests, SavesCustomLayersWithoutChangingOtherProjectSettings)
{
    auto store = OpenProjectSettings(ProjectRoot);
    std::string error;
    ASSERT_TRUE(store.Load(&error));
    store.SetString("unrelated", "preserved");
    ASSERT_TRUE(store.Save(&error));
    auto layers = Rendering::SortingLayers::Defaults();
    layers.Layers.push_back({765, "Canopy", 97});
    ASSERT_TRUE(SaveSortingLayerSettings(ProjectRoot, layers, error)) << error;
    EXPECT_EQ(Rendering::SortingLayers::Load(ProjectRoot).Layers, layers.Layers);
    EXPECT_EQ(Rendering::SortingLayers::Current().Layers, layers.Layers);
    ASSERT_TRUE(store.Load(&error));
    std::string unrelated;
    ASSERT_TRUE(store.TryGetString("unrelated", unrelated));
    EXPECT_EQ(unrelated, "preserved");
    layers.Layers.back().Name = "Default";
    EXPECT_FALSE(SaveSortingLayerSettings(ProjectRoot, layers, error));
    EXPECT_FALSE(error.empty());
    EXPECT_EQ(Rendering::SortingLayers::Load(ProjectRoot).Layers.back().Name, "Canopy");
    EXPECT_EQ(Rendering::SortingLayers::Current().Layers.back().Name, "Canopy");
}

TEST_F(RenderSortingEditorTests, LoadingAnotherProjectClearsOutgoingCustomLayers)
{
    auto layers = Rendering::SortingLayers::Defaults();
    layers.Layers.push_back({765, "Canopy", 97});
    std::string error;
    ASSERT_TRUE(SaveSortingLayerSettings(ProjectRoot, layers, error));
    const auto other = ProjectRoot / "OtherProject";
    std::filesystem::create_directories(other);
    auto store = OpenProjectSettings(other);
    ASSERT_TRUE(store.Load(&error));
    ASSERT_TRUE(Rendering::SortingLayers::SetCurrent(Rendering::SortingLayers::FromJson(store.Json()), error));
    EXPECT_EQ(Rendering::SortingLayers::Current().Layers, Rendering::SortingLayers::Defaults().Layers);
}

TEST_F(RenderSortingEditorTests, RemovingLayerReservesItsIdForOldSceneReferences)
{
    auto layers = Rendering::SortingLayers::Defaults();
    layers.Layers.push_back({765, "Canopy", 97});
    std::string error;
    ASSERT_TRUE(SaveSortingLayerSettings(ProjectRoot, layers, error));
    layers.Layers.pop_back();
    ASSERT_TRUE(SaveSortingLayerSettings(ProjectRoot, layers, error));
    auto store = OpenProjectSettings(ProjectRoot);
    ASSERT_TRUE(store.Load(&error));
    int64_t nextId = 0;
    ASSERT_TRUE(store.TryGetInt64("sortingLayerNextId", nextId));
    EXPECT_EQ(nextId, 766);
}

TEST_F(RenderSortingEditorTests, DiagnosticReportsEffectiveParentOrderAndRefreshes)
{
    const auto parent = Mesh();
    const auto child = Mesh();
    Sorting group{};
    group.Layer = 30;
    group.OrderInLayer = 7;
    group.GroupChildren = true;
    World.AddComponentImmediate(parent, group);
    World.AddComponentImmediate(child, Components::Parent{parent});
    UIElement content;
    std::vector<std::function<void()>> refresh;
    InspectorContext context;
    context.World = &World;
    context.Entity = child;
    context.Parent = &content;
    context.SimulationRefreshCallbacks = &refresh;
    AddRenderSortingDiagnostic(context);
    ASSERT_EQ(content.GetChildren().size(), 1u);
    const auto* card = dynamic_cast<EditorUI::CollapsibleInfoCard*>(content.GetChildren().front().get());
    ASSERT_NE(card, nullptr);
    EXPECT_NE(card->GetFullText().find("Effective layer: Foreground. Order in layer: 7."), std::string::npos);
    group.Layer = -30;
    group.OrderInLayer = -2;
    World.AddComponentImmediate(parent, group);
    ASSERT_EQ(refresh.size(), 1u);
    refresh.front()();
    EXPECT_NE(card->GetFullText().find("Effective layer: Background. Order in layer: -2."), std::string::npos);
}

TEST_F(RenderSortingEditorTests, DiagnosticOnlyShowsAnEnabledAncestorGroup)
{
    const auto entity = Mesh();
    UIElement content;
    std::vector<std::function<void()>> refresh;
    InspectorContext context;
    context.World = &World;
    context.Entity = entity;
    context.Parent = &content;
    context.SimulationRefreshCallbacks = &refresh;
    AddRenderSortingDiagnostic(context);
    auto* card = content.GetChildren().front().get();
    const auto isHidden = [card]()
    {
        return card->Style().Get(Style::Display) == DisplayMode::None;
    };
    EXPECT_TRUE(isHidden());
    Sorting group{};
    group.GroupChildren = true;
    World.AddComponentImmediate(entity, group);
    refresh.front()();
    EXPECT_TRUE(isHidden()) << "A group's own settings are not overridden";
    const auto parent = Mesh();
    World.AddComponentImmediate(parent, group);
    World.AddComponentImmediate(entity, Components::Parent{parent});
    refresh.front()();
    EXPECT_FALSE(isHidden());
    World.RemoveComponentImmediate<Sorting>(parent);
    refresh.front()();
    EXPECT_TRUE(isHidden());
}

template<typename Control>
Control* FindControl(UIElement& element)
{
    if (auto* control = dynamic_cast<Control*>(&element))
        return control;
    for (const auto& child : element.GetChildren())
        if (auto* control = FindControl<Control>(*child))
            return control;
    return nullptr;
}

TEST_F(RenderSortingEditorTests, DefaultFieldsEditWithoutAnExistingComponentAndUndoRestoresAbsence)
{
    const auto entity = Mesh();
    UIElement content;
    InspectorContext context;
    context.World = &World;
    context.Entity = entity;
    context.Parent = &content;
    context.Undo = &Undo;
    AddRenderSortingFields(context);
    auto* layer = FindControl<Dropdown>(content);
    auto* order = FindControl<IntField>(content);
    ASSERT_NE(layer, nullptr);
    ASSERT_NE(order, nullptr);
    EXPECT_EQ(layer->GetSelectedValue(), "0");
    EXPECT_EQ(order->GetValue(), 0);
    EXPECT_FALSE(World.HasComponent<Sorting>(entity));
    EXPECT_FALSE(Undo.CanUndo());
    layer->SetSelectedValue("10");
    ASSERT_TRUE(World.HasComponent<Sorting>(entity));
    EXPECT_EQ(World.GetComponent<Sorting>(entity)->Layer, 10);
    Undo.Undo();
    EXPECT_FALSE(World.HasComponent<Sorting>(entity));
    order->SetValueWithoutNotify(12);
    order->NotifyValueChanging();
    ASSERT_TRUE(World.HasComponent<Sorting>(entity));
    EXPECT_EQ(World.GetComponent<Sorting>(entity)->OrderInLayer, 12);
    EXPECT_TRUE(Undo.HasOpenInteractiveEdit());
    order->NotifyValueChanged();
    EXPECT_FALSE(Undo.HasOpenInteractiveEdit());
    Undo.Undo();
    EXPECT_FALSE(World.HasComponent<Sorting>(entity));
    EXPECT_FALSE(Undo.CanUndo());
    Undo.Redo();
    EXPECT_EQ(World.GetComponent<Sorting>(entity)->OrderInLayer, 12);
}

TEST_F(RenderSortingEditorTests, OrderGestureEditsMixedSelectionInOneUndoStep)
{
    const auto first = Mesh();
    const auto second = Mesh();
    Sorting initial{};
    initial.Layer = 10;
    initial.OrderInLayer = 17;
    initial.GroupChildren = true;
    World.AddComponentImmediate(second, initial);
    InspectorContext context;
    context.World = &World;
    context.Entity = first;
    context.Entities = {first, second};
    context.Undo = &Undo;
    auto handlers = MakeRenderSortingOrderHandlers(context);
    handlers.first(3);
    handlers.first(8);
    handlers.second(8);
    EXPECT_EQ(World.GetComponent<Sorting>(first)->OrderInLayer, 8);
    EXPECT_EQ(World.GetComponent<Sorting>(second)->OrderInLayer, 8);
    EXPECT_EQ(World.GetComponent<Sorting>(second)->Layer, 10);
    EXPECT_TRUE(World.GetComponent<Sorting>(second)->GroupChildren);
    Undo.Undo();
    EXPECT_FALSE(World.HasComponent<Sorting>(first));
    EXPECT_EQ(World.GetComponent<Sorting>(second)->OrderInLayer, 17);
    EXPECT_FALSE(Undo.CanUndo());
    Undo.Redo();
    EXPECT_EQ(World.GetComponent<Sorting>(first)->OrderInLayer, 8);
    EXPECT_EQ(World.GetComponent<Sorting>(second)->OrderInLayer, 8);
}

TEST_F(RenderSortingEditorTests, CancelAndDefaultOrderLeaveNoImplicitOverride)
{
    const auto entity = Mesh();
    InspectorContext context;
    context.World = &World;
    context.Entity = entity;
    context.Undo = &Undo;
    {
        auto handlers = MakeRenderSortingOrderHandlers(context);
        handlers.first(4);
        ASSERT_TRUE(World.HasComponent<Sorting>(entity));
    }
    EXPECT_FALSE(World.HasComponent<Sorting>(entity));
    EXPECT_FALSE(Undo.CanUndo());
    auto handlers = MakeRenderSortingOrderHandlers(context);
    handlers.first(4);
    handlers.second(0);
    EXPECT_FALSE(World.HasComponent<Sorting>(entity));
    EXPECT_FALSE(Undo.CanUndo());
    EXPECT_FALSE(Undo.HasOpenInteractiveEdit());
}

}
}

namespace GameEngine::Editor
{
namespace
{
Dropdown* LayerPicker(UIElement& row, const char* name)
{
    for (const auto& child : row.GetChildren())
        if (child->HasClass(name)) return static_cast<Dropdown*>(child.get());
    return nullptr;
}
void ItemIdentities(const UIElement& element, std::vector<uint64_t>& ids)
{
    if (element.HasClass("dropdown-item")) ids.push_back(element.GetInstanceId());
    for (const auto& child : element.GetChildren()) ItemIdentities(*child, ids);
}
std::vector<uint64_t> ItemIdentities(const UIElement& element)
{
    std::vector<uint64_t> ids;
    ItemIdentities(element, ids);
    return ids;
}
}
}
