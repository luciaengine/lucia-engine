#pragma once

#include "AssetCore/GUID.h"
#include "AssetCore/Types.h"
#include "TileMap/TileCellEncoding.h"
#include "UndoRedo/IEditorCommand.h"

#include <vector>

namespace GameEngine
{
namespace Editor
{

/**
 * @brief One undoable tilemap edit, stored as the deltas it produced.
 *
 * A stroke can touch thousands of cells across several layers — an IntGrid
 * layer the user painted plus every Tiles layer the rules re-dressed from it —
 * so the command records only what actually changed, per layer. Replaying it is
 * a bounded write loop in either direction, with no map snapshot to hold.
 *
 * The stroke applies its edits live and commits the finished command through
 * UndoRedoService::CommitAlreadyApplied, so Do() runs only on redo.
 */
class TileMapEditCommand final : public IEditorCommand
{
public:
    /** A tile cell and its provenance: restoring one without the other lies. */
    struct CellDelta
    {
        int32 CellX = 0;
        int32 CellY = 0;
        TileMap::TileCell Before = TileMap::kEmptyCell;
        TileMap::TileCell After = TileMap::kEmptyCell;
        bool AutoBefore = false;
        bool AutoAfter = false;
    };

    struct IntGridDelta
    {
        int32 CellX = 0;
        int32 CellY = 0;
        uint8 Before = 0;
        uint8 After = 0;
    };

    /** Everything one gesture changed on one layer. */
    struct LayerEdit
    {
        uint32 LayerId = 0;
        std::vector<CellDelta> Cells;
        std::vector<IntGridDelta> IntGridValues;

        bool IsEmpty() const { return Cells.empty() && IntGridValues.empty(); }
    };

    TileMapEditCommand(const char* name, const GUID& mapGuid, std::vector<LayerEdit> edits);

    const char* GetName() const override { return m_Name; }
    const char* GetTypeName() const override { return "TileMapEditCommand"; }

    void Do() override;
    void Undo() override;

private:
    void Apply(bool useAfter);

    // Points at a string literal owned by the caller's translation unit.
    const char* m_Name;
    GUID m_MapGuid;
    std::vector<LayerEdit> m_Edits;
};

} // namespace Editor
} // namespace GameEngine
