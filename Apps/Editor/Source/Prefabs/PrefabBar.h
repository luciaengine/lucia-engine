#pragma once

#include "EditorChangeNotifications.h"
#include "UI/UIElement.h"

#include "ECS/ECS.h"
#include "Scene/PrefabAuthoring.h"

#include <filesystem>
#include <functional>
#include <vector>

namespace GameEngine
{
class Button;
class Label;
} // namespace GameEngine

namespace GameEngine::ECS
{
class World;
}

namespace GameEngine::Editor
{
class PrefabOverridesList;
struct PrefabOverridesListContext;
struct PrefabOverridesListReopen;
class SceneDocumentManager;
class UndoRedoService;

/// Instance Inspector bar with source selection, counts and an Overrides list.
/// Commits refresh it in place; membership or removed-component changes request a rebuild.
/// An instance rebuild can reopen its Overrides list.
class PrefabBar final : public UIElement
{
  public:
    /// Shows the prefab file at `prefabFile` in the Assets panel.
    using SelectAssetHandler = std::function<void(const std::filesystem::path& prefabFile)>;

    /// Builds a bar for an instance entity; null notifications leave a construction-time snapshot.
    /// undo and overridesContext serve the Overrides list. refreshMarks updates marks in place;
    /// requestRebuild defers an Inspector rebuild.
    PrefabBar(ECS::World& world, ECS::EntityHandle entity, EditorChangeNotifications* notifications,
              UndoRedoService* undo, const PrefabOverridesListContext& overridesContext,
              SelectAssetHandler selectAsset, std::function<void()> refreshMarks,
              std::function<void()> requestRebuild);
    ~PrefabBar() override;

    PrefabBar(const PrefabBar&) = delete;
    PrefabBar& operator=(const PrefabBar&) = delete;

  private:
    void Refresh();
    void RefreshAfterChange();
    void SelectAsset();
    void OpenOverridesList();
    void OnComponentChanged(const EditorChangeNotifications::ComponentChangedEvent& change);
    void OnWorldStructureChanged(const EditorChangeNotifications::WorldStructureChangedEvent& change);

    ECS::World& m_World;
    ECS::EntityHandle m_Entity;
    // The outermost instance root of the entity at the last refresh.
    ECS::EntityHandle m_Root;
    EditorChangeNotifications* m_Notifications = nullptr;
    EditorChangeNotifications::SubscriptionToken m_ComponentToken{};
    EditorChangeNotifications::SubscriptionToken m_StructureToken{};
    SelectAssetHandler m_SelectAsset;
    std::function<SceneDocumentManager*()> m_Document;
    PrefabOverridesListReopen* m_Reopen = nullptr;
    std::function<void()> m_RefreshMarks;
    std::function<void()> m_RequestRebuild;
    // The entity's own overrides as the Inspector's marks show them.
    std::vector<Scene::PrefabOverride> m_ShownOverrides;
    UIElement* m_Glyph = nullptr;
    Label* m_TitleLead = nullptr;
    Label* m_TitlePrefab = nullptr;
    Label* m_TitleTail = nullptr;
    UIElement* m_Counts = nullptr;
    Label* m_CountNone = nullptr;
    Button* m_Count = nullptr;
    Label* m_CountText = nullptr;
    Button* m_InstanceCount = nullptr;
    Label* m_InstanceCountText = nullptr;
    Button* m_StaleCount = nullptr;
    Label* m_StaleCountText = nullptr;
    PrefabOverridesList* m_OverridesList = nullptr;
    Button* m_SelectAssetButton = nullptr;
};

} // namespace GameEngine::Editor
