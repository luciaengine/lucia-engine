#include "TileMap/TileSetAsset.h"

#include "AssetCore/SharedFileRead.h"
#include "JsonRead.h"
#include "JsonWrite.h"
#include "Logger/Logger.h"
#include "TileMap/TileCellEncoding.h"

#include <algorithm>
#include <nlohmann/json.hpp>
#include <unordered_map>
#include <utility>

namespace GameEngine
{
namespace TileMap
{

namespace
{
using json = nlohmann::json;

json SerializeCustomValue(const TileCustomValue& value)
{
    json out;
    std::visit([&out](const auto& v) { out = v; }, value);
    return out;
}

bool ParseCustomValue(const json& in, TileCustomValue& out)
{
    if (in.is_boolean())
        out = in.get<bool>();
    else if (in.is_number_integer())
    {
        if (in.is_number_unsigned() && in.get<uint64>() > uint64(INT64_MAX))
            return false;
        out = in.get<int64>();
    }
    else if (in.is_number_float())
        out = in.get<float64>();
    else if (in.is_string())
        out = in.get<std::string>();
    else
        return false;
    return true;
}

json SerializePeering(const TerrainPeering& peering)
{
    json out;
    out["terrainSet"] = peering.TerrainSet;
    out["center"] = peering.Center;
    out["neighbors"] = peering.Neighbors;
    return out;
}

int16 ClampToTerrainIndex(int64 value)
{
    return static_cast<int16>(std::clamp<int64>(value, INT16_MIN, INT16_MAX));
}

TerrainPeering ParsePeering(const json& in)
{
    TerrainPeering peering;
    peering.TerrainSet = ClampToTerrainIndex(JsonRead::Integer(in, "terrainSet", -1));
    peering.Center = ClampToTerrainIndex(JsonRead::Integer(in, "center", -1));
    const json* neighbors = JsonRead::Array(in, "neighbors");
    if (neighbors && neighbors->size() == kPeeringSlotCount)
    {
        for (uint32 i = 0; i < kPeeringSlotCount; ++i)
        {
            peering.Neighbors[i] =
                ClampToTerrainIndex(JsonRead::IntegerAt(*neighbors, i, -1));
        }
    }
    return peering;
}

// Collision modes serialize as names rather than ordinals so a .tileset diff
// reads as authoring intent.
constexpr const char* kOneWayModeName = "oneway";

const char* CollisionModeName(TileCollisionMode mode)
{
    return mode == TileCollisionMode::OneWay ? kOneWayModeName : "solid";
}

TileCollisionMode ParseCollisionMode(const std::string& name)
{
    return name == kOneWayModeName ? TileCollisionMode::OneWay : TileCollisionMode::Solid;
}

} // namespace

bool TileSetAsset::Load()
{
    SetState(AssetState::Loading);
    String text;
    if (!ReadFileTextShared(GetPath(), text))
    {
        Logger::Log::Error("TileSetAsset: cannot open '{}'", GetPath().string());
        SetState(AssetState::Failed);
        return false;
    }
    return ParseJson(text);
}

bool TileSetAsset::LoadFromData(const Vector<uint8>& data)
{
    SetState(AssetState::Loading);
    const std::string text(reinterpret_cast<const char*>(data.data()), data.size());
    return ParseJson(text);
}

void TileSetAsset::Unload()
{
    m_AtlasTexture = GUID();
    m_AtlasWidthPx = 0;
    m_AtlasHeightPx = 0;
    m_TileWidthPx = 0;
    m_TileHeightPx = 0;
    m_MarginPx = 0;
    m_SpacingPx = 0;
    m_Columns = 0;
    m_Rows = 0;
    m_Tiles.clear();
    m_TerrainSets.clear();
    m_Brushes.clear();
    SetState(AssetState::Unloaded);
}

bool TileSetAsset::SaveToData(Vector<uint8>& outData) const
{
    const std::string serialized = SerializeJson();
    outData.assign(serialized.begin(), serialized.end());
    return true;
}

const TileSetTile* TileSetAsset::FindTile(uint32 tileId) const
{
    return const_cast<TileSetAsset*>(this)->FindTile(tileId);
}

TileSetTile* TileSetAsset::FindTile(uint32 tileId)
{
    // Tiles are kept sorted by id (SliceGrid assigns ascending; ParseJson sorts).
    const auto it = std::lower_bound(m_Tiles.begin(), m_Tiles.end(), tileId,
        [](const TileSetTile& tile, uint32 id) { return tile.Id < id; });
    if (it != m_Tiles.end() && it->Id == tileId)
        return &(*it);
    return nullptr;
}

void TileSetAsset::SetAtlasTexture(const GUID& texture, int32 atlasWidthPx, int32 atlasHeightPx)
{
    m_AtlasTexture = texture;
    m_AtlasWidthPx = atlasWidthPx;
    m_AtlasHeightPx = atlasHeightPx;
}

int32 TileSetAsset::GridColumns(int32 atlasWidthPx, int32 tileWidthPx, int32 marginPx, int32 spacingPx)
{
    if (atlasWidthPx <= 0 || tileWidthPx <= 0 || marginPx < 0 || spacingPx < 0)
        return 0;
    const int64 usable = static_cast<int64>(atlasWidthPx) - 2 * static_cast<int64>(marginPx) + spacingPx;
    const int64 stride = static_cast<int64>(tileWidthPx) + spacingPx;
    return static_cast<int32>(std::max<int64>(0, usable / stride));
}

int32 TileSetAsset::GridRows(int32 atlasHeightPx, int32 tileHeightPx, int32 marginPx, int32 spacingPx)
{
    return GridColumns(atlasHeightPx, tileHeightPx, marginPx, spacingPx);
}

TileUVRect TileSetAsset::ComputeTileUV(int32 atlasWidthPx, int32 atlasHeightPx,
                                       int32 tileWidthPx, int32 tileHeightPx,
                                       int32 marginPx, int32 spacingPx,
                                       int32 column, int32 row)
{
    TileUVRect uv;
    if (atlasWidthPx <= 0 || atlasHeightPx <= 0)
        return uv;
    const float32 x0 = static_cast<float32>(static_cast<int64>(marginPx) + static_cast<int64>(column) * (static_cast<int64>(tileWidthPx) + spacingPx));
    const float32 y0 = static_cast<float32>(static_cast<int64>(marginPx) + static_cast<int64>(row) * (static_cast<int64>(tileHeightPx) + spacingPx));
    uv.U0 = x0 / static_cast<float32>(atlasWidthPx);
    uv.V0 = y0 / static_cast<float32>(atlasHeightPx);
    uv.U1 = (x0 + static_cast<float32>(tileWidthPx)) / static_cast<float32>(atlasWidthPx);
    uv.V1 = (y0 + static_cast<float32>(tileHeightPx)) / static_cast<float32>(atlasHeightPx);
    return uv;
}

bool TileSetAsset::SliceGrid(int32 tileWidthPx, int32 tileHeightPx, int32 marginPx, int32 spacingPx)
{
    // Validated before anything is written: a refused slice must leave the asset
    // exactly as it was. Clearing the tile table here would destroy every
    // authored polygon, mode and material in memory, and the next slice the
    // author gets right would then write that emptied asset over their file.
    const int32 columns = GridColumns(m_AtlasWidthPx, tileWidthPx, marginPx, spacingPx);
    const int32 rows = GridRows(m_AtlasHeightPx, tileHeightPx, marginPx, spacingPx);
    if (static_cast<int64>(columns) * rows > kMaxTileSetTiles)
        return false;

    m_TileWidthPx = tileWidthPx;
    m_TileHeightPx = tileHeightPx;
    m_MarginPx = marginPx;
    m_SpacingPx = spacingPx;
    m_Columns = columns;
    m_Rows = rows;

    std::unordered_map<uint32, TileSetTile> authored;
    authored.reserve(m_Tiles.size());
    for (auto& tile : m_Tiles)
        authored.emplace(tile.Id, std::move(tile));

    m_Tiles.clear();
    m_Tiles.reserve(static_cast<size_t>(m_Columns) * static_cast<size_t>(m_Rows));
    for (int32 row = 0; row < m_Rows; ++row)
    {
        for (int32 column = 0; column < m_Columns; ++column)
        {
            const uint32 id = static_cast<uint32>(row * m_Columns + column) + 1;
            const auto it = authored.find(id);
            TileSetTile tile = it != authored.end() ? std::move(it->second) : TileSetTile{};
            tile.Id = id;
            tile.UV = ComputeTileUV(m_AtlasWidthPx, m_AtlasHeightPx,
                                    tileWidthPx, tileHeightPx, marginPx, spacingPx,
                                    column, row);
            m_Tiles.push_back(std::move(tile));
        }
    }
    return true;
}

bool TileSetAsset::ParseJson(const std::string& text)
{
    const auto reject = [this](const std::string& reason)
    {
        Logger::Log::Error("TileSetAsset: rejected '{}': {}", GetPath().string(), reason);
        m_AtlasTexture = GUID();
        m_AtlasWidthPx = 0;
        m_AtlasHeightPx = 0;
        m_TileWidthPx = 0;
        m_TileHeightPx = 0;
        m_MarginPx = 0;
        m_SpacingPx = 0;
        m_Columns = 0;
        m_Rows = 0;
        m_Tiles.clear();
        m_TerrainSets.clear();
    m_Brushes.clear();
        SetState(AssetState::Failed);
        return false;
    };

    // allow_exceptions = false: a malformed document reads back as discarded.
    const json doc = json::parse(text, nullptr, /*allow_exceptions*/ false);
    if (doc.is_discarded() || !doc.is_object())
        return reject("not a JSON object");

    m_Tiles.clear();
    m_TerrainSets.clear();
    m_Brushes.clear();

    if (const json* atlas = JsonRead::Member(doc, "atlasTexture"); atlas && atlas->is_string())
        m_AtlasTexture = GUID(atlas->get<std::string>());

    // Grid geometry is foreign input that sizes allocations here and in every
    // consumer that re-slices from it, so each field and their product are
    // bounded before anything is sized by them.
    const auto readBounded = [&doc](const char* key, int32 ceiling, int32& out)
    {
        const int64 value = JsonRead::Integer(doc, key, 0);
        if (value < 0 || value > ceiling)
            return false;
        out = static_cast<int32>(value);
        return true;
    };
    if (!readBounded("atlasWidthPx", kMaxAtlasAxisPx, m_AtlasWidthPx) ||
        !readBounded("atlasHeightPx", kMaxAtlasAxisPx, m_AtlasHeightPx) ||
        !readBounded("tileWidthPx", kMaxAtlasAxisPx, m_TileWidthPx) ||
        !readBounded("tileHeightPx", kMaxAtlasAxisPx, m_TileHeightPx) ||
        !readBounded("marginPx", kMaxAtlasAxisPx, m_MarginPx) ||
        !readBounded("spacingPx", kMaxAtlasAxisPx, m_SpacingPx) ||
        !readBounded("columns", kMaxTileSetTiles, m_Columns) ||
        !readBounded("rows", kMaxTileSetTiles, m_Rows))
    {
        return reject("atlas or grid geometry outside the supported range");
    }
    if (static_cast<int64>(m_Columns) * m_Rows > kMaxTileSetTiles)
        return reject("grid slices to more than the maximum tile count");

    if (const json* terrainSets = JsonRead::Array(doc, "terrainSets"))
    {
        for (const auto& setJson : *terrainSets)
        {
            TerrainSetDef set;
            set.Name = JsonRead::Text(setJson, "name");
            set.MatchMode = static_cast<TerrainMatchMode>(
                static_cast<uint8>(std::clamp<int64>(JsonRead::Integer(setJson, "matchMode", 0),
                                                     0, UINT8_MAX)));
            if (const json* terrains = JsonRead::Array(setJson, "terrains"))
            {
                for (const auto& terrainJson : *terrains)
                {
                    TerrainDef terrain;
                    terrain.Name = JsonRead::Text(terrainJson, "name");
                    terrain.ColorRGBA = static_cast<uint32>(std::clamp<int64>(
                        JsonRead::Integer(terrainJson, "colorRGBA", 0xFFFFFFFF), 0, UINT32_MAX));
                    set.Terrains.push_back(std::move(terrain));
                }
            }
            m_TerrainSets.push_back(std::move(set));
        }
    }

    if (const json* brushes = JsonRead::Array(doc, "brushes"))
    {
        for (const auto& brushJson : *brushes)
        {
            TileBrushDef brush;
            brush.Name = JsonRead::Text(brushJson, "name");
            const int64 width = JsonRead::Integer(brushJson, "width", 0);
            const int64 height = JsonRead::Integer(brushJson, "height", 0);
            // The footprint sizes an allocation, so a hostile file cannot name
            // one larger than a brush could ever be.
            if (width <= 0 || height <= 0 || width > kMaxBrushAxisCells ||
                height > kMaxBrushAxisCells)
                return reject("brush footprint outside the allowed range");
            brush.Width = static_cast<int32>(width);
            brush.Height = static_cast<int32>(height);
            const json* cells = JsonRead::Array(brushJson, "cells");
            if (cells == nullptr || cells->size() != static_cast<size_t>(width * height))
                return reject("brush cell count does not match its footprint");
            brush.Cells.reserve(cells->size());
            for (const auto& cellJson : *cells)
            {
                if (!cellJson.is_number_integer())
                    return reject("brush cell is not an integer");
                const int64 cell = cellJson.get<int64>();
                if (cell < 0 || cell > static_cast<int64>(UINT32_MAX))
                    return reject("brush cell outside the encodable range");
                brush.Cells.push_back(static_cast<uint32>(cell));
            }
            m_Brushes.push_back(std::move(brush));
        }
    }

    const json* tiles = JsonRead::Array(doc, "tiles");
    if (!tiles)
        return reject("no tiles array");
    if (tiles->size() > static_cast<size_t>(kMaxTileSetTiles))
        return reject("more tiles than one set may hold");
    {
        for (const auto& tileJson : *tiles)
        {
            // The renderer's descriptor table is sized by the highest id, so an
            // id is bounded by the set's tile ceiling, not merely by what a cell
            // encodes; 0 is the empty cell and names no tile.
            const int64 tileId = JsonRead::Integer(tileJson, "id", 0);
            if (tileId <= 0 || tileId > static_cast<int64>(kMaxTileSetTiles))
                return reject("tile id " + std::to_string(tileId) + " outside 1.." +
                              std::to_string(kMaxTileSetTiles));

            for (const char* field : {"u0", "v0", "u1", "v1", "probability"})
                if (!JsonRead::FitsFloat32(JsonRead::Number(tileJson, field, 0.0)))
                    return reject("tile numeric value exceeds the finite float range");
            TileSetTile tile;
            tile.Id = static_cast<uint32>(tileId);
            tile.UV.U0 = static_cast<float32>(JsonRead::Number(tileJson, "u0", 0.0));
            tile.UV.V0 = static_cast<float32>(JsonRead::Number(tileJson, "v0", 0.0));
            tile.UV.U1 = static_cast<float32>(JsonRead::Number(tileJson, "u1", 1.0));
            tile.UV.V1 = static_cast<float32>(JsonRead::Number(tileJson, "v1", 1.0));
            if (const json* textureOverride = JsonRead::Member(tileJson, "textureOverride");
                textureOverride && textureOverride->is_string())
            {
                tile.TextureOverride = GUID(textureOverride->get<std::string>());
            }
            tile.Probability =
                std::max(0.0f, static_cast<float32>(JsonRead::Number(tileJson, "probability", 1.0)));
            if (const json* collision = JsonRead::Array(tileJson, "collision"))
            {
                for (const auto& polygonJson : *collision)
                {
                    if (!polygonJson.is_array())
                        continue;
                    TileCollisionPolygon polygon;
                    for (const auto& pointJson : polygonJson)
                    {
                        if (pointJson.is_array() && pointJson.size() == 2)
                        {
                            if (!JsonRead::FitsFloat32(JsonRead::NumberAt(pointJson, 0, 0.0)) ||
                                !JsonRead::FitsFloat32(JsonRead::NumberAt(pointJson, 1, 0.0)))
                                return reject("collision point exceeds the finite float range");
                            polygon.Points.emplace_back(
                                static_cast<float32>(JsonRead::NumberAt(pointJson, 0, 0.0)),
                                static_cast<float32>(JsonRead::NumberAt(pointJson, 1, 0.0)));
                        }
                    }
                    if (polygon.Points.size() >= 3)
                        tile.CollisionPolygons.push_back(std::move(polygon));
                }
            }
            if (const json* mode = JsonRead::Member(tileJson, "collisionMode");
                mode && mode->is_string())
            {
                tile.CollisionMode = ParseCollisionMode(mode->get<std::string>());
            }
            if (const json* material = JsonRead::Member(tileJson, "physicsMaterial");
                material && material->is_string())
            {
                tile.PhysicsMaterial = GUID(material->get<std::string>());
            }
            if (const json* animation = JsonRead::Array(tileJson, "animation"))
            {
                for (const auto& frameJson : *animation)
                {
                    const int64 frameTileId = JsonRead::Integer(frameJson, "tileId", 0);
                    if (frameTileId <= 0 || frameTileId > static_cast<int64>(kMaxTileId))
                        continue;
                    if (!JsonRead::FitsFloat32(JsonRead::Number(frameJson, "durationSeconds", 0.1)))
                        return reject("animation duration exceeds the finite float range");
                    tile.AnimationFrames.push_back(TileAnimationFrame{
                        static_cast<uint32>(frameTileId),
                        std::max(0.0f, static_cast<float32>(
                                           JsonRead::Number(frameJson, "durationSeconds", 0.1)))});
                }
            }
            if (const json* peering = JsonRead::Object(tileJson, "peering"))
                tile.Peering = ParsePeering(*peering);
            if (const json* customData = JsonRead::Object(tileJson, "customData"))
            {
                for (const auto& [key, valueJson] : customData->items())
                {
                    TileCustomValue value;
                    if (valueJson.is_number_unsigned() && valueJson.get<uint64>() > uint64(INT64_MAX))
                        return reject("custom integer exceeds the signed 64-bit range");
                    if (ParseCustomValue(valueJson, value))
                        tile.CustomData.emplace(key, std::move(value));
                }
            }
            m_Tiles.push_back(std::move(tile));
        }
    }

    // FindTile binary-searches, so ascending id order is an invariant of the
    // tile table, not a convenience.
    std::sort(m_Tiles.begin(), m_Tiles.end(),
              [](const TileSetTile& a, const TileSetTile& b) { return a.Id < b.Id; });
    const auto duplicate = std::adjacent_find(m_Tiles.begin(), m_Tiles.end(),
        [](const TileSetTile& a, const TileSetTile& b) { return a.Id == b.Id; });
    if (duplicate != m_Tiles.end())
        return reject("two tiles share id " + std::to_string(duplicate->Id));

    SetState(AssetState::Loaded);
    return true;
}

std::string TileSetAsset::SerializeJson() const
{
    json doc;
    doc["schemaVersion"] = kSchemaVersion;
    doc["assetType"] = "TileSet";
    doc["atlasTexture"] = m_AtlasTexture.ToString();
    doc["atlasWidthPx"] = m_AtlasWidthPx;
    doc["atlasHeightPx"] = m_AtlasHeightPx;
    doc["tileWidthPx"] = m_TileWidthPx;
    doc["tileHeightPx"] = m_TileHeightPx;
    doc["marginPx"] = m_MarginPx;
    doc["spacingPx"] = m_SpacingPx;
    doc["columns"] = m_Columns;
    doc["rows"] = m_Rows;

    json terrainSets = json::array();
    for (const auto& set : m_TerrainSets)
    {
        json setJson;
        setJson["name"] = set.Name;
        setJson["matchMode"] = static_cast<uint8>(set.MatchMode);
        json terrains = json::array();
        for (const auto& terrain : set.Terrains)
        {
            terrains.push_back(json{{"name", terrain.Name}, {"colorRGBA", terrain.ColorRGBA}});
        }
        setJson["terrains"] = std::move(terrains);
        terrainSets.push_back(std::move(setJson));
    }
    doc["terrainSets"] = std::move(terrainSets);

    if (!m_Brushes.empty())
    {
        json brushes = json::array();
        for (const TileBrushDef& brush : m_Brushes)
        {
            brushes.push_back(json{{"name", brush.Name},
                                   {"width", brush.Width},
                                   {"height", brush.Height},
                                   {"cells", brush.Cells}});
        }
        doc["brushes"] = std::move(brushes);
    }

    json tiles = json::array();
    for (const auto& tile : m_Tiles)
    {
        json tileJson;
        tileJson["id"] = tile.Id;
        tileJson["u0"] = JsonWrite::ShortestFloat(tile.UV.U0);
        tileJson["v0"] = JsonWrite::ShortestFloat(tile.UV.V0);
        tileJson["u1"] = JsonWrite::ShortestFloat(tile.UV.U1);
        tileJson["v1"] = JsonWrite::ShortestFloat(tile.UV.V1);
        if (!tile.TextureOverride.IsNull())
            tileJson["textureOverride"] = tile.TextureOverride.ToString();
        if (tile.Probability != 1.0f)
            tileJson["probability"] = JsonWrite::ShortestFloat(tile.Probability);
        if (!tile.CollisionPolygons.empty())
        {
            json collision = json::array();
            for (const auto& polygon : tile.CollisionPolygons)
            {
                json points = json::array();
                for (const auto& point : polygon.Points)
                    points.push_back(json::array({JsonWrite::ShortestFloat(point.x), JsonWrite::ShortestFloat(point.y)}));
                collision.push_back(std::move(points));
            }
            tileJson["collision"] = std::move(collision);
        }
        if (tile.CollisionMode != TileCollisionMode::Solid)
            tileJson["collisionMode"] = CollisionModeName(tile.CollisionMode);
        if (!tile.PhysicsMaterial.IsNull())
            tileJson["physicsMaterial"] = tile.PhysicsMaterial.ToString();
        if (!tile.AnimationFrames.empty())
        {
            json animation = json::array();
            for (const auto& frame : tile.AnimationFrames)
            {
                animation.push_back(json{{"tileId", frame.TileId},
                                         {"durationSeconds", JsonWrite::ShortestFloat(frame.DurationSeconds)}});
            }
            tileJson["animation"] = std::move(animation);
        }
        if (tile.Peering.ParticipatesInTerrain())
            tileJson["peering"] = SerializePeering(tile.Peering);
        if (!tile.CustomData.empty())
        {
            json customData;
            for (const auto& [key, value] : tile.CustomData)
                customData[key] = SerializeCustomValue(value);
            tileJson["customData"] = std::move(customData);
        }
        tiles.push_back(std::move(tileJson));
    }
    doc["tiles"] = std::move(tiles);

    return doc.dump(2);
}

} // namespace TileMap
} // namespace GameEngine
