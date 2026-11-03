#pragma once

#include "AssetCore/GUID.h"
#include "AssetCore/Types.h"
#include "ECS/ECS.h"
#include "Platform/ContextMenu.h"
#include "TileMap/TileLayerEdits.h"
#include "UI/Interaction/DropTarget.h"
#include "UI/UIElement.h"

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace GameEngine
{

struct EditorContext;
class UIElement;

namespace Editor
{
class TileMapEditContext;
class UndoRedoService;
} // namespace Editor

/**
 * @brief The palette's layer section: the map's layers as rows in draw order,
 * each with its own visibility, lock, depth, duplicate and rename controls, a
 * header of layer-level actions, and drag-to-reorder.
 *
 * Lists the map the edit context targets and writes the target when a row is
 * clicked; the panel hears about that through OnLayerTargeted and adopts the
 * layer's tileset. Every edit goes through TileLayerEdits and lands on the
 * undo stack; the rows rebuild from the store's StructureVersion, so an undo
 * refreshes them the same way a click does.
 */
class TilePaletteLayerList : public UIElement, public UI::Interaction::IDropTarget
{
  public:
    TilePaletteLayerList();

    /// Editor services (ECS world, undo, main window for menus). Borrowed; outlives the list.
    void SetContext(const EditorContext* ctx) { m_Context = ctx; }
    /// The shared authoring state whose target this list shows and writes. Borrowed.
    void SetEditContext(Editor::TileMapEditContext* editContext) { m_EditContext = editContext; }
    void SetUndoRedoService(Editor::UndoRedoService* undoRedo) { m_UndoRedo = undoRedo; }

    /** A row click retargeted the edit context; the panel follows the target. */
    void SetOnLayerTargeted(std::function<void()> callback);
    /** A layer was added to the listed map and targeted; the panel binds its tileset. */
    void SetOnLayerAdded(std::function<void()> callback);
    /** The "New Map" header button; creating an asset is the panel's business. */
    void SetOnCreateMapRequested(std::function<void()> callback);
    /** One-line status for the panel's hint strip. */
    void SetOnHint(std::function<void(const std::string&)> callback);

    /**
     * Per-frame poll. Rebuilds the rows when the targeted map, its structure
     * version, or the target entity's visibility moved; only moves the active
     * bar when just the target layer changed.
     */
    void Update();

    /** The map whose layers are listed (null when nothing is targeted). */
    const GUID& ListedMap() const { return m_ListedMap; }

  private:
    void BuildHeader();
    void AddLayer(bool intGrid);
    void RemoveActiveLayer();
    void DuplicateLayer(uint32 sourceLayer);
    void ShowRowMenu(uint32 layerId, int x, int y);
    void CommitLayerEdit(const char* name, const Editor::TileLayerEdits::LayerStateSnapshot& before);
    Editor::TileLayerEdits::LayerStateSnapshot CaptureState() const;
    /** Rows in the order shown, frontmost first. */
    std::vector<uint32> RowOrder() const;

    void RefreshRows();
    void RebuildRows();
    /** Queues a row rebuild for the next Update: handlers run inside the rows being torn down. */
    void InvalidateRows() { m_RowsDirty = true; }
    /**
     * Marks one row as the paint target without rebuilding the list. The row's
     * name is an editable field, so a rebuild here would destroy the element the
     * user is typing into the moment it takes focus.
     */
    void SetActiveRow(uint32 layerId);
    /**
     * Targets `layerId` and updates the multi-selection: a plain click selects
     * only that row, Ctrl toggles it, Shift extends from the target row.
     */
    void TargetLayer(ECS::EntityHandle entity, uint32 layerId, int modifiers);
    /** The rows a batch action applies to: the multi-selection, or just `layerId`. */
    std::vector<uint32> BatchLayers(uint32 layerId) const;
    void SetSelectedVisible(const std::vector<uint32>& layers, bool visible);
    void SetSelectedLocked(const std::vector<uint32>& layers, bool locked);
    void DeleteLayers(const std::vector<uint32>& layers);
    void ApplySelectionClasses();
    void Hint(const std::string& text);

    // IDropTarget: dragging a row reorders draw order.
    bool AcceptsPayload(UI::Interaction::PayloadTypeId typeId) const override;
    bool HitTestDropTarget(float x, float y, UI::Interaction::DropHit& out) const override;
    UI::Interaction::DropFeedback CanDrop(const UI::Interaction::DropRequest& request) const override;
    void PerformDrop(const UI::Interaction::DropRequest& request) override;
    void SetDropPreview(const UI::Interaction::DropPreviewState& state) override;

    const EditorContext* m_Context = nullptr;
    Editor::TileMapEditContext* m_EditContext = nullptr;
    Editor::UndoRedoService* m_UndoRedo = nullptr;

    std::function<void()> m_OnLayerTargeted;
    std::function<void()> m_OnLayerAdded;
    std::function<void()> m_OnCreateMapRequested;
    std::function<void(const std::string&)> m_OnHint;

    UIElement* m_Rows = nullptr;
    std::vector<std::pair<uint32, UIElement*>> m_RowElements;
    // Rows marked for a batch action, in no particular order. The target row is
    // always one of them; a plain click makes it the only one.
    std::vector<uint32> m_Selected;
    std::unique_ptr<INativeContextMenu> m_RowMenu;
    uint32 m_RowMenuLayer = 0;

    bool m_DragPending = false;
    uint32 m_DragLayer = 0;
    float32 m_DragStartX = 0.0f;
    float32 m_DragStartY = 0.0f;
    std::string m_DragLabel;
    // Depths as they were when the drag began, so a cancelled drag restores.
    std::vector<std::pair<uint32, float32>> m_DragRestore;

    // Signature of the list as last built: rebuilding only when it moves keeps
    // Update() to a version compare and one component read on quiet frames.
    ECS::EntityHandle m_ListedEntity;
    GUID m_ListedMap;
    uint64 m_ListedStructureVersion = 0;
    uint32 m_ListedTarget = 0;
    bool m_ListedVisible = false;
    bool m_RowsDirty = false;
    // Applied on the next update, never from inside the focus handler that
    // sets it: restyling a row while it is taking focus re-enters layout.
    uint32 m_PendingActiveLayer = 0;
    bool m_ActiveRowDirty = false;
};

} // namespace GameEngine
