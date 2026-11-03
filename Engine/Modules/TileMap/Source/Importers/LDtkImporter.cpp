#include "TileMap/Importers/LDtkImporter.h"

#include "AssetCore/SharedFileRead.h"
#include "Importers/ImportCommon.h"
#include "Importers/ImportBudget.h"
#include "JsonRead.h"
#include "TileMap/TileCellEncoding.h"

#include <nlohmann/json.hpp>

#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

namespace GameEngine
{
namespace TileMap
{
namespace Importers
{

namespace
{
using json = nlohmann::json;

// LDtk gridTiles/autoLayerTiles flip bits.
constexpr int64 kLDtkFlipX = 1;
constexpr int64 kLDtkFlipY = 2;

struct LDtkTileSetDef
{
    int64 Uid = 0;
    std::string Identifier;
    int32 GridSize = 0;
    int32 Spacing = 0;
    int32 Padding = 0;
    int32 PxWid = 0;
    int32 PxHei = 0;
    int32 Columns = 0;
    int32 Rows = 0;
    size_t ResultIndex = 0; // index into TileMapImportResult::TileSets
};

struct LDtkLayerDef
{
    std::vector<IntGridValueDef> IntGridValues;
};

struct LDtkContext
{
    ImportBudget Budget;
    std::filesystem::path ProjectPath;
    const TextureGuidResolver* ResolveTexture = nullptr;
    TileMapImportResult* Result = nullptr;
    std::unordered_set<std::string> OutputNames;
    std::unordered_map<int64, LDtkTileSetDef> TileSets;
    std::unordered_map<int64, LDtkLayerDef> LayerDefs;
};

void Warn(LDtkContext& ctx, std::string message)
{
    ctx.Result->Warnings.push_back(std::move(message));
}

bool Fail(TileMapImportResult& result, std::string error)
{
    result.Success = false;
    result.Error = std::move(error);
    result.TileSets.clear();
    result.Maps.clear();
    return false;
}

bool ReadInteger(const json& value, int64& output)
{
    if (!value.is_number_integer() ||
        (value.is_number_unsigned() && value.get<uint64>() > static_cast<uint64>(std::numeric_limits<int64>::max())))
        return false;
    output = value.get<int64>();
    return true;
}

int32 ReadClampedInt(const json& node, const char* key, int32 minValue, int32 maxValue)
{
    const int64 raw = JsonRead::Integer(node, key, 0);
    if (raw < minValue)
        return minValue;
    if (raw > maxValue)
        return maxValue;
    return static_cast<int32>(raw);
}

bool ChargeJsonItems(ImportBudget& budget, const json& root)
{
    std::vector<const json*> pending{&root};
    while (!pending.empty())
    {
        const json& value = *pending.back();
        pending.pop_back();
        if (!value.is_structured())
            continue;
        if (!budget.Items(value.size()))
            return false;
        for (const json& child : value)
            if (child.is_structured())
                pending.push_back(&child);
    }
    return true;
}

bool ImportTileSetDefs(LDtkContext& ctx, const json& defs)
{
    if (!defs.contains("tilesets") || !defs["tilesets"].is_array())
        return Fail(*ctx.Result, "LDtk import: defs.tilesets missing or not an array");
    if (defs["tilesets"].size() > static_cast<size_t>(kMaxImportTileSets))
        return Fail(*ctx.Result, "LDtk import: too many tileset definitions");

    for (const json& setJson : defs["tilesets"])
    {
        if (!setJson.is_object())
            continue;
        LDtkTileSetDef def;
        def.Uid = JsonRead::Integer(setJson, "uid", -1);
        def.Identifier = JsonRead::Text(setJson, "identifier", "Tileset");
        def.GridSize = ReadClampedInt(setJson, "tileGridSize", 0, kMaxImportTilePixels);
        def.Spacing = ReadClampedInt(setJson, "spacing", 0, kMaxImportTilePixels);
        def.Padding = ReadClampedInt(setJson, "padding", 0, kMaxImportTilePixels);
        def.PxWid = ReadClampedInt(setJson, "pxWid", 0, 1 << 20);
        def.PxHei = ReadClampedInt(setJson, "pxHei", 0, 1 << 20);

        // Internal-icon tilesets (relPath null) and embedded atlases have no
        // importable image; skip them and any layer that draws from them.
        std::string relPath;
        if (setJson.contains("relPath") && setJson["relPath"].is_string())
            relPath = setJson["relPath"].get<std::string>();
        if (relPath.empty())
        {
            Warn(ctx, "LDtk import: tileset '" + def.Identifier +
                          "' has no image path; skipped (layers using it import empty)");
            continue;
        }
        if (def.GridSize <= 0 || def.PxWid <= 0 || def.PxHei <= 0)
        {
            Warn(ctx, "LDtk import: tileset '" + def.Identifier +
                          "' has a degenerate grid; skipped");
            continue;
        }

        def.Columns = TileSetAsset::GridColumns(def.PxWid, def.GridSize, def.Padding, def.Spacing);
        def.Rows = TileSetAsset::GridRows(def.PxHei, def.GridSize, def.Padding, def.Spacing);
        if (def.Columns <= 0 || def.Rows <= 0 ||
            static_cast<int64>(def.Columns) * def.Rows > kMaxImportTilesPerSet)
        {
            Warn(ctx, "LDtk import: tileset '" + def.Identifier +
                          "' slices to an unusable tile count; skipped");
            continue;
        }

        ImportedTileSet imported;
        imported.Guid = GUID::Generate();
        imported.Name = SanitizeAssetName(def.Identifier);
        imported.ImagePath = ResolveRelativeTo(ctx.ProjectPath, relPath);
        if (imported.ImagePath.empty())
        {
            return Fail(*ctx.Result, "LDtk import: tileset '" + def.Identifier +
                                         "' names an atlas image outside the project directory");
        }

        if (!ReserveImportName(ctx.OutputNames, imported.Name, ".tileset"))
            return Fail(*ctx.Result, "LDtk import: duplicate output filename '" + imported.Name +
                                         ".tileset'; rename the source tilesets to unique names");

        GUID atlasGuid;
        if (ctx.ResolveTexture && *ctx.ResolveTexture)
            atlasGuid = (*ctx.ResolveTexture)(imported.ImagePath);
        if (atlasGuid.IsNull())
            Warn(ctx, "LDtk import: atlas image '" + PathText(imported.ImagePath) +
                          "' did not resolve to a texture; tileset '" + def.Identifier +
                          "' imports untextured");

        auto asset = std::make_unique<TileSetAsset>(
            imported.Guid, ctx.ProjectPath.parent_path() / (imported.Name + ".tileset"));
        if (!ctx.Budget.Tiles(static_cast<uint64>(def.Columns) * def.Rows))
            return Fail(*ctx.Result, "LDtk import: exceeds the whole-import tile budget; split the source project");
        asset->SetAtlasTexture(atlasGuid, def.PxWid, def.PxHei);
        asset->SliceGrid(def.GridSize, def.GridSize, def.Padding, def.Spacing);

        // Per-tile custom data and enum tags, where they map onto our custom values.
        if (setJson.contains("customData") && setJson["customData"].is_array())
        {
            for (const json& dataJson : setJson["customData"])
            {
                const int64 tileId = JsonRead::Integer(dataJson, "tileId", -1);
                if (tileId < 0 || tileId >= static_cast<int64>(kMaxImportTilesPerSet))
                    continue;
                if (TileSetTile* tile = asset->FindTile(static_cast<uint32>(tileId) + 1))
                    tile->CustomData["data"] = JsonRead::Text(dataJson, "data");
            }
        }
        if (setJson.contains("enumTags") && setJson["enumTags"].is_array())
        {
            for (const json& tagJson : setJson["enumTags"])
            {
                const std::string enumValue = JsonRead::Text(tagJson, "enumValueId");
                if (enumValue.empty() || !tagJson.contains("tileIds") ||
                    !tagJson["tileIds"].is_array())
                    continue;
                for (const json& idJson : tagJson["tileIds"])
                {
                    int64 tileId = 0;
                    if (!ReadInteger(idJson, tileId))
                        continue;
                    if (tileId < 0 || tileId >= static_cast<int64>(kMaxImportTilesPerSet))
                        continue;
                    if (TileSetTile* tile = asset->FindTile(static_cast<uint32>(tileId) + 1))
                        tile->CustomData[enumValue] = true;
                }
            }
        }

        // Layer instances bind to a tileset by uid, so a repeated uid would
        // silently bind them to whichever definition landed first.
        if (ctx.TileSets.find(def.Uid) != ctx.TileSets.end())
        {
            return Fail(*ctx.Result, "LDtk import: two tilesets share uid " +
                                         std::to_string(def.Uid));
        }

        imported.Asset = std::move(asset);
        def.ResultIndex = ctx.Result->TileSets.size();
        ctx.Result->TileSets.push_back(std::move(imported));
        ctx.TileSets.emplace(def.Uid, def);
    }
    return true;
}

void ImportLayerDefs(LDtkContext& ctx, const json& defs)
{
    if (!defs.contains("layers") || !defs["layers"].is_array())
        return;
    for (const json& layerJson : defs["layers"])
    {
        if (!layerJson.is_object())
            continue;
        LDtkLayerDef def;
        if (layerJson.contains("intGridValues") && layerJson["intGridValues"].is_array())
        {
            for (const json& valueJson : layerJson["intGridValues"])
            {
                const int64 value = JsonRead::Integer(valueJson, "value", 0);
                if (value <= 0 || value > 0xFF)
                    continue;
                def.IntGridValues.push_back(IntGridValueDef{
                    static_cast<uint8>(value), JsonRead::Text(valueJson, "identifier"),
                    ParseHexColorRGBA(JsonRead::Text(valueJson, "color"))});
            }
        }
        ctx.LayerDefs.emplace(JsonRead::Integer(layerJson, "uid", -1), std::move(def));
    }
}

/** Reads an LDtk [x, y] pair; false when it is not two integers. */
bool ReadIntegerPair(const json* pair, int64& outX, int64& outY)
{
    if (!pair || !pair->is_array() || pair->size() != 2 || !(*pair)[0].is_number_integer() ||
        !(*pair)[1].is_number_integer())
        return false;
    return ReadInteger((*pair)[0], outX) && ReadInteger((*pair)[1], outY);
}

/** Writes one LDtk tile array (gridTiles/autoLayerTiles) into `layer`. */
bool ImportTileArray(LDtkContext& ctx, const json& tiles, TileMapLayerData& layer,
                     const LDtkTileSetDef& tileSet, int32 gridSize, int32 cWid, int32 cHei)
{
    int32 skipped = 0;
    int32 overlapped = 0;
    for (const json& tileJson : tiles)
    {
        int64 pxX = 0;
        int64 pxY = 0;
        int64 srcX = 0;
        int64 srcY = 0;
        if (!ReadIntegerPair(JsonRead::Member(tileJson, "px"), pxX, pxY) ||
            !ReadIntegerPair(JsonRead::Member(tileJson, "src"), srcX, srcY))
        {
            ++skipped;
            continue;
        }
        const int64 cellX = pxX / gridSize;
        const int64 cellY = pxY / gridSize;
        if (pxX < 0 || pxY < 0 || cellX >= cWid || cellY >= cHei)
        {
            ++skipped;
            continue;
        }

        if (srcX < tileSet.Padding || srcY < tileSet.Padding)
        {
            ++skipped;
            continue;
        }
        const int64 stride = static_cast<int64>(tileSet.GridSize) + tileSet.Spacing;
        const int64 column = (srcX - tileSet.Padding) / stride;
        const int64 row = (srcY - tileSet.Padding) / stride;
        if (column >= tileSet.Columns || row >= tileSet.Rows)
        {
            ++skipped;
            continue;
        }
        const uint32 tileId = static_cast<uint32>(row * tileSet.Columns + column) + 1;

        const int64 flips = JsonRead::Integer(tileJson, "f", 0);
        const int32 engineY = (cHei - 1) - static_cast<int32>(cellY);
        const int32 engineX = static_cast<int32>(cellX);
        if (!CellIsEmpty(layer.GetCell(engineX, engineY)))
            ++overlapped;
        // Array order is display order (later on top); the last write wins,
        // which keeps the topmost tile of an LDtk rule stack.
        if (!layer.FindChunk(ChunkOfCell(engineX, engineY)) && !ctx.Budget.Chunk())
            return Fail(*ctx.Result, "LDtk import: exceeds the whole-import chunk budget; split the source project");
        layer.SetCell(engineX, engineY,
                      MakeCell(tileId, (flips & kLDtkFlipX) != 0, (flips & kLDtkFlipY) != 0));
    }
    if (skipped > 0)
        Warn(ctx, "LDtk import: layer '" + layer.Name + "': skipped " + std::to_string(skipped) +
                      " out-of-range or malformed tiles");
    if (overlapped > 0)
        Warn(ctx, "LDtk import: layer '" + layer.Name + "': " + std::to_string(overlapped) +
                      " cells stacked several tiles; kept the topmost");
    return true;
}

bool ImportLevel(LDtkContext& ctx, const json& level)
{
    const std::string levelName = JsonRead::Text(level, "identifier", "Level");
    if (!level.contains("layerInstances") || !level["layerInstances"].is_array())
    {
        Warn(ctx, "LDtk import: level '" + levelName + "' has no layer instances; skipped");
        return true;
    }
    const json& layerInstances = level["layerInstances"];
    if (layerInstances.size() > static_cast<size_t>(kMaxImportLayers))
        return Fail(*ctx.Result, "LDtk import: level '" + levelName + "' has too many layers");

    ImportedTileMap imported;
    imported.Name = SanitizeAssetName(PathText(ctx.ProjectPath.stem()) + "_" + levelName);
    if (!ReserveImportName(ctx.OutputNames, imported.Name, ".tilemap"))
        return Fail(*ctx.Result, "LDtk import: duplicate output filename '" + imported.Name +
                                     ".tilemap'; rename the source levels to unique names");
    auto asset = std::make_unique<TileMapAsset>(
        GUID::Generate(), ctx.ProjectPath.parent_path() / (imported.Name + ".tilemap"));
    TileMapData& data = asset->Data();

    for (const json& inst : layerInstances)
    {
        if (!inst.is_object())
            continue;
        const std::string type = JsonRead::Text(inst, "__type");
        if (type != "IntGrid" && type != "Tiles" && type != "AutoLayer")
            continue;
        const std::string name = JsonRead::Text(inst, "__identifier", "Layer");
        const int32 cWid = ReadClampedInt(inst, "__cWid", 0, kMaxImportCellsPerAxis);
        const int32 cHei = ReadClampedInt(inst, "__cHei", 0, kMaxImportCellsPerAxis);
        const int32 gridSize = ReadClampedInt(inst, "__gridSize", 0, kMaxImportCellsPerAxis);
        if (cWid <= 0 || cHei <= 0 || gridSize <= 0 ||
            static_cast<int64>(cWid) * cHei > kMaxImportCellsPerLayer)
        {
            Warn(ctx, "LDtk import: layer '" + name + "' has degenerate or oversized bounds; skipped");
            continue;
        }

        if (!ctx.Budget.Cells(static_cast<uint64>(cWid) * cHei))
            return Fail(*ctx.Result, "LDtk import: exceeds the whole-import cell budget; split the source project");
        const LDtkTileSetDef* tileSet = nullptr;
        int64 tileSetUid = 0;
        if (inst.contains("__tilesetDefUid") && ReadInteger(inst["__tilesetDefUid"], tileSetUid))
        {
            const auto it = ctx.TileSets.find(tileSetUid);
            if (it != ctx.TileSets.end())
                tileSet = &it->second;
        }

        const bool hasAutoTiles = inst.contains("autoLayerTiles") &&
                                  inst["autoLayerTiles"].is_array() &&
                                  !inst["autoLayerTiles"].empty();

        if (type == "IntGrid")
        {
            const uint32 layerId = data.AddLayer(name, TileMapLayerType::IntGrid);
            TileMapLayerData& layer = *data.FindLayer(layerId);
            const auto defIt = ctx.LayerDefs.find(JsonRead::Integer(inst, "layerDefUid", -1));
            if (defIt != ctx.LayerDefs.end())
                layer.IntGridValues = defIt->second.IntGridValues;

            if (inst.contains("intGridCsv") && inst["intGridCsv"].is_array())
            {
                const json& csv = inst["intGridCsv"];
                if (csv.size() != static_cast<size_t>(cWid) * static_cast<size_t>(cHei))
                {
                    Warn(ctx, "LDtk import: layer '" + name +
                                  "': intGridCsv length does not match bounds; values skipped");
                }
                else
                {
                    int32 skipped = 0;
                    for (int32 cy = 0; cy < cHei; ++cy)
                    {
                        for (int32 cx = 0; cx < cWid; ++cx)
                        {
                            const json& v = csv[static_cast<size_t>(cy) * cWid + cx];
                            int64 value = -1;
                            ReadInteger(v, value);
                            if (value == 0)
                                continue;
                            if (value < 0 || value > 0xFF)
                            {
                                ++skipped;
                                continue;
                            }
                            if (!layer.FindIntGridChunk(ChunkOfCell(cx, (cHei - 1) - cy)) && !ctx.Budget.Chunk())
                                return Fail(*ctx.Result, "LDtk import: exceeds the whole-import chunk budget; split the source project");
                            layer.SetIntGridValue(cx, (cHei - 1) - cy,
                                                  static_cast<uint8>(value));
                        }
                    }
                    if (skipped > 0)
                        Warn(ctx, "LDtk import: layer '" + name + "': skipped " +
                                      std::to_string(skipped) +
                                      " IntGrid values outside 1..255");
                }
            }

            // The dressed output of the layer's rules, as a sibling Tiles layer.
            if (hasAutoTiles && tileSet)
            {
                const uint32 tilesLayerId = data.AddLayer(name + " (tiles)", TileMapLayerType::Tiles);
                TileMapLayerData& tilesLayer = *data.FindLayer(tilesLayerId);
                tilesLayer.TileSetGuid = ctx.Result->TileSets[tileSet->ResultIndex].Guid;
                if (!ImportTileArray(ctx, inst["autoLayerTiles"], tilesLayer, *tileSet, gridSize, cWid, cHei))
                    return false;
            }
        }
        else if (type == "Tiles" || type == "AutoLayer")
        {
            if (!tileSet)
            {
                Warn(ctx, "LDtk import: layer '" + name + "' has no importable tileset; skipped");
                continue;
            }
            const uint32 layerId = data.AddLayer(name, TileMapLayerType::Tiles);
            TileMapLayerData& layer = *data.FindLayer(layerId);
            layer.TileSetGuid = ctx.Result->TileSets[tileSet->ResultIndex].Guid;
            if (inst.contains("gridTiles") && inst["gridTiles"].is_array() &&
                !ImportTileArray(ctx, inst["gridTiles"], layer, *tileSet, gridSize, cWid, cHei))
                return false;
            if (hasAutoTiles && !ImportTileArray(ctx, inst["autoLayerTiles"], layer, *tileSet, gridSize, cWid, cHei))
                return false;
        }
        // Entities layers have no tilemap representation; skipped silently.
    }

    imported.Asset = std::move(asset);
    ctx.Result->Maps.push_back(std::move(imported));
    return true;
}

} // namespace

TileMapImportResult ImportLDtkProject(const std::filesystem::path& ldtkPath,
                                      const TextureGuidResolver& resolveTexture)
{
    TileMapImportResult result;
    LDtkContext ctx;
    const SourceFileCharge projectCharge = ctx.Budget.ReadFile(ldtkPath);
    if (projectCharge == SourceFileCharge::NotARegularFile)
    {
        Fail(result, "LDtk import: cannot read '" + PathText(ldtkPath) + "': missing or not a regular file");
        return result;
    }
    if (projectCharge == SourceFileCharge::OverBudget)
    {
        Fail(result, "LDtk import: exceeds the whole-import source byte budget; split the source project");
        return result;
    }
    String text;
    if (!ReadFileTextShared(ldtkPath, text))
    {
        Fail(result, "LDtk import: cannot read '" + PathText(ldtkPath) + "'");
        return result;
    }

    ctx.ProjectPath = ldtkPath;
    ctx.ResolveTexture = &resolveTexture;
    ctx.Result = &result;

    // allow_exceptions = false: a malformed document reads back as discarded.
    // Nothing in this importer throws, because catch(const std::exception&) does
    // not fire in every build that links it.
    const json doc = json::parse(text, nullptr, /*allow_exceptions*/ false);
    if (doc.is_discarded())
    {
        Fail(result, "LDtk import: malformed JSON in '" + PathText(ldtkPath) + "'");
        return result;
    }
    if (!ChargeJsonItems(ctx.Budget, doc))
    {
        Fail(result, "LDtk import: exceeds the whole-import item budget; split the source project");
        return result;
    }
    const json* defs = JsonRead::Object(doc, "defs");
    if (!defs)
    {
        Fail(result, "LDtk import: '" + PathText(ldtkPath) +
                         "' is not an LDtk project (no defs object)");
        return result;
    }
    if (!ImportTileSetDefs(ctx, *defs))
        return result;
    ImportLayerDefs(ctx, *defs);

    const json* levels = JsonRead::Array(doc, "levels");
    if (!levels)
    {
        Fail(result, "LDtk import: no levels array");
        return result;
    }
    if (levels->size() > static_cast<size_t>(kMaxImportLevels))
    {
        Fail(result, "LDtk import: too many levels");
        return result;
    }

    for (const json& level : *levels)
    {
        if (!level.is_object())
            continue;
        // External levels hold their layer instances in a sibling .ldtkl file.
        const json* externalRelPath = JsonRead::Member(level, "externalRelPath");
        const json* layerInstances = JsonRead::Member(level, "layerInstances");
        if (externalRelPath && externalRelPath->is_string() &&
            (!layerInstances || layerInstances->is_null()))
        {
            const std::string relative = externalRelPath->get<std::string>();
            const std::filesystem::path levelPath = ResolveRelativeTo(ldtkPath, relative);
            if (levelPath.empty())
            {
                Warn(ctx, "LDtk import: external level '" + relative +
                              "' resolves outside the project directory; skipped");
                continue;
            }
            const SourceFileCharge levelCharge = ctx.Budget.ReadFile(levelPath);
            if (levelCharge == SourceFileCharge::NotARegularFile)
            {
                Warn(ctx, "LDtk import: cannot read external level '" + PathText(levelPath) +
                              "': missing or not a regular file; skipped");
                continue;
            }
            if (levelCharge == SourceFileCharge::OverBudget)
            {
                Fail(result, "LDtk import: exceeds the whole-import source byte budget; split the source project");
                return result;
            }
            String levelText;
            if (!ReadFileTextShared(levelPath, levelText))
            {
                Warn(ctx, "LDtk import: cannot read external level '" + PathText(levelPath) +
                              "'; skipped");
                continue;
            }
            const json externalLevel = json::parse(levelText, nullptr, /*allow_exceptions*/ false);
            if (externalLevel.is_discarded())
            {
                Warn(ctx, "LDtk import: malformed JSON in external level '" + PathText(levelPath) +
                              "'; skipped");
                continue;
            }
            if (!ChargeJsonItems(ctx.Budget, externalLevel))
            {
                Fail(result, "LDtk import: exceeds the whole-import item budget; split the source project");
                return result;
            }
            if (!externalLevel.is_object())
            {
                Fail(result, "LDtk import: external level root must be an object");
                return result;
            }
            if (!ImportLevel(ctx, externalLevel))
                return result;
            continue;
        }
        if (!ImportLevel(ctx, level))
            return result;
    }

    result.Success = true;
    return result;
}

} // namespace Importers
} // namespace TileMap
} // namespace GameEngine
