#pragma once

#include "ECS/ECS.h"

#include <cstdint>

namespace GameEngine
{
class UIElement;
} // namespace GameEngine

namespace GameEngine::ECS
{
class World;
} // namespace GameEngine::ECS

namespace GameEngine::Editor
{

/// The eye on a Hierarchy row: the row's `hierarchy-eye` child. It shows the entity's on/off state, as
/// the entity icon does (HierarchyRowActivity), and it is a tree icon hit target like the entity icon,
/// so the tree's icon press, multi-selection and drag switch the entity from either icon.
class HierarchyRowEye
{
  public:
    HierarchyRowEye();

    /// Gives `row` its eye; once per pooled row.
    void AddTo(UIElement& row);

    /// Shows the eye on `row` when the eye column is on and the row shows an entity.
    void Present(UIElement& row, ECS::EntityHandle entity);

    /// True once after the eye column was turned on or off, from the menu or from Settings.
    bool ConsumeShownChange();

  private:
    uint64_t m_ShownRevision = 0;
};

/// Whether Hierarchy rows show the eye column. The editor preferences keep the choice; on by default.
bool IsHierarchyEyeShown();
/// Turns the eye column on or off and saves the choice. The Hierarchy menu and Settings share it.
void SetHierarchyEyeShown(bool shown);
/// Adds the Hierarchy page, with the eye column toggle, to the editor settings.
void RegisterHierarchySettingsCategory();

} // namespace GameEngine::Editor
