#pragma once

#include "SceneView/SceneViewTools.h"
#include "SceneView/TileMapPaintOverlay.h"
#include "SceneView/TileMapPaintTarget.h"
#include "TileMap/TileMapEditCommand.h"
#include "TileMap/TileMapTerrainBrush.h"

#include "AssetCore/GUID.h"
#include "AssetCore/Types.h"
#include "ECS/ECS.h"
#include "Mathematics/Matrix4x4.h"
#include "TileMapECS/TileMapRuntimeStore.h"

#include <memory>
#include <utility>
#include <unordered_set>
#include <vector>

namespace GameEngine
{


namespace Engine::Renderer
{
class RenderServices;
}
namespace TileMapECS
{
class TileMapRenderFeature;
}

namespace Editor
{
class TileMapEditContext;
class UndoRedoService;
} // namespace Editor

namespace Editor::SceneTools
{

/**
 * @brief Paints the selected TileMapLayerRef entity's layer from the Scene View.
 *
 * The tool owns no tilemap state of its own: what to paint comes from the
 * TileMapEditContext the palette panel writes, and every cell write goes
 * through TileMapRuntimeStore so the renderer's dirty-chunk ledger stays
 * truthful. One pointer-down/up gesture produces exactly one undo entry,
 * whatever the gesture was — stroke, rectangle, fill, move or paste.
 */
class TileMapPaintTool : public ISceneTool
{
public:
    TileMapPaintTool();

    const char* GetName() const override { return "TileMapPaintTool"; }
    void OnDeactivated() override;
    void OnPointerEvent(const ScenePointerEvent& event) override;
    void OnKeyEvent(const SceneKeyEvent& event) override;
    void GatherGizmos(GizmoCollector& collector) override;
    std::string_view StatusText() const override;

    void SetEditContext(std::shared_ptr<Editor::TileMapEditContext> editContext)
    {
        m_EditContext = std::move(editContext);
    }
    void SetUndoRedoService(Editor::UndoRedoService* undo) { m_UndoRedo = undo; }
    /** Where the stamp ghost's render feature is looked up. Borrowed; null disables the preview. */
    void SetRenderServices(Engine::Renderer::RenderServices* services) { m_RenderServices = services; }

    /**
     * Paints one cell as if the pointer had clicked it, for automation that has
     * no viewport. Phase mirrors a pointer stroke so the undo boundary is the
     * same one interactive painting produces. Returns false when the edit
     * context's target does not resolve to a paintable layer: nothing was
     * written and no stroke is open.
     */
    bool PaintCellForAutomation(int32 cellX, int32 cellY, bool strokeBegin, bool strokeEnd,
                                bool erase = false);

    using CellRect = TileMapCellRect;

    /**
     * Automation counterparts of the marquee gestures, so a harness can exercise
     * the same code paths without synthesising viewport drags.
     */
    void SetSelectionForAutomation(int32 x0, int32 y0, int32 x1, int32 y1);
    void ClearSelection();
    const CellRect& Selection() const { return m_Selection; }
    bool CopySelection();
    bool PasteAt(int32 cellX, int32 cellY);
    bool MoveSelectionBy(int32 deltaX, int32 deltaY);
    /** Returns false when no paint target resolves; nothing was written. */
    bool FillRectForAutomation(int32 x0, int32 y0, int32 x1, int32 y1);

    /** The layer the next stroke would write, or an invalid target. */
    TileMapPaintTarget ResolveTarget() const;

private:
    using PaintTarget = TileMapPaintTarget;

    /** One scatter candidate: a stamp cell and the weight its tile carries. */
    struct ScatterEntry
    {
        TileMap::TileCell Cell = TileMap::kEmptyCell;
        float32 Weight = 1.0f;
    };

    /** A lifted or copied block of cells, row-major from the block's min corner. */
    struct CellBlock
    {
        int32 Width = 0;
        int32 Height = 0;
        std::vector<TileMap::TileCell> Cells;

        bool IsEmpty() const { return Width <= 0 || Height <= 0 || Cells.empty(); }
        TileMap::TileCell At(int32 offsetX, int32 offsetY) const;
    };

    /** What the in-progress pointer drag is doing. */
    enum class DragMode : uint8
    {
        None = 0,
        Paint,
        Line,
        Rect,
        Marquee,
        MoveSelection
    };

    void RebuildScatterWeights();
    bool RayToCell(const PaintTarget& target, const GizmoRay& ray, int32& outCellX,
                   int32& outCellY) const;

