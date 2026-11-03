#include "TileMap/TileMapData.h"

#include <algorithm>

namespace GameEngine
{
namespace TileMap
{

uint32 TileMapData::AddLayer(const std::string& name, TileMapLayerType type)
{
    if (m_NextLayerId == UINT32_MAX)
        return 0;
    TileMapLayerData& layer = m_Layers.emplace_back();
    layer.Id = m_NextLayerId++;
    layer.Name = name;
    layer.Type = type;
    return layer.Id;
}

bool TileMapData::RemoveLayer(uint32 layerId)
{
    const auto count = std::erase_if(m_Layers, [layerId](const TileMapLayerData& layer)
    {
        return layer.Id == layerId;
    });
    return count > 0;
}

uint32 TileMapData::DuplicateLayer(uint32 layerId, const std::string& name)
{
    const auto source = std::find_if(m_Layers.begin(), m_Layers.end(),
                                     [layerId](const TileMapLayerData& layer)
    {
        return layer.Id == layerId;
    });
    if (source == m_Layers.end())
        return 0;

    if (m_NextLayerId == UINT32_MAX)
        return 0;

    // Value copy: the chunk maps come with it, so the duplicate is a full copy
    // of the painted content rather than an empty layer of the same shape.
    TileMapLayerData copy = *source;
    copy.Id = m_NextLayerId++;
    copy.Name = name;

    const uint32 newId = copy.Id;
    m_Layers.insert(source + 1, std::move(copy));
    return newId;
}

bool TileMapData::InsertLayer(size_t index, TileMapLayerData layer)
{
    if (layer.Id == 0 || index > m_Layers.size() || FindLayer(layer.Id) != nullptr)
        return false;

    // Saturating, matching RestoreNextLayerId: a layer restored at the maximum
    // id must not wrap the counter back onto ids that already exist.
    const uint32 after = layer.Id == UINT32_MAX ? UINT32_MAX : layer.Id + 1;
    m_NextLayerId = std::max(m_NextLayerId, after);
    m_Layers.insert(m_Layers.begin() + static_cast<std::ptrdiff_t>(index), std::move(layer));
    return true;
}

TileMapLayerData* TileMapData::FindLayer(uint32 layerId)
{
    for (auto& layer : m_Layers)
    {
        if (layer.Id == layerId)
            return &layer;
    }
    return nullptr;
}

const TileMapLayerData* TileMapData::FindLayer(uint32 layerId) const
{
    return const_cast<TileMapData*>(this)->FindLayer(layerId);
}

void TileMapData::RestoreNextLayerId(uint32 nextLayerId)
{
    // Saturating, not wrapping: a layer already at the maximum id would
    // otherwise reset the counter to 1 and hand out ids that already exist.
    uint32 minimum = 1;
    for (const auto& layer : m_Layers)
    {
        const uint32 after = layer.Id == UINT32_MAX ? UINT32_MAX : layer.Id + 1;
        minimum = std::max(minimum, after);
    }
    m_NextLayerId = std::max(nextLayerId, minimum);
}

void TileMapData::Clear()
{
    m_Layers.clear();
    m_NextLayerId = 1;
}

} // namespace TileMap
} // namespace GameEngine
