#include "Terrain/TerrainGrassPresetRow.h"

#include "Components/Terrain/TerrainGrass.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "EditorChangeNotifications.h"
#include "Inspectors/InspectorComponentRowHelpers.h"
#include "Inspectors/InspectorUIHelpers.h"
#include "TerrainGrass/TerrainGrassPresets.h"
#include "UI/Controls/Dropdown.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace GameEngine
{
namespace
{

constexpr const char* kPresetTooltip =
    "The kind of field: sets the colors, blade height and width, density and tilt below, in one undo step. "
    "Every setting stays editable. Reads Custom when one of them differs from the preset.";
constexpr int kCustomIndex = static_cast<int>(TerrainGrass::GrassPreset::Custom);

// One entry of the Preset list: the stored preset and what the list shows for it.
struct GrassPresetEntry
{
    TerrainGrass::GrassPreset Preset;
    const char* Name;
    /// CSS class for the preset image.
    const char* PictureClass;
};

// In the list's order, Meadow (the component's defaults) first.
constexpr GrassPresetEntry kGrassPresetEntries[] = {
    {TerrainGrass::GrassPreset::Meadow, "Meadow",
     "option-picture--grass-meadow"},
    {TerrainGrass::GrassPreset::Lawn, "Lawn", "option-picture--grass-lawn"},
    {TerrainGrass::GrassPreset::Steppe, "Steppe", "option-picture--grass-steppe"},
    {TerrainGrass::GrassPreset::Stylized, "Stylized",
     "option-picture--grass-stylized"},
};

std::vector<Dropdown::Option> PresetOptions()
{
    std::vector<Dropdown::Option> options;
    for (const GrassPresetEntry& entry : kGrassPresetEntries)
        options.push_back({entry.Name, entry.Name, {}, entry.PictureClass});
    options.push_back({"Custom", "Custom", {}, {}});
    return options;
}

// The list index of the preset the grass is on, or kCustomIndex. Reconciled first, so a scene whose
// dials were written by another path reads Custom.
int ShownPresetIndex(const Components::TerrainGrass* grass)
{
    return grass ? static_cast<int>(TerrainGrass::MatchTerrainGrassPreset(*grass)) : kCustomIndex;
}

// The Preset list, kept in step with the grass's dials.
class GrassPresetDropdown final : public Dropdown
{
  public:
    GrassPresetDropdown(ECS::World* world, ECS::EntityHandle terrain, Editor::EditorChangeNotifications* notifications,
                        Editor::UndoRedoService* undo, std::function<void()> requestRefresh)
        : m_World(world)
        , m_Terrain(terrain)
        , m_Notifications(notifications)
        , m_Undo(undo)
        , m_RequestRefresh(std::move(requestRefresh))
    {
        AddClass("inspector-dropdown");
        RequestSubtreeStyleAssetPath("UI/panels/Terrain/TerrainGrassPresets.css", "editor");
        SetOptions(PresetOptions());
        Refresh();
        SetOnValueChanged([this](const std::string&) { OnPresetPicked(); });
        if (m_Notifications)
            m_Subscription = m_Notifications->SubscribeComponentChanged(
                [this](const Editor::EditorChangeNotifications::ComponentChangedEvent& event) { OnChanged(event); });
    }

    ~GrassPresetDropdown() override
    {
        if (m_Notifications && m_Subscription)
            m_Notifications->Unsubscribe(m_Subscription);
    }

  private:
    void OnPresetPicked()
    {
        const int index = GetSelectedIndex();
        if (!m_World || index < 0 || static_cast<size_t>(index) >= std::size(kGrassPresetEntries))
        {
            SyncSelection();
            return;
        }
        const GrassPresetEntry& entry = kGrassPresetEntries[static_cast<size_t>(index)];
        InspectorDrag::CommitComponentWithUndo<Components::TerrainGrass>(
            m_World, m_Terrain, m_Notifications, m_Undo, std::string("Grass preset: ") + entry.Name,
            [&entry](Components::TerrainGrass& grass) { TerrainGrass::ApplyTerrainGrassPreset(grass, entry.Preset); });
        // The dial rows show the values they were built with: rebuild them so they show the preset's.
        if (m_RequestRefresh)
            m_RequestRefresh();
    }

    void OnChanged(const Editor::EditorChangeNotifications::ComponentChangedEvent& event)
    {
        if (event.world == m_World && event.entity == m_Terrain &&
            event.componentType == ECS::GetComponentTypeId<Components::TerrainGrass>())
            Refresh();
    }

    void Refresh()
    {
        // An open list belongs to the user: the selection is not moved under the pointer.
        if (!IsMenuOpen())
            SyncSelection();
    }

    void SyncSelection()
    {
        const auto* grass = m_World ? m_World->GetComponent<Components::TerrainGrass>(m_Terrain) : nullptr;
        SetSelectedIndexWithoutNotify(ShownPresetIndex(grass));
    }

    ECS::World* m_World = nullptr;
    ECS::EntityHandle m_Terrain{};
    Editor::EditorChangeNotifications* m_Notifications = nullptr;
    Editor::UndoRedoService* m_Undo = nullptr;
    std::function<void()> m_RequestRefresh;
    Editor::EditorChangeNotifications::SubscriptionToken m_Subscription{};
};

} // namespace

void AddTerrainGrassPresetRow(UIElement* parent, ECS::World* world, ECS::EntityHandle terrain,
                              Editor::EditorChangeNotifications* notifications, Editor::UndoRedoService* undo,
                              std::function<void()> requestRefresh)
{
    auto* row = InspectorUI::AddRow(parent);
    InspectorUI::AddLabel(row, "Preset", kPresetTooltip);
    if (auto* field = InspectorUI::AddFieldContainer(row))
        field->AddChild(std::make_unique<GrassPresetDropdown>(world, terrain, notifications, undo,
                                                             std::move(requestRefresh)));
}

} // namespace GameEngine
