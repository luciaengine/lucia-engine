#include "TileMap/TileMapLayerData.h"

#include <algorithm>

namespace GameEngine
{
namespace TileMap
{

namespace
{

template <typename ChunkMap>
std::vector<ChunkCoord> SortedCoords(const ChunkMap& chunks)
{
    std::vector<ChunkCoord> coords;
    coords.reserve(chunks.size());
    for (const auto& [packed, chunk] : chunks)
        coords.push_back(UnpackChunkCoord(packed));
    std::sort(coords.begin(), coords.end(), [](ChunkCoord a, ChunkCoord b)
    {
        if (a.Y != b.Y)
            return a.Y < b.Y;
        return a.X < b.X;
    });
    return coords;
}

} // namespace

TileCell TileMapLayerData::GetCell(int32 cellX, int32 cellY) const
{
    const auto it = m_Chunks.find(PackChunkCoord(ChunkOfCell(cellX, cellY)));
    if (it == m_Chunks.end())
        return kEmptyCell;
    return it->second.Cells[LocalCellIndex(cellX, cellY)];
}

void TileMapLayerData::SetCell(int32 cellX, int32 cellY, TileCell cell)
{
    const uint64 key = PackChunkCoord(ChunkOfCell(cellX, cellY));
    if (CellIsEmpty(cell))
    {
        const auto it = m_Chunks.find(key);
        if (it == m_Chunks.end())
            return;
        it->second.Cells[LocalCellIndex(cellX, cellY)] = kEmptyCell;
        return;
    }
    m_Chunks[key].Cells[LocalCellIndex(cellX, cellY)] = cell;
}

uint8 TileMapLayerData::GetIntGridValue(int32 cellX, int32 cellY) const
{
    const auto it = m_IntGridChunks.find(PackChunkCoord(ChunkOfCell(cellX, cellY)));
    if (it == m_IntGridChunks.end())
        return 0;
    return it->second.Values[LocalCellIndex(cellX, cellY)];
}

void TileMapLayerData::SetIntGridValue(int32 cellX, int32 cellY, uint8 value)
{
    const uint64 key = PackChunkCoord(ChunkOfCell(cellX, cellY));
    if (value == 0)
    {
        const auto it = m_IntGridChunks.find(key);
        if (it == m_IntGridChunks.end())
            return;
        it->second.Values[LocalCellIndex(cellX, cellY)] = 0;
        return;
    }
    m_IntGridChunks[key].Values[LocalCellIndex(cellX, cellY)] = value;
}

bool TileMapLayerData::IsAutoCell(int32 cellX, int32 cellY) const
{
    const auto it = m_AutoChunks.find(PackChunkCoord(ChunkOfCell(cellX, cellY)));
    if (it == m_AutoChunks.end())
        return false;
    return it->second.Get(cellX, cellY);
}

void TileMapLayerData::SetAutoCell(int32 cellX, int32 cellY, bool automatic)
{
    const uint64 key = PackChunkCoord(ChunkOfCell(cellX, cellY));
    if (!automatic)
    {
        const auto it = m_AutoChunks.find(key);
        if (it == m_AutoChunks.end())
            return;
        it->second.Set(cellX, cellY, false);
        return;
    }
    m_AutoChunks[key].Set(cellX, cellY, true);
}

const IntGridValueDef* TileMapLayerData::FindIntGridValue(uint8 value) const
{
    for (const IntGridValueDef& def : IntGridValues)
    {
        if (def.Value == value)
            return &def;
    }
    return nullptr;
}

void TileMapLayerData::CompactEmptyChunks()
{
    std::erase_if(m_Chunks, [](const auto& entry) { return entry.second.IsAllEmpty(); });
    std::erase_if(m_IntGridChunks, [](const auto& entry) { return entry.second.IsAllZero(); });
    std::erase_if(m_AutoChunks, [](const auto& entry) { return entry.second.IsAllZero(); });
}

std::vector<ChunkCoord> TileMapLayerData::SortedChunkCoords() const
{
    return SortedCoords(m_Chunks);
}

std::vector<ChunkCoord> TileMapLayerData::SortedIntGridChunkCoords() const
{
    return SortedCoords(m_IntGridChunks);
}

std::vector<ChunkCoord> TileMapLayerData::SortedAutoChunkCoords() const
{
    return SortedCoords(m_AutoChunks);
}

const TileMapChunk* TileMapLayerData::FindChunk(ChunkCoord coord) const
{
    const auto it = m_Chunks.find(PackChunkCoord(coord));
    return it != m_Chunks.end() ? &it->second : nullptr;
}

const IntGridChunk* TileMapLayerData::FindIntGridChunk(ChunkCoord coord) const
{
    const auto it = m_IntGridChunks.find(PackChunkCoord(coord));
    return it != m_IntGridChunks.end() ? &it->second : nullptr;
}

TileMapChunk& TileMapLayerData::GetOrCreateChunk(ChunkCoord coord)
{
    return m_Chunks[PackChunkCoord(coord)];
}

IntGridChunk& TileMapLayerData::GetOrCreateIntGridChunk(ChunkCoord coord)
{
    return m_IntGridChunks[PackChunkCoord(coord)];
}

const AutoTileChunk* TileMapLayerData::FindAutoChunk(ChunkCoord coord) const
{
    const auto it = m_AutoChunks.find(PackChunkCoord(coord));
    return it != m_AutoChunks.end() ? &it->second : nullptr;
}

AutoTileChunk& TileMapLayerData::GetOrCreateAutoChunk(ChunkCoord coord)
{
    return m_AutoChunks[PackChunkCoord(coord)];
}

void TileMapLayerData::Clear()
{
    m_Chunks.clear();
    m_IntGridChunks.clear();
    m_AutoChunks.clear();
}

} // namespace TileMap
} // namespace GameEngine
