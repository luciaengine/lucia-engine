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

/// The Inspector's bar on an entity of a prefab instance: the glyph, the prefab and the instance on line 1;
/// the select button and the counts on line 2. A count pill opens the Overrides list, and a new bar on the instance
/// opens the list again when a rebuild closed it. A commit in the instance refreshes the bar in place; an exit from
/// the instance or a change to the removed components asks for a rebuild.
class PrefabBar final : public UIElement
{
  public:
    /// Shows the prefab file at `prefabFile` in the Assets panel.
    using SelectAssetHandler = std::function<void(const std::filesystem::path& prefabFile)>;

    /// A bar for `entity`, which is in a prefab instance of `world`. `notifications` may be null: the
    /// bar then shows the state at construction. `undo` takes the steps of the Overrides list, which reads
    /// `overridesContext`. `refreshMarks` sets the Inspector's override marks again in place; `requestRebuild`
    /// rebuilds the Inspector later.
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
