#pragma once

#include "AssetCore/Asset.h"
#include "AssetCore/GUID.h"
#include "AssetCore/Types.h"
#include "TileMap/TileMapData.h"

#include <filesystem>

namespace GameEngine
{
namespace TileMap
{

/**
 * @brief External tilemap asset (.tilemap): the serialized form of a
 * TileMapData. Layers reference tilesets by GUID; per-layer cell data is
 * stored as sparse chunks sorted by (Y, X), each chunk one CSV string per
 * row — text-diffable and stable under re-serialization.
 */
class TileMapAsset : public ::GameEngine::Asset
{
public:
    // 2 adds the IntGrid value table, the auto-tiling rule sets and the
    // provenance plane. Every block is optional and omitted when empty, so a
    // version-1 file loads unchanged and a map that uses none of them differs
    // from its version-1 form only in this number.
    static constexpr int32 kSchemaVersion = 2;

    TileMapAsset(const ::GameEngine::GUID& guid, const std::filesystem::path& path)
        : ::GameEngine::Asset(guid, ::GameEngine::AssetType::TileMap, path) {}

    bool Load() override;
    bool LoadFromData(const Vector<uint8>& data) override;
    void Unload() override;

    bool SaveToData(Vector<uint8>& outData) const;

    TileMapData& Data() { return m_Data; }
    const TileMapData& Data() const { return m_Data; }

private:
    bool ParseJson(const std::string& text);
    std::string SerializeJson() const;

    TileMapData m_Data;
};

} // namespace TileMap
} // namespace GameEngine
