#include "TileMap/TileMapAsset.h"

#include "AssetCore/SharedFileRead.h"
#include "JsonRead.h"
#include "JsonWrite.h"
#include "Logger/Logger.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <nlohmann/json.hpp>
#include <unordered_set>
#include <utility>

namespace GameEngine
{
namespace TileMap
{

namespace
{
using json = nlohmann::json;

const char* LayerTypeName(TileMapLayerType type)
{
    switch (type)
    {
        case TileMapLayerType::Tiles:   return "tiles";
        case TileMapLayerType::IntGrid: return "intgrid";
    }
    return "tiles";
}

TileMapLayerType LayerTypeFromName(const std::string& name)
{
    if (name == "intgrid")
        return TileMapLayerType::IntGrid;
    return TileMapLayerType::Tiles;
}

template <typename Value>
std::string RowToCsv(const Value* row)
{
    std::string out;
    out.reserve(kChunkSize * 4);
    for (int32 x = 0; x < kChunkSize; ++x)
    {
        if (x != 0)
            out.push_back(',');
        out += std::to_string(static_cast<uint64>(row[x]));
    }
    return out;
}

template <typename Value>
bool CsvToRow(const std::string& csv, Value* row)
{
    const char* cursor = csv.data();
    const char* end = csv.data() + csv.size();
    for (int32 x = 0; x < kChunkSize; ++x)
    {
        uint64 value = 0;
        const auto result = std::from_chars(cursor, end, value);
        // A value wider than the plane's type is corruption, not something to
        // truncate: an IntGrid 999 would silently become 231.
        if (result.ec != std::errc() || value > uint64(std::numeric_limits<Value>::max()))
            return false;
        row[x] = static_cast<Value>(value);
        cursor = result.ptr;
        if (x + 1 < kChunkSize)
        {
            if (cursor >= end || *cursor != ',')
                return false;
            ++cursor;
        }
    }
    return cursor == end;
}

template <typename Chunk, typename Value>
json ChunksToJson(const std::vector<ChunkCoord>& coords,
                  const Chunk* (TileMapLayerData::*find)(ChunkCoord) const,
                  const TileMapLayerData& layer,
                  const std::array<Value, kChunkCellCount> Chunk::*plane)
{
    json chunks = json::array();
    for (const ChunkCoord coord : coords)
    {
        const Chunk* chunk = (layer.*find)(coord);
        if (!chunk)
            continue;
        json chunkJson;
        chunkJson["x"] = coord.X;
        chunkJson["y"] = coord.Y;
        json rows = json::array();
        for (int32 y = 0; y < kChunkSize; ++y)
            rows.push_back(RowToCsv((chunk->*plane).data() + y * kChunkSize));
        chunkJson["rows"] = std::move(rows);
        chunks.push_back(std::move(chunkJson));
    }
    return chunks;
}

/** An unsigned 32-bit id or count field, clamped into range. */
uint32 ReadUInt32(const json& object, const char* key, uint32 fallback)
{
    return static_cast<uint32>(
        std::clamp<int64>(JsonRead::Integer(object, key, fallback), 0, UINT32_MAX));
}

/** A signed 32-bit field, clamped into range. */
int32 ReadInt32(const json& object, const char* key, int32 fallback)
{
    return static_cast<int32>(
        std::clamp<int64>(JsonRead::Integer(object, key, fallback), INT32_MIN, INT32_MAX));
}

/**
 * A chunk coordinate: present, an integer and inside int32. A clamped
 * coordinate would move the chunk's cells somewhere the author never put them.
 */
bool ReadChunkAxis(const json& chunkJson, const char* key, int32& out)
{
    const int64 value = JsonRead::Integer(chunkJson, key, INT64_MIN);
    if (value < INT32_MIN / kChunkSize || value > INT32_MAX / kChunkSize)
        return false;
    out = static_cast<int32>(value);
    return true;
}

template <typename Chunk, typename Value>
bool JsonToChunks(const json& chunksJson,
                  Chunk& (TileMapLayerData::*getOrCreate)(ChunkCoord),
                  TileMapLayerData& layer,
                  std::array<Value, kChunkCellCount> Chunk::*plane)
{
    if (!chunksJson.is_array())
        return false;
    for (const auto& chunkJson : chunksJson)
    {
        ChunkCoord coord{};
        if (!ReadChunkAxis(chunkJson, "x", coord.X) || !ReadChunkAxis(chunkJson, "y", coord.Y))
            return false;
        const json* rowsJson = JsonRead::Array(chunkJson, "rows");
        if (!rowsJson || rowsJson->size() != static_cast<size_t>(kChunkSize))
            return false;
        Chunk& chunk = (layer.*getOrCreate)(coord);
        for (int32 y = 0; y < kChunkSize; ++y)
        {
            const auto& rowJson = (*rowsJson)[y];
            if (!rowJson.is_string() ||
                !CsvToRow(rowJson.get_ref<const std::string&>(),
                          (chunk.*plane).data() + y * kChunkSize))
                return false;
        }
    }
    return true;
}

// --- Rule and provenance serialization -------------------------------------
//
// Pattern cells are written as tokens rather than numbers so a rule reads in a
// diff: "." don't care, "*" any non-zero, "=N" require, "!N" forbid.

const char* kDontCareToken = ".";
const char* kAnyToken = "*";

std::string PatternRowToString(const std::vector<RulePatternCell>& pattern, int32 size, int32 row)
{
    std::string out;
    for (int32 x = 0; x < size; ++x)
    {
        if (x != 0)
            out.push_back(',');
        const RulePatternCell& cell = pattern[static_cast<size_t>(row) * static_cast<size_t>(size) +
                                              static_cast<size_t>(x)];
        switch (cell.Op)
        {
        case RulePatternOp::DontCare:     out += kDontCareToken; break;
        case RulePatternOp::RequireAny:   out += kAnyToken; break;
        case RulePatternOp::RequireValue: out += "=" + std::to_string(cell.Value); break;
        case RulePatternOp::ForbidValue:  out += "!" + std::to_string(cell.Value); break;
        }
    }
    return out;
}

bool PatternTokenToCell(const std::string& token, RulePatternCell& outCell)
{
    if (token == kDontCareToken)
    {
        outCell = RulePatternCell{};
        return true;
    }
    if (token == kAnyToken)
    {
        outCell = RulePatternCell{RulePatternOp::RequireAny, 0};
        return true;
    }
    if (token.size() < 2)
        return false;
    uint32 value = 0;
    const char* begin = token.data() + 1;
    const char* end = token.data() + token.size();
    if (std::from_chars(begin, end, value).ec != std::errc() || value > 0xFFu)
        return false;
    if (token[0] == '=')
        outCell = RulePatternCell{RulePatternOp::RequireValue, static_cast<uint8>(value)};
    else if (token[0] == '!')
        outCell = RulePatternCell{RulePatternOp::ForbidValue, static_cast<uint8>(value)};
    else
        return false;
    return true;
}

std::vector<std::string> SplitCsv(const std::string& text)
{
    std::vector<std::string> parts;
    size_t start = 0;
    while (true)
    {
        const size_t comma = text.find(',', start);
        parts.push_back(text.substr(start, comma == std::string::npos ? comma : comma - start));
        if (comma == std::string::npos)
            return parts;
        start = comma + 1;
    }
}

json RuleToJson(const TileRule& rule)
{
    json ruleJson;
    ruleJson["id"] = rule.Id;
    ruleJson["name"] = rule.Name;
    ruleJson["enabled"] = rule.Enabled;
    ruleJson["patternSize"] = rule.PatternSize;

    json pattern = json::array();
    for (int32 y = 0; y < rule.PatternSize; ++y)
        pattern.push_back(PatternRowToString(rule.Pattern, rule.PatternSize, y));
    ruleJson["pattern"] = std::move(pattern);

    if (rule.OutputMode == RuleOutputMode::Stamp)
    {
        json stamp = json::array();
        for (int32 y = 0; y < rule.PatternSize; ++y)
        {
            std::string row;
            for (int32 x = 0; x < rule.PatternSize; ++x)
            {
                if (x != 0)
                    row.push_back(',');
                row += std::to_string(rule.Stamp[static_cast<size_t>(y) *
                                                     static_cast<size_t>(rule.PatternSize) +
                                                 static_cast<size_t>(x)]);
            }
            stamp.push_back(std::move(row));
        }
        ruleJson["stamp"] = std::move(stamp);
    }
    else
    {
        json tiles = json::array();
        for (const RuleTileOutput& output : rule.Tiles)
            tiles.push_back(json{{"cell", output.Cell}, {"weight", output.Weight}});
        ruleJson["tiles"] = std::move(tiles);
    }

    ruleJson["chance"] = rule.Chance;
    ruleJson["breakOnMatch"] = rule.BreakOnMatch;
    ruleJson["mirrorX"] = rule.MirrorX;
    ruleJson["mirrorY"] = rule.MirrorY;
    ruleJson["moduloX"] = rule.ModuloX;
    ruleJson["moduloY"] = rule.ModuloY;
    ruleJson["offsetX"] = rule.OffsetX;
    ruleJson["offsetY"] = rule.OffsetY;
    return ruleJson;
}

bool RuleFromJson(const json& ruleJson, TileRule& outRule)
{
    outRule.Id = ReadUInt32(ruleJson, "id", 0);
    outRule.Name = JsonRead::Text(ruleJson, "name");
    outRule.Enabled = JsonRead::Boolean(ruleJson, "enabled", true);
    // Narrow before validating: a value outside int32 would truncate into a
    // size that passes.
    outRule.PatternSize = static_cast<int32>(std::clamp<int64>(
        JsonRead::Integer(ruleJson, "patternSize", 3), INT32_MIN, INT32_MAX));
    if (!IsValidRulePatternSize(outRule.PatternSize))
        return false;

    const int32 size = outRule.PatternSize;
    outRule.Pattern.assign(static_cast<size_t>(size) * static_cast<size_t>(size),
                           RulePatternCell{});
    const json* patternJson = JsonRead::Array(ruleJson, "pattern");
    if (!patternJson || patternJson->size() != static_cast<size_t>(size))
        return false;
    for (int32 y = 0; y < size; ++y)
    {
        const auto& rowJson = (*patternJson)[y];
        if (!rowJson.is_string())
            return false;
        const std::vector<std::string> tokens = SplitCsv(rowJson.get_ref<const std::string&>());
        if (tokens.size() != static_cast<size_t>(size))
            return false;
        for (int32 x = 0; x < size; ++x)
        {
            if (!PatternTokenToCell(tokens[static_cast<size_t>(x)],
                                    outRule.Pattern[static_cast<size_t>(y) *
                                                        static_cast<size_t>(size) +
                                                    static_cast<size_t>(x)]))
            {
                return false;
            }
        }
    }

    if (const json* stampJson = JsonRead::Array(ruleJson, "stamp"))
    {
        outRule.OutputMode = RuleOutputMode::Stamp;
        outRule.Stamp.assign(static_cast<size_t>(size) * static_cast<size_t>(size), kEmptyCell);
        if (stampJson->size() != static_cast<size_t>(size))
            return false;
        for (int32 y = 0; y < size; ++y)
        {
            const auto& rowJson = (*stampJson)[y];
            if (!rowJson.is_string())
                return false;
            const std::vector<std::string> tokens = SplitCsv(rowJson.get_ref<const std::string&>());
            if (tokens.size() != static_cast<size_t>(size))
                return false;
            for (int32 x = 0; x < size; ++x)
            {
                const std::string& token = tokens[static_cast<size_t>(x)];
                uint32 value = 0;
                if (std::from_chars(token.data(), token.data() + token.size(), value).ec !=
                    std::errc())
                {
                    return false;
                }
                outRule.Stamp[static_cast<size_t>(y) * static_cast<size_t>(size) +
                              static_cast<size_t>(x)] = value;
            }
        }
    }
    else
    {
        outRule.OutputMode = RuleOutputMode::WeightedTiles;
        if (const json* tilesJson = JsonRead::Array(ruleJson, "tiles"))
        {
            for (const auto& tileJson : *tilesJson)
            {
                if (!JsonRead::FitsFloat32(JsonRead::Number(tileJson, "weight", 1.0)))
                    return false;
                outRule.Tiles.push_back(
                    RuleTileOutput{ReadUInt32(tileJson, "cell", 0),
                                   static_cast<float32>(JsonRead::Number(tileJson, "weight", 1.0))});
            }
        }
    }

    if (!JsonRead::FitsFloat32(JsonRead::Number(ruleJson, "chance", 1.0)))
        return false;
    outRule.Chance = static_cast<float32>(JsonRead::Number(ruleJson, "chance", 1.0));
    outRule.BreakOnMatch = JsonRead::Boolean(ruleJson, "breakOnMatch", true);
    outRule.MirrorX = JsonRead::Boolean(ruleJson, "mirrorX", false);
    outRule.MirrorY = JsonRead::Boolean(ruleJson, "mirrorY", false);
    outRule.ModuloX = ReadInt32(ruleJson, "moduloX", 1);
    outRule.ModuloY = ReadInt32(ruleJson, "moduloY", 1);
    outRule.OffsetX = ReadInt32(ruleJson, "offsetX", 0);
    outRule.OffsetY = ReadInt32(ruleJson, "offsetY", 0);
    return true;
}

json RulesToJson(const TileRuleSet& rules)
{
    json rulesJson;
    rulesJson["sourceLayer"] = rules.SourceLayerId;
    rulesJson["seed"] = rules.Seed;
    rulesJson["nextGroupId"] = rules.NextGroupId;
    rulesJson["nextRuleId"] = rules.NextRuleId;

    json groups = json::array();
    for (const TileRuleGroup& group : rules.Groups)
    {
        json groupJson;
        groupJson["id"] = group.Id;
        groupJson["name"] = group.Name;
        groupJson["enabled"] = group.Enabled;
        json ruleArray = json::array();
        for (const TileRule& rule : group.Rules)
            ruleArray.push_back(RuleToJson(rule));
        groupJson["rules"] = std::move(ruleArray);
        groups.push_back(std::move(groupJson));
    }
    rulesJson["groups"] = std::move(groups);
    return rulesJson;
}

bool RulesFromJson(const json& rulesJson, TileRuleSet& outRules)
{
    if (!rulesJson.is_object())
        return false;
    outRules.SourceLayerId = ReadUInt32(rulesJson, "sourceLayer", 0);
    outRules.Seed = ReadUInt32(rulesJson, "seed", 0);
    outRules.NextGroupId = ReadUInt32(rulesJson, "nextGroupId", 1);
    outRules.NextRuleId = ReadUInt32(rulesJson, "nextRuleId", 1);
    if (!JsonRead::Member(rulesJson, "groups"))
        return true;
    const json* groupsJson = JsonRead::Array(rulesJson, "groups");
    if (!groupsJson)
        return false;

    for (const auto& groupJson : *groupsJson)
    {
        TileRuleGroup group;
        group.Id = ReadUInt32(groupJson, "id", 0);
        group.Name = JsonRead::Text(groupJson, "name");
        group.Enabled = JsonRead::Boolean(groupJson, "enabled", true);
        if (JsonRead::Member(groupJson, "rules"))
        {
            const json* groupRules = JsonRead::Array(groupJson, "rules");
            if (!groupRules)
                return false;
            for (const auto& ruleJson : *groupRules)
            {
                TileRule rule;
                if (!RuleFromJson(ruleJson, rule))
                    return false;
                group.Rules.push_back(std::move(rule));
            }
        }
        outRules.Groups.push_back(std::move(group));
    }
    return true;
}

json AutoChunksToJson(const TileMapLayerData& layer)
{
    json chunks = json::array();
    for (const ChunkCoord coord : layer.SortedAutoChunkCoords())
    {
        const AutoTileChunk* chunk = layer.FindAutoChunk(coord);
        if (!chunk)
            continue;
        json chunkJson;
        chunkJson["x"] = coord.X;
        chunkJson["y"] = coord.Y;
        chunkJson["rows"] = chunk->Rows;
        chunks.push_back(std::move(chunkJson));
    }
    return chunks;
}

bool AutoChunksFromJson(const json& chunksJson, TileMapLayerData& layer)
{
    if (!chunksJson.is_array())
        return false;
    for (const auto& chunkJson : chunksJson)
    {
        const json* rowsJson = JsonRead::Array(chunkJson, "rows");
        if (!rowsJson || rowsJson->size() != static_cast<size_t>(kChunkSize))
            return false;
        ChunkCoord coord{};
        if (!ReadChunkAxis(chunkJson, "x", coord.X) || !ReadChunkAxis(chunkJson, "y", coord.Y))
            return false;
        AutoTileChunk& chunk = layer.GetOrCreateAutoChunk(coord);
        for (int32 y = 0; y < kChunkSize; ++y)
        {
            const auto& rowJson = (*rowsJson)[y];
            if (!rowJson.is_number_unsigned() || rowJson.get<uint64>() > UINT32_MAX)
                return false;
            chunk.Rows[static_cast<size_t>(y)] = rowJson.get<uint32>();
        }
    }
    return true;
}

} // namespace

bool TileMapAsset::Load()
{
    SetState(AssetState::Loading);
    String text;
    if (!ReadFileTextShared(GetPath(), text))
    {
        Logger::Log::Error("TileMapAsset: cannot open '{}'", GetPath().string());
        SetState(AssetState::Failed);
        return false;
    }
    return ParseJson(text);
}

bool TileMapAsset::LoadFromData(const Vector<uint8>& data)
{
    SetState(AssetState::Loading);
    const std::string text(reinterpret_cast<const char*>(data.data()), data.size());
    return ParseJson(text);
}

void TileMapAsset::Unload()
{
    m_Data.Clear();
    SetState(AssetState::Unloaded);
}

bool TileMapAsset::SaveToData(Vector<uint8>& outData) const
{
    const std::string serialized = SerializeJson();
    outData.assign(serialized.begin(), serialized.end());
    return true;
}

bool TileMapAsset::ParseJson(const std::string& text)
{
    const auto reject = [this](const std::string& reason)
    {
        Logger::Log::Error("TileMapAsset: rejected '{}': {}", GetPath().string(), reason);
        m_Data.Clear();
        SetState(AssetState::Failed);
        return false;
    };

    // allow_exceptions = false: a malformed document reads back as discarded.
    const json doc = json::parse(text, nullptr, /*allow_exceptions*/ false);
    if (doc.is_discarded() || !doc.is_object())
        return reject("not a JSON object");

    m_Data.Clear();
    m_Data.CellWidth = static_cast<float32>(JsonRead::Number(doc, "cellWidth", 1.0));
    m_Data.CellHeight = static_cast<float32>(JsonRead::Number(doc, "cellHeight", 1.0));
    if (!(m_Data.CellWidth > 0.0f) || !(m_Data.CellHeight > 0.0f) ||
        !std::isfinite(m_Data.CellWidth) || !std::isfinite(m_Data.CellHeight))
        return reject("cell size must be positive");

    const json* layersJson = JsonRead::Array(doc, "layers");
    if (!layersJson)
        return reject("no layers array");
    {
        // Layer ids address layers; a repeat would make every layer after the
        // first unreachable through FindLayer.
        std::unordered_set<uint32> seenIds;
        for (const auto& layerJson : *layersJson)
        {
            m_Data.AddLayer(JsonRead::Text(layerJson, "name"),
                                LayerTypeFromName(JsonRead::Text(layerJson, "type", "tiles")));
            TileMapLayerData& layer = m_Data.Layers().back();
            // 0 means "no layer", and UINT32_MAX would leave the id counter
            // with nothing to hand out next; an id that is not an integer at
            // all names no layer the map's references could reach.
            const int64 id = JsonRead::Integer(layerJson, "id", 0);
            if (id <= 0 || id >= static_cast<int64>(UINT32_MAX))
                return reject("layer id " + std::to_string(id) + " is not assignable");
            layer.Id = static_cast<uint32>(id);
            if (!seenIds.insert(layer.Id).second)
                return reject("two layers share id " + std::to_string(layer.Id));
            if (const json* tileSet = JsonRead::Member(layerJson, "tileset");
                tileSet && tileSet->is_string())
            {
                layer.TileSetGuid = GUID(tileSet->get<std::string>());
            }
            layer.Locked = JsonRead::Boolean(layerJson, "locked", false);

            if (const json* chunks = JsonRead::Member(layerJson, "chunks");
                chunks && !JsonToChunks(*chunks, &TileMapLayerData::GetOrCreateChunk, layer,
                                        &TileMapChunk::Cells))
            {
                return reject("malformed chunk data in layer '" + layer.Name + "'");
            }
            if (const json* intChunks = JsonRead::Member(layerJson, "intChunks");
                intChunks && !JsonToChunks(*intChunks, &TileMapLayerData::GetOrCreateIntGridChunk,
                                           layer, &IntGridChunk::Values))
            {
                return reject("malformed intgrid data in layer '" + layer.Name + "'");
            }
            if (const json* autoChunks = JsonRead::Member(layerJson, "autoChunks");
                autoChunks && !AutoChunksFromJson(*autoChunks, layer))
            {
                return reject("malformed provenance data in layer '" + layer.Name + "'");
            }
            if (const json* intValues = JsonRead::Array(layerJson, "intValues"))
            {
                for (const auto& valueJson : *intValues)
                {
                    const int64 value = JsonRead::Integer(valueJson, "value", 0);
                    if (value <= 0 || value > 0xFF)
                        continue;
                    layer.IntGridValues.push_back(
                        IntGridValueDef{static_cast<uint8>(value), JsonRead::Text(valueJson, "name"),
                                        ReadUInt32(valueJson, "colorRGBA", 0xFFFFFFFFu)});
                }
            }
            if (const json* rules = JsonRead::Member(layerJson, "rules");
                rules && !RulesFromJson(*rules, layer.Rules))
            {
                return reject("malformed rules in layer '" + layer.Name + "'");
            }
        }
    }

    m_Data.RestoreNextLayerId(ReadUInt32(doc, "nextLayerId", 1));

    SetState(AssetState::Loaded);
    return true;
}

std::string TileMapAsset::SerializeJson() const
{
    // CompactEmptyChunks is the caller's job (editor save path); serialization
    // itself must not mutate, so all-empty chunks that remain are written out.
    json doc;
    doc["schemaVersion"] = kSchemaVersion;
    doc["assetType"] = "TileMap";
    doc["cellWidth"] = JsonWrite::ShortestFloat(m_Data.CellWidth);
    doc["cellHeight"] = JsonWrite::ShortestFloat(m_Data.CellHeight);
    doc["nextLayerId"] = m_Data.NextLayerId();

    json layers = json::array();
    for (const auto& layer : m_Data.Layers())
    {
        json layerJson;
        layerJson["id"] = layer.Id;
        layerJson["name"] = layer.Name;
        layerJson["type"] = LayerTypeName(layer.Type);
        layerJson["tileset"] = layer.TileSetGuid.ToString();
        // Only when set: an unlocked layer is the overwhelming majority, and a
        // false on every layer is noise in the diff of a hand-edited .tilemap.
        if (layer.Locked)
            layerJson["locked"] = true;
        layerJson["chunks"] = ChunksToJson(layer.SortedChunkCoords(),
                                           &TileMapLayerData::FindChunk, layer,
                                           &TileMapChunk::Cells);
        json intChunks = ChunksToJson(layer.SortedIntGridChunkCoords(),
                                      &TileMapLayerData::FindIntGridChunk, layer,
                                      &IntGridChunk::Values);
        if (!intChunks.empty())
            layerJson["intChunks"] = std::move(intChunks);

        // The phase-6 blocks are written only when they carry something, so a
        // map authored without IntGrid semantics serializes byte-identically to
        // what an earlier build produced.
        if (!layer.IntGridValues.empty())
        {
            json values = json::array();
            for (const IntGridValueDef& def : layer.IntGridValues)
            {
                values.push_back(json{{"value", def.Value},
                                      {"name", def.Name},
                                      {"colorRGBA", def.ColorRGBA}});
            }
            layerJson["intValues"] = std::move(values);
        }
        json autoChunks = AutoChunksToJson(layer);
        if (!autoChunks.empty())
            layerJson["autoChunks"] = std::move(autoChunks);
        if (!layer.Rules.IsEmpty() || layer.Rules.SourceLayerId != 0)
            layerJson["rules"] = RulesToJson(layer.Rules);

        layers.push_back(std::move(layerJson));
    }
    doc["layers"] = std::move(layers);

    return doc.dump(2);
}

} // namespace TileMap
} // namespace GameEngine
