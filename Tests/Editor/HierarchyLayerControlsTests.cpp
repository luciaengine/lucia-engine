// The Hierarchy's layer columns: the Sorting Layer and Render Layer dropdowns of a pooled row, the edit
// each makes on the row's targets, and the refresh after a layer or a layer name changes.
#include <gtest/gtest.h>

#include "Editor/Hierarchy/LayerControls/HierarchyLayerControls.h"

#include "Components/Rendering/MeshRenderer.h"
#include "Components/Rendering/RenderSorting.h"
#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "Engine/Rendering/RenderLayerNames.h"
#include "Engine/Rendering/SortingLayers.h"
#include "Inspectors/RenderLayers/RenderLayerField.h"
#include "Panels/HierarchyCommandIds.h"
#include "Rendering/RenderLayerMask.h"
#include "Rendering/Sorting/RenderSortingEditor.h"
#include "UI/Controls/Dropdown.h"
#include "UI/Controls/Label.h"
#include "UI/StyleProperties.h"
#include "UI/UIElement.h"
#include "UndoRedo/UndoRedoService.h"

#include <string>
#include <vector>

namespace GameEngine::Editor
{
namespace
{
using Sorting = Components::RenderSorting;
namespace gr = GameEngine::Rendering;

class HierarchyLayerControlsTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        RegisterRenderSortingComponent(ECS::GetComponentTypeId<Components::MeshRenderer>());
        std::string error;
        ASSERT_TRUE(gr::SortingLayers::SetCurrent(gr::SortingLayers::Defaults(), error));
        gr::SetRenderLayerNames({});
    }

    void TearDown() override { gr::SetRenderLayerNames({}); }

    ECS::EntityHandle Mesh(uint32 layers = gr::RenderLayerMask::Default().Bits)
    {
        const auto entity = World.CreateEntity();
        World.AddComponentImmediate(entity, Components::Transform{});
        Components::MeshRenderer renderer{};
        renderer.RenderLayers.Bits = layers;
        World.AddComponentImmediate(entity, renderer);
        return entity;
    }

    void Present(ECS::EntityHandle entity, std::vector<ECS::EntityHandle> targets, float panelWidth = 350.0f)
    {
        Controls.Present(World, entity, Row, Title, panelWidth, [targets]() { return targets; }, &Undo, nullptr);
    }

    Dropdown* Picker(const char* className)
    {
        for (const auto& child : Row.GetChildren())
            if (child->HasClass(className))
                return static_cast<Dropdown*>(child.get());
        return nullptr;
    }

    ECS::World World;
    UndoRedoService Undo;
    UIElement Row;
    Label Title;
    HierarchyLayerControls Controls;
};

} // namespace

TEST_F(HierarchyLayerControlsTest, ThePooledSortingPickerEditsTheEntityTheRowShowsNow)
{
    auto layers = gr::SortingLayers::Defaults();
    layers.Layers.push_back({1234, "Meadow Canopy", 99});
    std::string error;
    ASSERT_TRUE(gr::SortingLayers::SetCurrent(layers, error));
    const auto first = Mesh();
    const auto second = Mesh();
    ASSERT_TRUE(Controls.HandleCommand(HierarchyCommandIds::kCmdShowSortingLayer));
    Present(first, {first});
    Present(second, {second});

    Dropdown* picker = Picker("hierarchy-sorting-picker");
    ASSERT_NE(picker, nullptr);
    EXPECT_TRUE(picker->HasClass("hierarchy-layer-picker-narrow"));
    EXPECT_TRUE(Title.HasClass("hierarchy-title-one-layer-narrow"));
    picker->SetSelectedIndex(static_cast<int>(layers.Layers.size()) - 1);
    EXPECT_FALSE(World.HasComponent<Sorting>(first)) << "the row's previous entity is left alone";
    ASSERT_TRUE(World.HasComponent<Sorting>(second));
    EXPECT_EQ(World.GetComponent<Sorting>(second)->Layer, 1234);
    EXPECT_EQ(picker->GetSelectedValue(), "1234");

    EXPECT_TRUE(Controls.NeedsRefresh(World));
    EXPECT_FALSE(Controls.NeedsRefresh(World));
    auto edited = *World.GetComponent<Sorting>(second);
    edited.Layer = 10;
    World.AddComponentImmediate(second, edited);
    EXPECT_TRUE(Controls.NeedsRefresh(World)) << "a sorting edit elsewhere refreshes the column";
    ASSERT_TRUE(gr::SortingLayers::SetCurrent(gr::SortingLayers::Defaults(), error));
    EXPECT_TRUE(Controls.NeedsRefresh(World)) << "a project layer change refreshes the column";
}

