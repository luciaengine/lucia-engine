#pragma once

#include "AssetCore/GUID.h"
#include "AssetCore/Types.h"
#include "TileMap/TileMapEditContext.h"
#include "TileMap/TileSetAsset.h"
#include "Platform/ContextMenu.h"
#include "UI/Controls/DockPanel.h"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace GameEngine
{

struct EditorContext;
class AssetField;
class Button;
class Checkbox;
class IntField;
class Label;
class TilePaletteBrushList;
class TilePaletteGrid;
class TilePaletteIntGridList;
class TilePaletteLayerList;
class TilePaletteTerrainList;
class UIElement;

namespace Editor
{
class UndoRedoService;
}

/**
 * @brief Tilemap authoring palette: picks the tileset, arms the tool, selects
 * the stamp out of the atlas, and hosts the layer list that retargets the
 * layer a stroke writes to.
 *
 * The panel writes TileMapEditContext (tileset, stamp, tool); its layer list
 * writes the target; the Scene View tool reads the same context. Nothing here
 * registers a callback on the context — Update() polls Version(), which cannot
 * dangle if the panel is destroyed mid-edit.
 */
class TilePalettePanel : public DockPanel
{
  public:
    TilePalettePanel();

    std::string_view DeclaredTabIconClass() const override { return "tile-palette-icon"; }

    /// Editor services (asset registry, thumbnails, ECS world). Borrowed; outlives the panel.
    void SetContext(const EditorContext* ctx);
    void SetUndoRedoService(Editor::UndoRedoService* undoRedo);

    /**
     * Invoked when a tool button is pressed. The palette chooses *what* a stroke
     * does, but the Scene View owns *which* tool receives pointer events — so
     * arming a tile tool has to reach the scene controller, or the buttons would
     * silently do nothing until some other tool happened to be the tilemap one.
     */
    void SetOnToolActivated(std::function<void()> callback);

    /// The shared authoring state this panel writes. Borrowed; outlives the panel.
    void SetEditContext(Editor::TileMapEditContext* editContext);

    /** Per-frame poll, registered with EditorPanelManager. */
    void Update();

    /** Whether the Scene View's active tool is the tile paint tool; dims the tool row when not. */
    void SetSceneToolArmed(bool armed);

  private:
    void BuildLayout();
    std::unique_ptr<UIElement> BuildTileSetRow();
    std::unique_ptr<UIElement> BuildToolRow();
    std::unique_ptr<UIElement> BuildTerrainSection();
    std::unique_ptr<UIElement> BuildIntGridSection();
    std::unique_ptr<UIElement> BuildBrushSection();
    std::unique_ptr<UIElement> BuildReplaceSection();
    /** Replaces every cell of the target layer, or of the whole map, that holds the From tile. */
    void ReplaceTile(bool wholeMap);
    std::unique_ptr<UIElement> BuildLayerSection();

    /**
     * Writes the current stamp into the tileset as a named brush. Brushes live
     * in the tileset because their cell ids index it; the write goes through
     * TileSetDocument so an open Tile Set Editor sees the file change.
     */
    void SaveStampAsBrush();
    void DeleteBrush(size_t index);
    void ShowBrushMenu(size_t index, int x, int y);
    /** Arms brush `index` of the loaded tileset as the stamp. */
    void ChooseBrush(size_t index);

    void OnTileSetChanged(const GUID& tileSet);
    /// Loads the tileset asset behind `tileSet` and refills the grid (clears it on failure).
    void ReloadTileSet(const GUID& tileSet);
    void OnGridSelectionChanged(int32 column0, int32 row0, int32 column1, int32 row1);
    /// The layer list retargeted the context: adopt the layer's tileset and re-sync.
    void OnLayerTargeted();

    /// Mirrors the armed tool / scatter flag back onto the controls.
    void SyncFromEditContext();
    /// Refills the IntGrid chips when the target layer's value table can have moved.
    void RefreshIntGridValues();
    void SetHint(const std::string& text);

    /** Swaps the atlas grid for the empty-state message, and back. */
    void ShowGridOrEmptyState(bool hasTiles);
    /** Rewrites the hover readout (tile id, cell, atlas scale) under the grid. */
    void RefreshGridStatus();

    /**
     * Writes a new single-layer `.tilemap` into the project's asset root and
     * registers it, returning its GUID (null on failure). Nothing else in the
     * editor can author a tilemap asset, so the surface that paints them is
     * also the one that creates them.
     */
    GUID CreateTileMapAsset();

    /**
     * Copies the palette's tileset onto the target layer. A layer carries the
     * tileset its cell ids index, and the renderer reads it from there — so
     * choosing a tileset in the palette is what assigns it.
     */
    void ApplyTileSetToTargetLayer();
    /** Binds the palette's tileset to every Tiles layer of the target map, undoably. */
    void ApplyTileSetToAllLayers();

    /**
     * Points the palette at the tileset the newly targeted layer already uses.
     * Binding was push-only, so selecting a painted layer left the field empty
     * and demanded you re-pick the atlas the layer already names.
     */
    void AdoptTargetLayerTileSet();

    /** Hides the terrain and IntGrid sections when the layer cannot use them. */
    void GateEmptySections();

    /** Stacks the atlas over the layers, or puts them side by side in a wide dock. */
    void ApplySplitOrientation();

    const EditorContext* m_Context = nullptr;
    Editor::TileMapEditContext* m_EditContext = nullptr;

    AssetField* m_TileSetField = nullptr;
    UIElement* m_ApplyTileSetAllRow = nullptr;
    TilePaletteGrid* m_Grid = nullptr;
    Checkbox* m_ScatterToggle = nullptr;
    TilePaletteLayerList* m_LayerList = nullptr;
    UIElement* m_ToolRow = nullptr;
    bool m_SceneToolArmed = false;

    // The split flips to side-by-side in a wide dock; Splitter reads its own
    // row/col class at drag time, so both it and the container are re-classed.
    UIElement* m_SplitBody = nullptr;
    UIElement* m_Splitter = nullptr;
    bool m_SplitIsHorizontal = false;

    // The sections themselves, so each can be hidden outright when it has
    // nothing to offer: a notice saying a feature is unavailable, next to a
    // control that does nothing, is worse than showing neither.
    UIElement* m_TerrainSection = nullptr;
    UIElement* m_IntGridSection = nullptr;
    UIElement* m_BrushSection = nullptr;
    UIElement* m_ReplaceSection = nullptr;
    IntField* m_ReplaceFromField = nullptr;
    IntField* m_ReplaceToField = nullptr;
    TilePaletteBrushList* m_BrushList = nullptr;
    std::unique_ptr<INativeContextMenu> m_BrushMenu;
    size_t m_BrushMenuIndex = 0;
    // The tileset's brushes as last loaded, so a chip can arm one without
    // re-reading the file.
    std::vector<TileMap::TileBrushDef> m_Brushes;
    TilePaletteTerrainList* m_TerrainList = nullptr;
    TilePaletteIntGridList* m_IntGridList = nullptr;
    Checkbox* m_IntGridTintToggle = nullptr;
    Label* m_HintLabel = nullptr;
    UIElement* m_GridBar = nullptr;
    Label* m_GridStatusLabel = nullptr;
    std::string m_GridStatus;
    // Stands in for the atlas grid until a tileset is assigned, so the panel
    // does not open on a large empty block with no explanation.
    Label* m_EmptyStateLabel = nullptr;
    std::function<void()> m_OnToolActivated;

    std::vector<std::pair<Editor::TileMapTool, Button*>> m_ToolButtons;

    // Last observed TileMapEditContext::Version(); a mismatch drives SyncFromEditContext.
    uint64 m_SeenEditVersion = 0;
    GUID m_LoadedTileSet;
    // The target as last followed, so a retarget that arrives from outside the
    // panel (the selection sync) adopts the layer's tileset like a row click.
    GUID m_FollowedTargetMap;
    uint32 m_FollowedTargetLayer = 0;

    // Signature of the IntGrid chip row as last built: the target and the
    // map's structure version, which is what the value table moves with.
    GUID m_IntGridListMap;
    uint32 m_IntGridListLayer = 0;
    uint64 m_IntGridListVersion = 0;

    std::string m_Hint;
};

} // namespace GameEngine
