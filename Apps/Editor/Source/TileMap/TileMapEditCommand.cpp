#include "TileMap/TileMapEditCommand.h"

#include "TileMapECS/TileMapRuntimeStore.h"

#include <algorithm>
#include <utility>

namespace GameEngine
{
namespace Editor
{

TileMapEditCommand::TileMapEditCommand(const char* name, const GUID& mapGuid,
                                       std::vector<LayerEdit> edits)
    : m_Name(name)
    , m_MapGuid(mapGuid)
    , m_Edits(std::move(edits))
{
}

void TileMapEditCommand::Do() { Apply(true); }

void TileMapEditCommand::Undo() { Apply(false); }

void TileMapEditCommand::Apply(bool useAfter)
{
    TileMapECS::TileMapRuntimeStore* store = TileMapECS::TileMapRuntimeStore::TryGet();
    if (!store)
        return;

    // The dressed cells are replayed rather than recomputed: the recorded values
    // are what the user saw, and rule evaluation reads state the undo has not
    // finished restoring yet.
    //
    // Order matters, and it is not symmetric. One gesture writes the same cell
    // repeatedly — a stroke crossing its own path, and every rule re-dress that
    // follows an IntGrid edit — so the deltas form a chain per cell. Redo has to
    // walk that chain forwards and undo backwards; replaying a `Before` chain
    // forwards leaves the cell holding an intermediate value, not its original.
    const auto replay = [&](const LayerEdit& edit, size_t index)
    {
        if (index < edit.IntGridValues.size())
        {
            const IntGridDelta& delta = edit.IntGridValues[index];
            store->SetIntGridValue(m_MapGuid, edit.LayerId, delta.CellX, delta.CellY,
                                   useAfter ? delta.After : delta.Before);
        }
        if (index < edit.Cells.size())
        {
            const CellDelta& delta = edit.Cells[index];
            store->SetCellAuto(m_MapGuid, edit.LayerId, delta.CellX, delta.CellY,
                               useAfter ? delta.After : delta.Before,
                               useAfter ? delta.AutoAfter : delta.AutoBefore);
        }
    };

    for (const LayerEdit& edit : m_Edits)
    {
        const size_t count = std::max(edit.IntGridValues.size(), edit.Cells.size());
        for (size_t step = 0; step < count; ++step)
            replay(edit, useAfter ? step : count - 1 - step);
    }
}

} // namespace Editor
} // namespace GameEngine
