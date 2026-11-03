#include <gtest/gtest.h>

#include "Components/Rendering/MeshRenderer.h"
#include "Components/Rendering/RenderSorting.h"
#include "Components/Rendering/SkinnedMeshRenderer.h"
#include "Components/Transform.h"
#include "Components/Hierarchy.h"
#include "InspectorRegistry.h"
#include "ECS/World.h"
#include "ECS/ECSTemplates.h"
#include "Editor/Hierarchy/HierarchyLayerControls.h"
#include "Engine/Rendering/SortingLayers.h"
#include "Rendering/Sorting/RenderSortingEditor.h"
#include "Rendering/Sorting/SortingLayerSettings.h"
#include "Editor/Settings/SettingsStore.h"
#include "TestTempDir.h"
#include "UI/Controls/Dropdown.h"
#include "UI/Controls/Label.h"
#include "UI/UIElement.h"
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

TEST_F(RenderSortingEditorTests, PooledPickerUsesCurrentEntityAndCustomLayerIdentity)
{
    auto layers = Rendering::SortingLayers::Defaults();
    layers.Layers.push_back({1234, "Meadow Canopy", 99});
    std::string error;
    ASSERT_TRUE(Rendering::SortingLayers::SetCurrent(layers, error));
    const auto first = Mesh();
    const auto second = Mesh();
    UIElement row;
    Label title;
    HierarchyLayerControls controls;
    ASSERT_TRUE(controls.HandleCommand(0x4177));
    controls.Present(World, first, row, title, 350.0f, [first]() { return std::vector{first}; }, &Undo, nullptr);
    controls.Present(World, second, row, title, 350.0f, [second]() { return std::vector{second}; }, &Undo, nullptr);
    Dropdown* picker = nullptr;
    for (const auto& child : row.GetChildren())
        if (child->HasClass("hierarchy-sorting-picker"))
            picker = static_cast<Dropdown*>(child.get());
    ASSERT_NE(picker, nullptr);
    EXPECT_TRUE(picker->HasClass("hierarchy-layer-picker-narrow"));
    EXPECT_TRUE(title.HasClass("hierarchy-title-one-layer-narrow"));
    picker->SetSelectedIndex(static_cast<int>(layers.Layers.size()) - 1);
    EXPECT_FALSE(World.HasComponent<Sorting>(first));
    ASSERT_TRUE(World.HasComponent<Sorting>(second));
    EXPECT_EQ(World.GetComponent<Sorting>(second)->Layer, 1234);
    EXPECT_EQ(picker->GetSelectedValue(), "1234");
    EXPECT_TRUE(controls.NeedsRefresh(World));
    EXPECT_FALSE(controls.NeedsRefresh(World));
    auto edited = *World.GetComponent<Sorting>(second);
    edited.Layer = 10;
    World.AddComponentImmediate(second, edited);
    EXPECT_TRUE(controls.NeedsRefresh(World));
    ASSERT_TRUE(Rendering::SortingLayers::SetCurrent(Rendering::SortingLayers::Defaults(), error));
    EXPECT_TRUE(controls.NeedsRefresh(World));
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

TEST_F(RenderSortingEditorTests, VisibilityPickerRemainsIndependentAndUndoesTheSelection)
{
    const auto first = Mesh();
    const auto second = Mesh();
    const auto original = World.GetComponent<Components::MeshRenderer>(first)->renderLayerMask;
    UIElement row;
    Label title;
    HierarchyLayerControls controls;
    ASSERT_TRUE(controls.HandleCommand(0x4175));
    controls.Present(World, first, row, title, 350.0f,
        [first, second]() { return std::vector{first, second}; }, &Undo, nullptr);
    Dropdown* picker = nullptr;
    for (const auto& child : row.GetChildren())
        if (child->HasClass("hierarchy-visibility-picker"))
            picker = static_cast<Dropdown*>(child.get());
    ASSERT_NE(picker, nullptr);
    picker->SetSelectedValue("8");
    EXPECT_EQ(World.GetComponent<Components::MeshRenderer>(first)->renderLayerMask, 8u);
    EXPECT_EQ(World.GetComponent<Components::MeshRenderer>(second)->renderLayerMask, 8u);
    EXPECT_FALSE(World.HasComponent<Sorting>(first));
    EXPECT_FALSE(World.HasComponent<Sorting>(second));
    Undo.Undo();
    EXPECT_EQ(World.GetComponent<Components::MeshRenderer>(first)->renderLayerMask, original);
    EXPECT_EQ(World.GetComponent<Components::MeshRenderer>(second)->renderLayerMask, original);
    EXPECT_FALSE(Undo.CanUndo());
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
    const auto* text = dynamic_cast<Label*>(content.GetChildren().front().get());
    ASSERT_NE(text, nullptr);
    EXPECT_NE(text->GetText().find("Foreground / 7 / Group:"), std::string::npos);
    group.Layer = -30;
    group.OrderInLayer = -2;
    World.AddComponentImmediate(parent, group);
    ASSERT_EQ(refresh.size(), 1u);
    refresh.front()();
    EXPECT_NE(text->GetText().find("Background / -2 / Group:"), std::string::npos);
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
TEST_F(RenderSortingEditorTests, RecycledSortingRowsKeepOptionControlsAndSyncWithoutEditing)
{
    const auto first = Mesh(), second = Mesh();
    Sorting initial{}; initial.Layer = 10;
    World.AddComponentImmediate(second, initial);
    UIElement row; Label title; HierarchyLayerControls controls;
    controls.HandleCommand(0x4177);
    const auto present = [&](ECS::EntityHandle entity)
    { controls.Present(World, entity, row, title, 450, [entity] { return std::vector{entity}; }, &Undo, nullptr); };
    present(first);
    auto* picker = LayerPicker(row, "hierarchy-sorting-picker"); ASSERT_NE(picker, nullptr);
    const auto identities = ItemIdentities(*picker); ASSERT_FALSE(identities.empty());
    present(second);
    EXPECT_EQ(ItemIdentities(*picker), identities);
    EXPECT_EQ(picker->GetSelectedValue(), "10");
    EXPECT_FALSE(Undo.CanUndo());
    picker->SetSelectedValue("30");
    EXPECT_EQ(World.GetComponent<Sorting>(second)->Layer, 30);
    EXPECT_FALSE(World.HasComponent<Sorting>(first));
    Undo.Undo(); EXPECT_EQ(World.GetComponent<Sorting>(second)->Layer, 10);
}
TEST_F(RenderSortingEditorTests, CachedSortingChoicesRefreshNamesAndDiscardStaleMissingLayers)
{
    const auto entity = Mesh(); Sorting initial{}; initial.Layer = 12345;
    World.AddComponentImmediate(entity, initial);
    UIElement row; Label title; HierarchyLayerControls controls;
    controls.HandleCommand(0x4177);
    const auto present = [&] { controls.Present(World, entity, row, title, 450,
        [entity] { return std::vector{entity}; }, &Undo, nullptr); };
    present(); auto* picker = LayerPicker(row, "hierarchy-sorting-picker"); ASSERT_NE(picker, nullptr);
    EXPECT_EQ(picker->GetSelectedLabel(), "Missing layer (12345)");
    auto layers = Rendering::SortingLayers::Current();
    layers.Layers.push_back({12345, "Canopy", 99});
    std::string error; ASSERT_TRUE(Rendering::SortingLayers::SetCurrent(layers, error));
    present(); EXPECT_EQ(picker->GetSelectedLabel(), "Canopy");
    layers.Layers.back().Name = "Renamed Canopy";
    ASSERT_TRUE(Rendering::SortingLayers::SetCurrent(layers, error));
    present(); EXPECT_EQ(picker->GetSelectedLabel(), "Renamed Canopy");
    initial.Layer = 0; World.AddComponentImmediate(entity, initial);
    present(); EXPECT_EQ(picker->GetSelectedValue(), "0");
    EXPECT_FALSE(Undo.CanUndo());
}
TEST_F(RenderSortingEditorTests, RecycledVisibilityRowsReuseChoicesAndHandleCompositeMasks)
{
    const auto first = Mesh(), second = Mesh();
    Components::MeshRenderer renderer{}; renderer.renderLayerMask = 8;
    World.AddComponentImmediate(second, renderer);
    UIElement row; Label title; HierarchyLayerControls controls; controls.HandleCommand(0x4175);
    const auto present = [&](ECS::EntityHandle entity)
    { controls.Present(World, entity, row, title, 450, [entity] { return std::vector{entity}; }, &Undo, nullptr); };
    present(first); auto* picker = LayerPicker(row, "hierarchy-visibility-picker"); ASSERT_NE(picker, nullptr);
    const auto identities = ItemIdentities(*picker); ASSERT_FALSE(identities.empty());
    present(second); EXPECT_EQ(ItemIdentities(*picker), identities); EXPECT_EQ(picker->GetSelectedValue(), "8");
    renderer.renderLayerMask = 3; World.AddComponentImmediate(second, renderer);
    present(second); EXPECT_EQ(picker->GetSelectedValue(), "3");
    EXPECT_EQ(picker->GetSelectedLabel(), "Mask 0x00000003");
    present(first); EXPECT_EQ(picker->GetSelectedValue(), "1");
    EXPECT_FALSE(Undo.CanUndo());
}
}
