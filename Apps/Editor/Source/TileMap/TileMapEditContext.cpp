#include "TileMap/TileMapEditContext.h"

#include "TileMap/TileCellOrientation.h"

#include <utility>

namespace GameEngine
{
namespace Editor
{

bool TileStamp::IsEmpty() const
{
    return Width <= 0 || Height <= 0 || Cells.empty();
}

TileMap::TileCell TileStamp::At(int32 column, int32 row) const
{
    if (column < 0 || row < 0 || column >= Width || row >= Height)
        return TileMap::kEmptyCell;
    const size_t index = static_cast<size_t>(row) * static_cast<size_t>(Width) +
                         static_cast<size_t>(column);
    return index < Cells.size() ? Cells[index] : TileMap::kEmptyCell;
}

void TileMapEditContext::SetTileSet(const GUID& tileSet)
{
    if (m_TileSet == tileSet)
        return;
    m_TileSet = tileSet;
    // Stamp cell ids and terrain indices both index the old tileset, so neither
    // can survive the switch.
    m_Stamp = TileStamp{};
    m_TerrainSet = -1;
    m_Terrain = -1;
    ++m_Version;
}

void TileMapEditContext::SetStamp(TileStamp stamp)
{
    m_Stamp = std::move(stamp);
    ++m_Version;
}

const char* TileMapToolShortcutName(TileMapTool tool)
{
    switch (tool)
    {
    case TileMapTool::Brush: return "Brush Tool";
    case TileMapTool::Eraser: return "Eraser Tool";
    case TileMapTool::Picker: return "Picker Tool";
    case TileMapTool::Fill: return "Fill Tool";
    case TileMapTool::Rect: return "Rect Tool";
    case TileMapTool::Marquee: return "Marquee Tool";
    case TileMapTool::Terrain: return "Terrain Tool";
    case TileMapTool::IntGrid: return "IntGrid Tool";
    }
    return "Brush Tool";
}

void TileMapEditContext::SetTool(TileMapTool tool)
{
    if (m_Tool == tool)
        return;
    m_Tool = tool;
    ++m_Version;
}

void TileMapEditContext::SetRandomScatter(bool enabled)
{
    if (m_RandomScatter == enabled)
        return;
    m_RandomScatter = enabled;
    ++m_Version;
}

void TileMapEditContext::SetTerrain(int16 terrainSet, int16 terrain)
{
    if (m_TerrainSet == terrainSet && m_Terrain == terrain)
        return;
    m_TerrainSet = terrainSet;
    m_Terrain = terrain;
    ++m_Version;
}

void TileMapEditContext::SetIntGridValue(uint8 value)
{
    if (m_IntGridValue == value)
        return;
    m_IntGridValue = value;
    ++m_Version;
}

void TileMapEditContext::SetShowIntGridTint(bool show)
{
    if (m_ShowIntGridTint == show)
        return;
    m_ShowIntGridTint = show;
    ++m_Version;
}

void TileMapEditContext::SetTarget(ECS::EntityHandle entity, const GUID& mapGuid, uint32 layerId)
{
    if (m_TargetEntity == entity && m_TargetMap == mapGuid && m_TargetLayer == layerId)
        return;
    m_TargetEntity = entity;
    m_TargetMap = mapGuid;
    m_TargetLayer = layerId;
    ++m_Version;
}

void TileMapEditContext::ClearTarget()
{
    SetTarget(ECS::EntityHandle{}, GUID{}, 0);
}

bool TileMapEditContext::HasTargetLayer() const
{
    return !m_TargetMap.IsNull() && m_TargetLayer != 0;
}

bool TileMapEditContext::HasPaintTarget() const
{
    return m_TargetEntity.IsValid() && !m_TargetMap.IsNull() && m_TargetLayer != 0;
}

void TileMapEditContext::FlipStampHorizontal()
{
    if (m_Stamp.IsEmpty())
        return;
    TileStamp flipped = m_Stamp;
    for (int32 row = 0; row < m_Stamp.Height; ++row)
        for (int32 column = 0; column < m_Stamp.Width; ++column)
        {
            const TileMap::TileCell source = m_Stamp.At(m_Stamp.Width - 1 - column, row);
            flipped.Cells[static_cast<size_t>(row) * static_cast<size_t>(m_Stamp.Width) +
                          static_cast<size_t>(column)] = TileMap::FlipCellHorizontal(source);
        }
    SetStamp(std::move(flipped));
}

void TileMapEditContext::FlipStampVertical()
{
    if (m_Stamp.IsEmpty())
        return;
    TileStamp flipped = m_Stamp;
    for (int32 row = 0; row < m_Stamp.Height; ++row)
        for (int32 column = 0; column < m_Stamp.Width; ++column)
        {
            const TileMap::TileCell source = m_Stamp.At(column, m_Stamp.Height - 1 - row);
            flipped.Cells[static_cast<size_t>(row) * static_cast<size_t>(m_Stamp.Width) +
                          static_cast<size_t>(column)] = TileMap::FlipCellVertical(source);
        }
    SetStamp(std::move(flipped));
}

void TileMapEditContext::RotateStampClockwise()
{
    if (m_Stamp.IsEmpty())
        return;
    TileStamp rotated;
    rotated.Width = m_Stamp.Height;
    rotated.Height = m_Stamp.Width;
    rotated.Cells.resize(m_Stamp.Cells.size(), TileMap::kEmptyCell);
    // A quarter turn clockwise sends source (column, row) to (H-1-row, column).
    for (int32 row = 0; row < m_Stamp.Height; ++row)
        for (int32 column = 0; column < m_Stamp.Width; ++column)
        {
            const int32 destColumn = m_Stamp.Height - 1 - row;
            const int32 destRow = column;
            rotated.Cells[static_cast<size_t>(destRow) * static_cast<size_t>(rotated.Width) +
                          static_cast<size_t>(destColumn)] =
                TileMap::RotateCellClockwise(m_Stamp.At(column, row));
        }
    SetStamp(std::move(rotated));
}

} // namespace Editor
} // namespace GameEngine