    void BeginStroke(const char* name);
    void WriteCell(const PaintTarget& target, int32 cellX, int32 cellY,
                   TileMap::TileCell cell);
    /** Writes one IntGrid value and re-dresses the layers whose rules read it. */
    void WriteIntGridValue(const PaintTarget& target, int32 cellX, int32 cellY, uint8 value);
    void IntGridPaintAt(const PaintTarget& target, int32 cellX, int32 cellY);
    /** Appends the dressed cells a re-evaluation produced to the stroke's deltas. */
    void RecordDressedChanges(const std::vector<TileMapECS::TileMapDressedChange>& changes);
    /** The stroke's delta block for one layer, created on first touch. */
    TileMapEditCommand::LayerEdit& LayerEditFor(uint32 layerId);
    /** The stamp cell that belongs at this offset from a gesture's origin, tiled. */
    TileMap::TileCell StampCellAt(int32 offsetX, int32 offsetY);
    void StampAt(const PaintTarget& target, int32 cellX, int32 cellY);
    void TerrainPaintAt(const PaintTarget& target, int32 cellX, int32 cellY);
    void EraseAt(const PaintTarget& target, int32 cellX, int32 cellY);
    void PickAt(const PaintTarget& target, int32 cellX, int32 cellY);
    void FillFrom(const PaintTarget& target, int32 cellX, int32 cellY);
    void FillRect(const PaintTarget& target, const CellRect& rect);
    void StrokeLine(const PaintTarget& target, int32 fromX, int32 fromY, int32 toX, int32 toY);
    void ApplyAt(const PaintTarget& target, int32 cellX, int32 cellY);
    void CommitStroke();
    void CancelDrag();
    /**
     * Shows the stamp's tiles at the hover cell through the render feature, so
     * the author sees the art that will land, not just its outline. Cleared
     * whenever the cursor is, and for tools that write no stamp.
     */
    void UpdatePreview(const PaintTarget& target, int32 cellX, int32 cellY);
    /** Clears every cell of the marquee as one undo entry named `strokeName`. */
    void EraseSelection(const char* strokeName);
    void ClearPreview();
    TileMapECS::TileMapRenderFeature* PreviewFeature() const;
    CellBlock ReadBlock(const PaintTarget& target, const CellRect& rect) const;

    std::shared_ptr<Editor::TileMapEditContext> m_EditContext;
    Editor::UndoRedoService* m_UndoRedo = nullptr;
    Engine::Renderer::RenderServices* m_RenderServices = nullptr;

    TileMapPaintOverlay m_Overlay;

    bool m_StrokeActive = false;
    // Ctrl/Cmd held when the stroke began: the stroke clears cells instead of
    // stamping, whatever tool is armed. Decided at press so a modifier let go
    // mid-drag does not flip the gesture halfway.
    bool m_StrokeErases = false;
    // The same modifier as last seen on a hover, for the cursor and status line.
    bool m_HoverErases = false;
    const char* m_StrokeName = "Paint Tiles";
    // One entry per layer the gesture touched: the target layer's own writes
    // plus every Tiles layer the rules re-dressed underneath it.
    std::vector<TileMapEditCommand::LayerEdit> m_StrokeEdits;
    // Cells this stroke has already applied. A drag re-enters the cell it
    // painted last at the start of every segment, and crosses cells twice
    // when it doubles back; a scatter stroke would re-roll them each time.
    std::unordered_set<uint64> m_StrokeCells;
    int32 m_LastCellX = 0;
    int32 m_LastCellY = 0;
    bool m_HasLastCell = false;
    // Last cell under the pointer, so a paste can land where the user is looking.
    int32 m_HoverCellX = 0;
    int32 m_HoverCellY = 0;
    bool m_HasHoverCell = false;

    DragMode m_DragMode = DragMode::None;
    int32 m_DragStartX = 0;
    int32 m_DragStartY = 0;
    CellRect m_PendingRect;

    CellRect m_Selection;
    CellBlock m_Clipboard;
    // The cells lifted when a move drag began, so each pointer move can re-place
    // them from the original contents rather than from what the last move wrote.
    CellBlock m_MoveBlock;
    CellRect m_MoveOrigin;
    int32 m_MoveDeltaX = 0;
    int32 m_MoveDeltaY = 0;

    // Seeds the scatter mode's weighted pick; advanced per placed cell so a drag
    // does not repeat one tile.
    uint32 m_ScatterState = 0x9E3779B9u;
    // Rebuilt at most once per stroke — resolving the tileset per painted cell
    // would cost a file read per cell of a drag.
    std::vector<ScatterEntry> m_ScatterWeights;
    float32 m_ScatterWeightTotal = 0.0f;

    // Scratch for the cells one re-dress moved; reused so a drag does not
    // allocate per cell.
    std::vector<TileMapECS::TileMapDressedChange> m_DressedScratch;

    Editor::TileMapTerrainBrush m_TerrainBrush;
    // Scratch for the writes one terrain paint resolves; reused so a drag does
    // not allocate per cell.
    std::vector<Editor::TerrainCellWrite> m_TerrainWrites;
};

} // namespace Editor::SceneTools
} // namespace GameEngine