TEST_F(HierarchyLayerControlsTest, TheRenderLayerPickerSetsOneNamedLayerOnTheSelectionInOneStep)
{
    gr::RenderLayerNames names{};
    ASSERT_EQ(names.Set(3, "Props"), "");
    gr::SetRenderLayerNames(names);
    const auto first = Mesh(0b0001u);
    const auto second = Mesh(0b0110u);
    ASSERT_TRUE(Controls.HandleCommand(HierarchyCommandIds::kCmdShowRenderLayer));
    Present(first, {first, second}, 600.0f);

    Dropdown* picker = Picker("hierarchy-visibility-picker");
    ASSERT_NE(picker, nullptr);
    EXPECT_TRUE(Title.HasClass("hierarchy-title-one-layer"));
    ASSERT_NE(Picker("hierarchy-sorting-picker"), nullptr);
    EXPECT_EQ(Picker("hierarchy-sorting-picker")->Style().Get(Style::Display), DisplayMode::None)
        << "the Sorting Layer column stays off";
    picker->SetSelectedValue(std::to_string(gr::RenderLayerMask::FromLayer(3).Bits));
    EXPECT_EQ(picker->GetSelectedLabel(), "Props") << "the option carries the project's name";
    EXPECT_EQ(World.GetComponent<Components::MeshRenderer>(first)->RenderLayers.Bits, 0b1000u);
    EXPECT_EQ(World.GetComponent<Components::MeshRenderer>(second)->RenderLayers.Bits, 0b1000u);
    EXPECT_FALSE(World.HasComponent<Sorting>(first)) << "the columns are independent";
    EXPECT_EQ(Undo.GetUndoCount(), 1u);

    Undo.Undo();
    EXPECT_EQ(World.GetComponent<Components::MeshRenderer>(first)->RenderLayers.Bits, 0b0001u);
    EXPECT_EQ(World.GetComponent<Components::MeshRenderer>(second)->RenderLayers.Bits, 0b0110u);
}

TEST_F(HierarchyLayerControlsTest, AMixedMaskShowsAsAMaskAndARenameRefreshesTheNames)
{
    const auto entity = Mesh(0b0110u);
    ASSERT_TRUE(Controls.HandleCommand(HierarchyCommandIds::kCmdShowRenderLayer));
    Present(entity, {entity});
    Dropdown* picker = Picker("hierarchy-visibility-picker");
    ASSERT_NE(picker, nullptr);
    EXPECT_EQ(picker->GetSelectedLabel(), "Mask 0x00000006");
    EXPECT_TRUE(Controls.NeedsRefresh(World));
    EXPECT_FALSE(Controls.NeedsRefresh(World));

    gr::RenderLayerNames names{};
    ASSERT_EQ(names.Set(0, "World"), "");
    gr::SetRenderLayerNames(names);
    RefreshRenderLayerFields();
    EXPECT_TRUE(Controls.NeedsRefresh(World)) << "a rename on the settings page refreshes the column";

    World.GetComponentForWrite<Components::MeshRenderer>(entity)->RenderLayers.Bits = 0b0001u;
    Present(entity, {entity});
    EXPECT_EQ(picker->GetSelectedLabel(), "World");
}

} // namespace GameEngine::Editor
