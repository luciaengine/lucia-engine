#include "SceneView/TileMapPaintTool.h"

#include "TileMap/TileMapEditContext.h"
#include "UndoRedo/UndoRedoService.h"

#include "Assets/AssetManager.h"
#include "Components/TileMap/TileMapLayerRef.h"
#include "Components/Transform.h"
#include "Core/Engine.h"
#include "ECS/Entity.h"
#include "Editor/Shortcuts/EditorShortcuts.h"
#include "Mathematics/MatrixOps.h"
#include "Mathematics/Ray.h"
#include "TileMap/TileMapChunk.h"
#include "TileMap/TileMapData.h"
#include "TileMap/TileSetAsset.h"
#include "Engine/Rendering/RenderServices.h"
#include "TileMapECS/TileMapRenderFeature.h"
#include "TileMapECS/TileMapRuntimeStore.h"

#include <algorithm>
#include <cmath>
#include <unordered_set>
#include <memory>
#include <utility>

#include <glm/gtc/type_ptr.hpp>

namespace GameEngine
{
namespace Editor::SceneTools
{
namespace
{

// A bucket fill that walked an unbounded grid would never terminate on the
// empty plane outside the authored region. Bounding it to the used region plus
// one ring lets a fill still spill one cell past the current edge — enough to
// close a border — without becoming an infinite flood.
constexpr int32 kFillBorderRing = 1;
// Backstop for a fill over a very large authored region, so a mis-click cannot
// stall the editor for an unbounded time.
constexpr int32 kMaxFillCells = 1 << 20;

/** Undo-entry label for one gesture, chosen by what the armed tool does. */
const char* StrokeLabel(TileMapTool tool, const char* erase, const char* terrain, const char* paint)
{
    if (tool == TileMapTool::Eraser)
        return erase;
    if (tool == TileMapTool::Terrain)
        return terrain;
    if (tool == TileMapTool::IntGrid)
        return "Paint IntGrid";
    return paint;
}

/** xorshift32: a stroke needs cheap, repeatable variety, not statistical rigor. */
// Every key the tool answers is a rebindable "Tile Map Tool" catalog entry.
constexpr const char* kShortcutGroup = "Tile Map Tool";
constexpr TileMapTool kHotkeyTools[] = {TileMapTool::Brush,  TileMapTool::Eraser,
                                        TileMapTool::Picker, TileMapTool::Fill,
                                        TileMapTool::Rect,   TileMapTool::Marquee,
                                        TileMapTool::Terrain, TileMapTool::IntGrid};

uint64 PackCell(int32 cellX, int32 cellY)
{
    return (static_cast<uint64>(static_cast<uint32>(cellX)) << 32) |
           static_cast<uint64>(static_cast<uint32>(cellY));
}

uint32 NextRandom(uint32& state)
{
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

} // namespace

TileMapPaintTool::TileMapPaintTool() = default;

void TileMapPaintTool::OnDeactivated()
{
    ClearPreview();
    CommitStroke();
    CancelDrag();
    m_Overlay.Clear();
    m_HasHoverCell = false;
    // The selection survives a tool switch: coming back from the brush to the
    // marquee should find the region still framed.
}

TileMap::TileCell TileMapPaintTool::CellBlock::At(int32 offsetX, int32 offsetY) const
{
    if (offsetX < 0 || offsetY < 0 || offsetX >= Width || offsetY >= Height)
        return TileMap::kEmptyCell;
    const size_t index =
        static_cast<size_t>(offsetY) * static_cast<size_t>(Width) + static_cast<size_t>(offsetX);
    return index < Cells.size() ? Cells[index] : TileMap::kEmptyCell;
}

TileMapPaintTool::PaintTarget TileMapPaintTool::ResolveTarget() const
{
    PaintTarget target;
    if (!m_EditContext)
        return target;

    ECS::World* world = EngineCore::GetInstance().GetPrimaryWorld();
    if (!world)
        return target;

    // The edit context owns which layer a stroke edits; the tool only checks
    // that its choice still describes a paintable layer in the live world.
    const ECS::EntityHandle entity = m_EditContext->TargetEntity();
    if (!entity.IsValid())
        return target;
    const auto* layerRef = world->GetComponent<Components::TileMapLayerRef>(entity);
    if (!layerRef || layerRef->LayerId == 0 || layerRef->TileMap.IsNull())
        return target;
    const GUID mapGuid = layerRef->TileMap.ToGuid();
    if (mapGuid != m_EditContext->TargetMap() || layerRef->LayerId != m_EditContext->TargetLayer())
        return target;
    const auto* worldXf = world->GetComponent<Components::WorldTransform>(entity);
    if (!worldXf)
        return target;

    TileMapECS::TileMapRuntimeStore* store = TileMapECS::TileMapRuntimeStore::TryGet();
    if (!store)
        return target;
    const TileMap::TileMapData* data = store->GetMap(mapGuid);
    if (!data || data->CellWidth <= 0.0f || data->CellHeight <= 0.0f)
        return target;

    // A locked layer is not a paint target. Refusing here covers every tool
    // and every stroke shape, because this is the one place a target is
    // resolved -- gating each tool separately would leak one eventually.
    if (const TileMap::TileMapLayerData* layer = data->FindLayer(layerRef->LayerId);
        layer != nullptr && layer->Locked)
    {
        return target;
    }

    const Mathematics::Matrix4x4 model(glm::make_mat4(worldXf->matrix));
    target.MapGuid = mapGuid;
    target.LayerId = layerRef->LayerId;
    target.WorldToLocal = Mathematics::Inverse(model);
    target.CellWidth = data->CellWidth;
    target.CellHeight = data->CellHeight;
    target.PlaneZ = layerRef->ZOffset;
    target.Valid = true;
    return target;
}

bool TileMapPaintTool::RayToCell(const PaintTarget& target, const GizmoRay& ray, int32& outCellX,
                                 int32& outCellY) const
{
    if (!target.Valid)
        return false;

    // Intersect in the tilemap's own space: the layer plane is local z = ZOffset
    // whatever the entity's rotation, so one inverse transform replaces having
    // to build a world-space plane.
    const auto& inv = target.WorldToLocal.GetGLM();
    const glm::vec4 localOrigin =
        inv * glm::vec4(ray.origin.x, ray.origin.y, ray.origin.z, 1.0f);
    const glm::vec4 localDir =
        inv * glm::vec4(ray.direction.x, ray.direction.y, ray.direction.z, 0.0f);

    constexpr float32 kParallelEpsilon = 1e-6f;
    if (std::abs(localDir.z) < kParallelEpsilon)
        return false;

    const float32 t = (target.PlaneZ - localOrigin.z) / localDir.z;
    if (t <= 0.0f || !std::isfinite(t))
        return false;

    const float32 localX = localOrigin.x + localDir.x * t;
    const float32 localY = localOrigin.y + localDir.y * t;
    outCellX = static_cast<int32>(std::floor(localX / target.CellWidth));
    outCellY = static_cast<int32>(std::floor(localY / target.CellHeight));
    return true;
}

void TileMapPaintTool::BeginStroke(const char* name)
{
    m_StrokeActive = true;
    m_StrokeName = name;
    m_StrokeEdits.clear();
    m_StrokeCells.clear();
    m_HasLastCell = false;
    // The palette may have changed the stamp or tileset since the last stroke.
    m_ScatterWeights.clear();
    // Arming the terrain brush per stroke picks up a tileset or terrain-set
    // change; it keeps an already-loaded tileset, so this is not a file read.
    if (m_EditContext && m_EditContext->Tool() == TileMapTool::Terrain)
        (void)m_TerrainBrush.Begin(m_EditContext->TileSet(), m_EditContext->TerrainSet());
}

void TileMapPaintTool::WriteCell(const PaintTarget& target, int32 cellX, int32 cellY,
                                 TileMap::TileCell cell)
{
    TileMapECS::TileMapRuntimeStore* store = TileMapECS::TileMapRuntimeStore::TryGet();
    if (!store)
        return;

    const TileMap::TileCell before = store->GetCell(target.MapGuid, target.LayerId, cellX, cellY);
    if (before == cell)
        return;
    const TileMap::TileMapData* map = store->GetMap(target.MapGuid);
    const TileMap::TileMapLayerData* layer = map ? map->FindLayer(target.LayerId) : nullptr;
    const bool wasAuto = layer && layer->IsAutoCell(cellX, cellY);
    if (!store->SetCell(target.MapGuid, target.LayerId, cellX, cellY, cell))
        return;

    TileMapEditCommand::CellDelta delta{};
    delta.CellX = cellX;
    delta.CellY = cellY;
    delta.Before = before;
    delta.After = cell;
    delta.AutoBefore = wasAuto;
    delta.AutoAfter = false;
    LayerEditFor(target.LayerId).Cells.push_back(delta);
}

TileMapEditCommand::LayerEdit& TileMapPaintTool::LayerEditFor(uint32 layerId)
{
    for (TileMapEditCommand::LayerEdit& edit : m_StrokeEdits)
    {
        if (edit.LayerId == layerId)
            return edit;
    }
    TileMapEditCommand::LayerEdit edit;
    edit.LayerId = layerId;
    m_StrokeEdits.push_back(std::move(edit));
    return m_StrokeEdits.back();
}

void TileMapPaintTool::RecordDressedChanges(
    const std::vector<TileMapECS::TileMapDressedChange>& changes)
{
    for (const TileMapECS::TileMapDressedChange& dressed : changes)
    {
        TileMapEditCommand::CellDelta delta{};
        delta.CellX = dressed.Change.Cell.X;
        delta.CellY = dressed.Change.Cell.Y;
        delta.Before = dressed.Change.Before;
        delta.After = dressed.Change.After;
        delta.AutoBefore = dressed.Change.AutoBefore;
        delta.AutoAfter = dressed.Change.AutoAfter;
        LayerEditFor(dressed.LayerId).Cells.push_back(delta);
    }
}

void TileMapPaintTool::WriteIntGridValue(const PaintTarget& target, int32 cellX, int32 cellY,
                                         uint8 value)
{
    TileMapECS::TileMapRuntimeStore* store = TileMapECS::TileMapRuntimeStore::TryGet();
    if (!store)
        return;

    const uint8 before = store->GetIntGridValue(target.MapGuid, target.LayerId, cellX, cellY);
    if (before == value)
        return;
    if (!store->SetIntGridValue(target.MapGuid, target.LayerId, cellX, cellY, value))
        return;

    TileMapEditCommand::IntGridDelta delta{};
    delta.CellX = cellX;
    delta.CellY = cellY;
    delta.Before = before;
    delta.After = value;
    LayerEditFor(target.LayerId).IntGridValues.push_back(delta);

    // Re-dress per painted cell rather than per stroke: the point of rules is
    // that the art appears under the brush while it moves.
    m_DressedScratch.clear();
    if (store->DressLayersFromSource(target.MapGuid, target.LayerId, {TileMap::CellCoord{cellX, cellY}},
                                     m_DressedScratch))
    {
        RecordDressedChanges(m_DressedScratch);
    }
}

void TileMapPaintTool::IntGridPaintAt(const PaintTarget& target, int32 cellX, int32 cellY)
{
    WriteIntGridValue(target, cellX, cellY, m_EditContext->IntGridValue());
}

void TileMapPaintTool::RebuildScatterWeights()
{
    m_ScatterWeights.clear();
    m_ScatterWeightTotal = 0.0f;
    if (!m_EditContext)
        return;

    // An unresolvable tileset degrades to a uniform pick rather than painting
    // nothing: the stamp's tile ids are still valid cells to write.
    std::unique_ptr<TileMap::TileSetAsset> tileSet;
    auto& engine = EngineCore::GetInstance();
    AssetMetadata meta{};
    if (engine.IsInitialized() && !m_EditContext->TileSet().IsNull() &&
        engine.GetAssetManager().GetRegistry().TryGetAssetMetadata(m_EditContext->TileSet(), meta) &&
        !meta.Path.empty())
    {
        auto candidate = std::make_unique<TileMap::TileSetAsset>(m_EditContext->TileSet(), meta.Path);
        if (candidate->Load())
            tileSet = std::move(candidate);
    }

    for (const TileMap::TileCell cell : m_EditContext->Stamp().Cells)
    {
        if (TileMap::CellIsEmpty(cell))
            continue;
        const TileMap::TileSetTile* tile =
            tileSet ? tileSet->FindTile(TileMap::CellTileId(cell)) : nullptr;
        const float32 weight = tile ? std::max(0.0f, tile->Probability) : 1.0f;
        if (weight <= 0.0f)
            continue;
        m_ScatterWeights.push_back(ScatterEntry{cell, weight});
        m_ScatterWeightTotal += weight;
    }
}

TileMap::TileCell TileMapPaintTool::StampCellAt(int32 offsetX, int32 offsetY)
{
    if (m_EditContext->RandomScatter())
    {
        // Weighted pick over the stamp's non-empty cells, using each tile's
        // authored Probability. The weights are cached per gesture: resolving
        // and re-decoding the tileset per painted cell would turn a drag into
        // one file read per cell.
        if (m_ScatterWeights.empty())
            RebuildScatterWeights();
        if (m_ScatterWeightTotal <= 0.0f)
            return TileMap::kEmptyCell;

        constexpr float32 kRandomDenominator = 4294967295.0f;
        const float32 pick =
            (static_cast<float32>(NextRandom(m_ScatterState)) / kRandomDenominator) *
            m_ScatterWeightTotal;
        float32 running = 0.0f;
        for (const ScatterEntry& entry : m_ScatterWeights)
        {
            running += entry.Weight;
            if (pick <= running)
                return entry.Cell;
        }
        // Float summation can leave `pick` a hair past the final running total.
        return m_ScatterWeights.back().Cell;
    }

    const TileStamp& stamp = m_EditContext->Stamp();
    if (stamp.IsEmpty())
        return TileMap::kEmptyCell;

    // Tile the stamp across the region. The modulo is made non-negative first so
    // a region extending to negative cell coordinates keeps the same phase.
    const int32 column = ((offsetX % stamp.Width) + stamp.Width) % stamp.Width;
    const int32 rowFromBottom = ((offsetY % stamp.Height) + stamp.Height) % stamp.Height;
    // Stamp rows read top-down while cell Y grows upward.
    return stamp.At(column, stamp.Height - 1 - rowFromBottom);
}

void TileMapPaintTool::StampAt(const PaintTarget& target, int32 cellX, int32 cellY)
{
    const TileStamp& stamp = m_EditContext->Stamp();
    if (stamp.IsEmpty())
        return;

    if (m_EditContext->RandomScatter())
    {
        WriteCell(target, cellX, cellY, StampCellAt(0, 0));
        return;
    }

    for (int32 row = 0; row < stamp.Height; ++row)
        for (int32 column = 0; column < stamp.Width; ++column)
        {
            const TileMap::TileCell cell = stamp.At(column, row);
            if (TileMap::CellIsEmpty(cell))
                continue;
            // Stamp rows read top-down while cell Y grows upward, so the
            // footprint lands the same way up as it looked in the palette.
            WriteCell(target, cellX + column, cellY + (stamp.Height - 1 - row), cell);
        }
}

void TileMapPaintTool::TerrainPaintAt(const PaintTarget& target, int32 cellX, int32 cellY)
{
    if (!m_TerrainBrush.IsActive())
        return;

    m_TerrainWrites.clear();
    m_TerrainBrush.Paint(target.MapGuid, target.LayerId, cellX, cellY, m_EditContext->Terrain(),
                         m_TerrainWrites);
    for (const Editor::TerrainCellWrite& write : m_TerrainWrites)
        WriteCell(target, write.CellX, write.CellY, write.Cell);
}

void TileMapPaintTool::EraseAt(const PaintTarget& target, int32 cellX, int32 cellY)
{
    WriteCell(target, cellX, cellY, TileMap::kEmptyCell);
}

void TileMapPaintTool::PickAt(const PaintTarget& target, int32 cellX, int32 cellY)
{
    TileMapECS::TileMapRuntimeStore* store = TileMapECS::TileMapRuntimeStore::TryGet();
    if (!store)
        return;
    const TileMap::TileCell cell = store->GetCell(target.MapGuid, target.LayerId, cellX, cellY);
    if (TileMap::CellIsEmpty(cell))
        return;

    TileStamp stamp;
    stamp.Width = 1;
    stamp.Height = 1;
    stamp.Cells.assign(1, cell);
    m_EditContext->SetStamp(std::move(stamp));
}

void TileMapPaintTool::FillFrom(const PaintTarget& target, int32 cellX, int32 cellY)
{
    TileMapECS::TileMapRuntimeStore* store = TileMapECS::TileMapRuntimeStore::TryGet();
    const TileStamp& stamp = m_EditContext->Stamp();
    if (!store || stamp.IsEmpty())
        return;
    const TileMap::TileMapData* data = store->GetMap(target.MapGuid);
    if (!data)
        return;
    const TileMap::TileMapLayerData* layer = data->FindLayer(target.LayerId);
    if (!layer)
        return;

    // Bound the flood to the layer's authored chunk extent plus one ring.
    const std::vector<TileMap::ChunkCoord> chunks = layer->SortedChunkCoords();
    int32 minX = cellX;
    int32 minY = cellY;
    int32 maxX = cellX;
    int32 maxY = cellY;
    for (const TileMap::ChunkCoord coord : chunks)
    {
        minX = std::min(minX, coord.X * TileMap::kChunkSize);
        minY = std::min(minY, coord.Y * TileMap::kChunkSize);
        maxX = std::max(maxX, coord.X * TileMap::kChunkSize + TileMap::kChunkSize - 1);
        maxY = std::max(maxY, coord.Y * TileMap::kChunkSize + TileMap::kChunkSize - 1);
    }
    minX -= kFillBorderRing;
    minY -= kFillBorderRing;
    maxX += kFillBorderRing;
    maxY += kFillBorderRing;

    const TileMap::TileCell seed = store->GetCell(target.MapGuid, target.LayerId, cellX, cellY);
    // A single-tile, non-scattering stamp that already matches the seed would
    // repaint the region with what it holds — bail rather than walk it.
    if (stamp.Width == 1 && stamp.Height == 1 && !m_EditContext->RandomScatter() &&
        seed == stamp.Cells.front())
    {
        return;
    }

    std::unordered_set<uint64> visited;
    std::vector<std::pair<int32, int32>> frontier{{cellX, cellY}};
    int32 painted = 0;
    while (!frontier.empty() && painted < kMaxFillCells)
    {
        const auto [x, y] = frontier.back();
        frontier.pop_back();
        if (x < minX || x > maxX || y < minY || y > maxY)
            continue;
        const uint64 key = (static_cast<uint64>(static_cast<uint32>(x)) << 32) |
                           static_cast<uint64>(static_cast<uint32>(y));
        if (!visited.insert(key).second)
            continue;
        if (store->GetCell(target.MapGuid, target.LayerId, x, y) != seed)
            continue;

        WriteCell(target, x, y, StampCellAt(x - cellX, y - cellY));
        ++painted;
        frontier.push_back({x + 1, y});
        frontier.push_back({x - 1, y});
        frontier.push_back({x, y + 1});
        frontier.push_back({x, y - 1});
    }
}

void TileMapPaintTool::FillRect(const PaintTarget& target, const CellRect& rect)
{
    if (!rect.Valid)
        return;
    if (m_EditContext->Tool() == TileMapTool::Terrain)
    {
        // Terrain has no stamp to tile: each cell is resolved against the field
        // its predecessors left behind, exactly as a freehand drag would.
        for (int32 y = rect.MinY; y <= rect.MaxY; ++y)
            for (int32 x = rect.MinX; x <= rect.MaxX; ++x)
                TerrainPaintAt(target, x, y);
        return;
    }

    const bool erasing = m_EditContext->Tool() == TileMapTool::Eraser;
    if (!erasing && m_EditContext->Stamp().IsEmpty())
        return;

    for (int32 y = rect.MinY; y <= rect.MaxY; ++y)
        for (int32 x = rect.MinX; x <= rect.MaxX; ++x)
        {
            const TileMap::TileCell cell =
                erasing ? TileMap::kEmptyCell : StampCellAt(x - rect.MinX, y - rect.MinY);
            // A tiled stamp with holes leaves those cells alone rather than
            // punching them out, which is what makes a patterned stamp usable
            // as a rectangle brush.
            if (!erasing && TileMap::CellIsEmpty(cell))
                continue;
            WriteCell(target, x, y, cell);
        }
}

TileMapPaintTool::CellBlock TileMapPaintTool::ReadBlock(const PaintTarget& target,
                                                        const CellRect& rect) const
{
    CellBlock block;
    TileMapECS::TileMapRuntimeStore* store = TileMapECS::TileMapRuntimeStore::TryGet();
    if (!store || !rect.Valid)
        return block;

    block.Width = rect.Width();
    block.Height = rect.Height();
    block.Cells.reserve(static_cast<size_t>(block.Width) * static_cast<size_t>(block.Height));
    for (int32 y = rect.MinY; y <= rect.MaxY; ++y)
        for (int32 x = rect.MinX; x <= rect.MaxX; ++x)
            block.Cells.push_back(store->GetCell(target.MapGuid, target.LayerId, x, y));
    return block;
}

void TileMapPaintTool::ClearSelection()
{
    m_Selection = CellRect{};
    m_Overlay.ClearSelection();
}

void TileMapPaintTool::SetSelectionForAutomation(int32 x0, int32 y0, int32 x1, int32 y1)
{
    m_Selection = CellRect::FromCorners(x0, y0, x1, y1);
    m_Overlay.UpdateSelection(ResolveTarget(), m_Selection, m_PendingRect);
}

bool TileMapPaintTool::CopySelection()
{
    const PaintTarget target = ResolveTarget();
    if (!target.Valid || !m_Selection.Valid)
        return false;
    m_Clipboard = ReadBlock(target, m_Selection);
    return !m_Clipboard.IsEmpty();
}

bool TileMapPaintTool::PasteAt(int32 cellX, int32 cellY)
{
    const PaintTarget target = ResolveTarget();
    if (!target.Valid || m_Clipboard.IsEmpty())
        return false;

    BeginStroke("Paste Tiles");
    for (int32 y = 0; y < m_Clipboard.Height; ++y)
        for (int32 x = 0; x < m_Clipboard.Width; ++x)
            WriteCell(target, cellX + x, cellY + y, m_Clipboard.At(x, y));
    CommitStroke();

    // Leave the selection over what was just pasted, so a paste can be nudged
    // or re-copied without re-marqueeing it.
    m_Selection = CellRect::FromCorners(cellX, cellY, cellX + m_Clipboard.Width - 1,
                                        cellY + m_Clipboard.Height - 1);
    m_Overlay.UpdateSelection(target, m_Selection, m_PendingRect);
    return true;
}

bool TileMapPaintTool::MoveSelectionBy(int32 deltaX, int32 deltaY)
{
    const PaintTarget target = ResolveTarget();
    if (!target.Valid || !m_Selection.Valid || (deltaX == 0 && deltaY == 0))
        return false;

    const CellBlock block = ReadBlock(target, m_Selection);
    if (block.IsEmpty())
        return false;

    BeginStroke("Move Tiles");
    // Clear the source first, then write the destination: doing it in this order
    // means an overlapping move keeps the cells that land back inside the source.
    for (int32 y = 0; y < block.Height; ++y)
        for (int32 x = 0; x < block.Width; ++x)
            WriteCell(target, m_Selection.MinX + x, m_Selection.MinY + y, TileMap::kEmptyCell);
    for (int32 y = 0; y < block.Height; ++y)
        for (int32 x = 0; x < block.Width; ++x)
            WriteCell(target, m_Selection.MinX + deltaX + x, m_Selection.MinY + deltaY + y,
                      block.At(x, y));
    CommitStroke();

    m_Selection = CellRect::FromCorners(m_Selection.MinX + deltaX, m_Selection.MinY + deltaY,
                                        m_Selection.MaxX + deltaX, m_Selection.MaxY + deltaY);
    m_Overlay.UpdateSelection(target, m_Selection, m_PendingRect);
    return true;
}

bool TileMapPaintTool::FillRectForAutomation(int32 x0, int32 y0, int32 x1, int32 y1)
{
    const PaintTarget target = ResolveTarget();
    if (!target.Valid)
        return false;
    m_ScatterWeights.clear();
    BeginStroke("Fill Tile Rect");
    FillRect(target, CellRect::FromCorners(x0, y0, x1, y1));
    CommitStroke();
    return true;
}

TileMapECS::TileMapRenderFeature* TileMapPaintTool::PreviewFeature() const
{
    // Looked up per use rather than cached: the feature is registered by the
    // renderer on its own schedule and can be rebuilt with the device.
    return m_RenderServices != nullptr
               ? m_RenderServices->GetFeature<TileMapECS::TileMapRenderFeature>()
               : nullptr;
}

void TileMapPaintTool::UpdatePreview(const PaintTarget& target, int32 cellX, int32 cellY)
{
    TileMapECS::TileMapRenderFeature* feature = PreviewFeature();
    if (feature == nullptr || m_EditContext == nullptr || !target.Valid)
        return;
    const TileMapTool tool = m_EditContext->Tool();
    const TileStamp& stamp = m_EditContext->Stamp();
    // Only the tools that lay the stamp down as-is get a ghost: the eraser
    // removes, the picker reads, scatter and terrain resolve per cell at write.
    const bool stamps = (tool == TileMapTool::Brush || tool == TileMapTool::Rect) &&
                        !stamp.IsEmpty() && !m_EditContext->RandomScatter();
    if (!stamps)
    {
        ClearPreview();
        return;
    }

    TileMapECS::TileMapPreview preview;
    preview.Map = target.MapGuid;
    preview.LayerId = target.LayerId;
    preview.OriginX = cellX;
    preview.OriginY = cellY;
    preview.Instances.reserve(static_cast<size_t>(stamp.Width) * static_cast<size_t>(stamp.Height));
    for (int32 row = 0; row < stamp.Height; ++row)
        for (int32 column = 0; column < stamp.Width; ++column)
        {
            const TileMap::TileCell cell = stamp.At(column, row);
            if (TileMap::CellIsEmpty(cell))
                continue;
            // Stamp rows read top-down while cell Y grows upward, the same
            // flip StampAt applies. A footprint past one chunk is clipped: the
            // ghost is addressed like a chunk's instances.
            const int32 localX = column;
            const int32 localY = stamp.Height - 1 - row;
            if (localX >= TileMap::kChunkSize || localY >= TileMap::kChunkSize)
                continue;
            TileMapECS::TileInstance instance;
            instance.LocalIndex = static_cast<uint32>(localY * TileMap::kChunkSize + localX);
            instance.Cell = cell;
            preview.Instances.push_back(instance);
        }
    feature->SetPreview(std::move(preview));
}

std::string_view TileMapPaintTool::StatusText() const
{
    if (m_EditContext == nullptr)
        return {};
    if (!m_EditContext->HasPaintTarget())
        return "Select a tile layer to paint into";
    if (m_HoverErases || m_StrokeErases)
        return "Erasing — release Ctrl to paint";
    const TileMapTool tool = m_EditContext->Tool();
    const bool needsStamp = tool == TileMapTool::Brush || tool == TileMapTool::Rect ||
                            tool == TileMapTool::Fill;
    if (needsStamp && m_EditContext->Stamp().IsEmpty())
        return "Pick a tile in the Tile Palette to paint";
    if (tool == TileMapTool::Terrain && m_EditContext->TerrainSet() < 0)
        return "Pick a terrain in the Tile Palette to paint";
    return {};
}

void TileMapPaintTool::ClearPreview()
{
    if (TileMapECS::TileMapRenderFeature* feature = PreviewFeature())
        feature->ClearPreview();
}

void TileMapPaintTool::CancelDrag()
{
    m_DragMode = DragMode::None;
    m_PendingRect = CellRect{};
    m_MoveBlock = CellBlock{};
    m_Overlay.ClearPending();
}

void TileMapPaintTool::StrokeLine(const PaintTarget& target, int32 fromX, int32 fromY, int32 toX,
                                  int32 toY)
{
    // Bresenham over cells: a fast drag delivers sparse pointer samples, and
    // stamping only at those samples leaves gaps in the painted run.
    int32 x = fromX;
    int32 y = fromY;
    const int32 dx = std::abs(toX - fromX);
    const int32 dy = -std::abs(toY - fromY);
    const int32 stepX = fromX < toX ? 1 : -1;
    const int32 stepY = fromY < toY ? 1 : -1;
    int32 error = dx + dy;

    while (true)
    {
        ApplyAt(target, x, y);
        if (x == toX && y == toY)
            break;
        const int32 doubled = error * 2;
        if (doubled >= dy)
        {
            error += dy;
            x += stepX;
        }
        if (doubled <= dx)
        {
            error += dx;
            y += stepY;
        }
    }
}

void TileMapPaintTool::ApplyAt(const PaintTarget& target, int32 cellX, int32 cellY)
{
    // Each cell takes one application per stroke: the pick is what it was the
    // first time the stroke reached the cell.
    if (!m_StrokeCells.insert(PackCell(cellX, cellY)).second)
        return;
    if (m_StrokeErases)
    {
        EraseAt(target, cellX, cellY);
        return;
    }
    switch (m_EditContext->Tool())
    {
    case TileMapTool::Eraser:
        EraseAt(target, cellX, cellY);
        break;
    case TileMapTool::Picker:
        PickAt(target, cellX, cellY);
        break;
    case TileMapTool::Terrain:
        TerrainPaintAt(target, cellX, cellY);
        break;
    case TileMapTool::IntGrid:
        IntGridPaintAt(target, cellX, cellY);
        break;
    case TileMapTool::Fill:
        // A marquee turns the bucket into a bounded region fill: the flood's
        // shape is whatever the user already framed, which is the only way to
        // fill an area that existing tiles do not enclose.
        if (m_Selection.Contains(cellX, cellY))
            FillRect(target, m_Selection);
        else
            FillFrom(target, cellX, cellY);
        break;
    default:
        StampAt(target, cellX, cellY);
        break;
    }
}

void TileMapPaintTool::CommitStroke()
{
    if (!m_StrokeActive)
        return;
    m_StrokeActive = false;
    m_StrokeErases = false;
    m_HasLastCell = false;

    std::erase_if(m_StrokeEdits, [](const TileMapEditCommand::LayerEdit& edit)
                  { return edit.IsEmpty(); });
    if (m_StrokeEdits.empty() || !m_UndoRedo || !m_EditContext)
    {
        m_StrokeEdits.clear();
        return;
    }

    m_UndoRedo->CommitAlreadyApplied(std::make_unique<TileMapEditCommand>(
        m_StrokeName, m_EditContext->TargetMap(), std::move(m_StrokeEdits)));
    m_StrokeEdits.clear();
}

void TileMapPaintTool::OnPointerEvent(const ScenePointerEvent& event)
{
    if (!m_EditContext)
        return;

    const PaintTarget target = ResolveTarget();
    if (!target.Valid)
    {
        m_Overlay.HideCursor();
    ClearPreview();
        return;
    }

    int32 cellX = 0;
    int32 cellY = 0;
    if (!RayToCell(target, event.ray, cellX, cellY))
    {
        m_Overlay.HideCursor();
    ClearPreview();
        return;
    }
    m_HoverCellX = cellX;
    m_HoverCellY = cellY;
    m_HasHoverCell = true;
    // Ctrl/Cmd alone turns the armed stamping tool into an eraser for the
    // gesture (Shift is the line, Ctrl+Shift the rectangle, Alt the picker).
    const TileMapTool hoverTool = m_EditContext->Tool();
    m_HoverErases = event.primary && !event.shift && !event.alt &&
                    (hoverTool == TileMapTool::Brush || hoverTool == TileMapTool::Rect ||
                     hoverTool == TileMapTool::Fill || hoverTool == TileMapTool::Terrain);
    m_Overlay.UpdateCursor(target, *m_EditContext, cellX, cellY, m_HoverErases);
    if (m_HoverErases)
        ClearPreview();
    else
        UpdatePreview(target, cellX, cellY);

    if (event.button != PointerButton::Left)
    {
        m_Overlay.UpdateSelection(target, m_Selection, m_PendingRect);
        return;
    }

    switch (event.phase)
    {
    case PointerPhase::Down:
    {
        // Alt is the transient eyedropper the whole editor expects, whatever
        // tool is armed; it must not open an undo entry, since it writes no cell.
        if (event.alt)
        {
            PickAt(target, cellX, cellY);
            break;
        }

        m_DragStartX = cellX;
        m_DragStartY = cellY;
        // Weights are per gesture, and the palette may have changed the stamp
        // or tileset since the last one.
        m_ScatterWeights.clear();
        m_StrokeErases = m_HoverErases;

        const TileMapTool tool = m_EditContext->Tool();
        if (m_StrokeErases)
        {
            m_DragMode = DragMode::Paint;
            BeginStroke("Erase Tiles");
            ApplyAt(target, cellX, cellY);
            m_LastCellX = cellX;
            m_LastCellY = cellY;
            m_HasLastCell = true;
            break;
        }
        if (tool == TileMapTool::Marquee)
        {
            // Dragging from inside an existing selection moves it; dragging
            // anywhere else starts a new one.
            if (m_Selection.Contains(cellX, cellY))
            {
                m_DragMode = DragMode::MoveSelection;
                m_MoveOrigin = m_Selection;
                m_MoveBlock = ReadBlock(target, m_Selection);
                m_MoveDeltaX = 0;
                m_MoveDeltaY = 0;
                BeginStroke("Move Tiles");
            }
            else
            {
                m_DragMode = DragMode::Marquee;
                m_PendingRect = CellRect::FromCorners(cellX, cellY, cellX, cellY);
            }
            break;
        }

        if (tool == TileMapTool::Picker)
        {
            PickAt(target, cellX, cellY);
            break;
        }

        if (tool == TileMapTool::Fill)
        {
            BeginStroke("Fill Tiles");
            ApplyAt(target, cellX, cellY);
            CommitStroke();
            break;
        }

        // Rect as its own tool, or as the Ctrl/Cmd+Shift modifier over a brush.
        if (tool == TileMapTool::Rect || (event.primary && event.shift))
        {
            m_DragMode = DragMode::Rect;
            m_PendingRect = CellRect::FromCorners(cellX, cellY, cellX, cellY);
            break;
        }

        if (event.shift)
        {
            m_DragMode = DragMode::Line;
            m_PendingRect = CellRect::FromCorners(cellX, cellY, cellX, cellY);
            break;
        }

        m_DragMode = DragMode::Paint;
        BeginStroke(StrokeLabel(tool, "Erase Tiles", "Paint Terrain", "Paint Tiles"));
        ApplyAt(target, cellX, cellY);
        m_LastCellX = cellX;
        m_LastCellY = cellY;
        m_HasLastCell = true;
        break;
    }
    case PointerPhase::Move:
    {
        switch (m_DragMode)
        {
        case DragMode::Paint:
            if (m_HasLastCell && (cellX != m_LastCellX || cellY != m_LastCellY))
                StrokeLine(target, m_LastCellX, m_LastCellY, cellX, cellY);
            m_LastCellX = cellX;
            m_LastCellY = cellY;
            m_HasLastCell = true;
            break;
        case DragMode::Marquee:
        case DragMode::Rect:
        case DragMode::Line:
            m_PendingRect = CellRect::FromCorners(m_DragStartX, m_DragStartY, cellX, cellY);
            break;
        case DragMode::MoveSelection:
        {
            const int32 deltaX = cellX - m_DragStartX;
            const int32 deltaY = cellY - m_DragStartY;
            if (deltaX == m_MoveDeltaX && deltaY == m_MoveDeltaY)
                break;
            // Re-place from the lifted copy every time rather than moving what
            // the previous frame wrote, so dragging back and forth is lossless.
            for (int32 y = 0; y < m_MoveBlock.Height; ++y)
                for (int32 x = 0; x < m_MoveBlock.Width; ++x)
                    WriteCell(target, m_MoveOrigin.MinX + m_MoveDeltaX + x,
                              m_MoveOrigin.MinY + m_MoveDeltaY + y, TileMap::kEmptyCell);
            for (int32 y = 0; y < m_MoveBlock.Height; ++y)
                for (int32 x = 0; x < m_MoveBlock.Width; ++x)
                    WriteCell(target, m_MoveOrigin.MinX + deltaX + x,
                              m_MoveOrigin.MinY + deltaY + y, m_MoveBlock.At(x, y));
            m_MoveDeltaX = deltaX;
            m_MoveDeltaY = deltaY;
            m_Selection = CellRect::FromCorners(
                m_MoveOrigin.MinX + deltaX, m_MoveOrigin.MinY + deltaY,
                m_MoveOrigin.MaxX + deltaX, m_MoveOrigin.MaxY + deltaY);
            break;
        }
        case DragMode::None:
            break;
        }
        break;
    }
    case PointerPhase::Up:
    {
        switch (m_DragMode)
        {
        case DragMode::Marquee:
            m_Selection = m_PendingRect;
            break;
        case DragMode::Rect:
            BeginStroke(StrokeLabel(m_EditContext->Tool(), "Erase Tile Rect",
                                    "Paint Terrain Rect", "Fill Tile Rect"));
            FillRect(target, m_PendingRect);
            CommitStroke();
            break;
        case DragMode::Line:
            BeginStroke(StrokeLabel(m_EditContext->Tool(), "Erase Tile Line",
                                    "Paint Terrain Line", "Paint Tile Line"));
            StrokeLine(target, m_DragStartX, m_DragStartY, cellX, cellY);
            CommitStroke();
            break;
        case DragMode::Paint:
        case DragMode::MoveSelection:
            CommitStroke();
            break;
        case DragMode::None:
            break;
        }
        CancelDrag();
        break;
    }
    }

    m_Overlay.UpdateSelection(target, m_Selection, m_PendingRect);
}

void TileMapPaintTool::OnKeyEvent(const SceneKeyEvent& event)
{
    if (!event.pressed || !m_EditContext)
        return;

    const int key = static_cast<int>(event.keyCode);
    const int mods = static_cast<int>(event.mods);
    auto matches = [key, mods](const char* name)
    { return Editor::MatchesCatalogShortcut(kShortcutGroup, name, key, mods); };

    for (const TileMapTool tool : kHotkeyTools)
    {
        if (matches(TileMapToolShortcutName(tool)))
        {
            m_EditContext->SetTool(tool);
            return;
        }
    }

    if (matches("Copy Tiles"))
    {
        (void)CopySelection();
        return;
    }
    if (matches("Cut Tiles"))
    {
        if (CopySelection())
            EraseSelection("Cut Tiles");
        return;
    }
    if (matches("Paste Tiles"))
    {
        // Paste lands under the cursor when there is one, so the block goes
        // where the user is looking rather than back where it was copied.
        if (m_HasHoverCell)
            (void)PasteAt(m_HoverCellX, m_HoverCellY);
        else if (m_Selection.Valid)
            (void)PasteAt(m_Selection.MinX, m_Selection.MinY);
        return;
    }

    // Orientation hotkeys mirror the palette's flip/rotate buttons so a stamp
    // can be reoriented without leaving the viewport.
    if (matches("Flip Horizontal"))
        m_EditContext->FlipStampHorizontal();
    else if (matches("Flip Vertical"))
        m_EditContext->FlipStampVertical();
    else if (matches("Rotate Stamp"))
        m_EditContext->RotateStampClockwise();
    else if (matches("Clear Selection"))
        ClearSelection();
    else if (matches("Erase Selection"))
        EraseSelection("Erase Tile Selection");
    else if (matches("Nudge Left"))
        (void)MoveSelectionBy(-1, 0);
    else if (matches("Nudge Right"))
        (void)MoveSelectionBy(1, 0);
    else if (matches("Nudge Up"))
        (void)MoveSelectionBy(0, 1);
    else if (matches("Nudge Down"))
        (void)MoveSelectionBy(0, -1);
}

void TileMapPaintTool::EraseSelection(const char* strokeName)
{
    if (!m_Selection.Valid)
        return;
    const PaintTarget target = ResolveTarget();
    if (!target.Valid)
        return;
    BeginStroke(strokeName);
    for (int32 y = m_Selection.MinY; y <= m_Selection.MaxY; ++y)
        for (int32 x = m_Selection.MinX; x <= m_Selection.MaxX; ++x)
            WriteCell(target, x, y, TileMap::kEmptyCell);
    CommitStroke();
}

void TileMapPaintTool::GatherGizmos(GizmoCollector& collector)
{
    // Rebuilt here rather than on pointer events: the tint has to survive a
    // frame in which the pointer never moved, and an undo that changed it.
    m_Overlay.UpdateIntGridTint(ResolveTarget(), m_EditContext.get());
    m_Overlay.Gather(collector);
}

bool TileMapPaintTool::PaintCellForAutomation(int32 cellX, int32 cellY, bool strokeBegin,
                                              bool strokeEnd, bool erase)
{
    if (!m_EditContext)
        return false;
    const PaintTarget target = ResolveTarget();
    if (!target.Valid)
        return false;

    if (strokeBegin)
    {
        m_StrokeErases = erase;
        BeginStroke(erase ? "Erase Tiles"
                          : StrokeLabel(m_EditContext->Tool(), "Erase Tiles", "Paint Terrain",
                                        "Paint Tiles"));
    }
    ApplyAt(target, cellX, cellY);
    if (strokeEnd)
        CommitStroke();
    return true;
}

} // namespace Editor::SceneTools
} // namespace GameEngine
