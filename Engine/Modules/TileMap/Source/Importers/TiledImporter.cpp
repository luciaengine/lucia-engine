#include "TileMap/Importers/TiledImporter.h"

#include "Importers/ImportCommon.h"
#include "Importers/ImportBudget.h"
#include "TileMap/TileCellEncoding.h"

#include <pugixml.hpp>
#include <zlib.h>

#include <algorithm>
#include <charconv>
#include <cstring>
#include <cmath>
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

// Tiled GID flag bits (matching this engine's cell bits by design).
constexpr uint32 kTiledFlipH = 0x80000000u;
constexpr uint32 kTiledFlipV = 0x40000000u;
constexpr uint32 kTiledFlipD = 0x20000000u;
constexpr uint32 kTiledRotateHex120 = 0x10000000u;
constexpr uint32 kTiledGidMask = 0x0FFFFFFFu;

struct TiledTileSetRef
{
    uint32 FirstGid = 0;
    int32 TileCount = 0;
    std::string Name;
    size_t ResultIndex = 0;
};

struct TiledContext
{
    ImportBudget Budget;
    std::filesystem::path TmxPath;
    const TextureGuidResolver* ResolveTexture = nullptr;
    TileMapImportResult* Result = nullptr;
    std::unordered_set<std::string> OutputNames;
    std::vector<TiledTileSetRef> TileSets; // sorted by FirstGid
    int32 MapWidth = 0;
    int32 MapHeight = 0;
    bool Infinite = false;
    int32 LayerCount = 0;
};

void Warn(TiledContext& ctx, std::string message)
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

bool ChargeXmlItems(ImportBudget& budget, const pugi::xml_node& node, int32 depth = 0)
{
    if (depth > kMaxImportLayerDepth + 8 || !budget.Items(1))
        return false;
    for ([[maybe_unused]] const auto& attribute : node.attributes())
        if (!budget.Items(1))
            return false;
    for (const auto& child : node.children())
        if (!ChargeXmlItems(budget, child, depth + 1))
            return false;
    return true;
}

// --- layer <data> decoding --------------------------------------------------

bool DecodeBase64(const std::string& text, std::vector<uint8>& outBytes, std::string& outError)
{
    static constexpr int8 kInvalid = -1;
    int8 table[256];
    std::fill(std::begin(table), std::end(table), kInvalid);
    for (int32 i = 0; i < 26; ++i)
    {
        table[static_cast<uint8>('A' + i)] = static_cast<int8>(i);
        table[static_cast<uint8>('a' + i)] = static_cast<int8>(26 + i);
    }
    for (int32 i = 0; i < 10; ++i)
        table[static_cast<uint8>('0' + i)] = static_cast<int8>(52 + i);
    table[static_cast<uint8>('+')] = 62;
    table[static_cast<uint8>('/')] = 63;

    outBytes.clear();
    outBytes.reserve(text.size() / 4 * 3);
    uint32 accum = 0;
    int32 bits = 0;
    bool padded = false;
    for (const char c : text)
    {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
            continue;
        if (c == '=')
        {
            padded = true;
            continue;
        }
        if (padded || table[static_cast<uint8>(c)] == kInvalid)
        {
            outError = "invalid base64 character";
            return false;
        }
        accum = (accum << 6) | static_cast<uint32>(table[static_cast<uint8>(c)]);
        bits += 6;
        if (bits >= 8)
        {
            bits -= 8;
            outBytes.push_back(static_cast<uint8>((accum >> bits) & 0xFFu));
        }
    }
    return true;
}

/**
 * Inflates zlib- or gzip-wrapped data into exactly `expectedSize` bytes: the
 * size comes from the declared bounds, so output beyond it is an error rather
 * than growth. That bounds one payload only — what bounds a whole layer is the
 * per-layer cell budget its caller charges every chunk against.
 */
bool Inflate(const std::vector<uint8>& compressed, size_t expectedSize,
             std::vector<uint8>& outBytes, std::string& outError)
{
    outBytes.assign(expectedSize, 0);
    z_stream stream{};
    // 15 = max window; +32 auto-detects the zlib vs gzip wrapper.
    if (inflateInit2(&stream, 15 + 32) != Z_OK)
    {
        outError = "zlib initialization failed";
        return false;
    }
    stream.next_in = const_cast<Bytef*>(compressed.data());
    stream.avail_in = static_cast<uInt>(compressed.size());
    stream.next_out = outBytes.data();
    stream.avail_out = static_cast<uInt>(outBytes.size());
    const int status = inflate(&stream, Z_FINISH);
    const uLong produced = stream.total_out;
    inflateEnd(&stream);
    if (status != Z_STREAM_END || produced != expectedSize)
    {
        outError = "compressed layer data is corrupt or does not match the layer bounds";
        return false;
    }
    return true;

}

bool ParseCsvGids(const char* text, size_t expectedCells, std::vector<uint32>& outGids,
                  std::string& outError)
{
    outGids.clear();
    outGids.reserve(expectedCells);
    const char* cursor = text;
    const char* end = text + std::strlen(text);
    while (cursor < end)
    {
        while (cursor < end && (*cursor == ',' || *cursor == ' ' || *cursor == '\t' ||
                                *cursor == '\r' || *cursor == '\n'))
            ++cursor;
        if (cursor >= end)
            break;
        uint64 value = 0;
        const auto parsed = std::from_chars(cursor, end, value);
        if (parsed.ec != std::errc() || value > 0xFFFFFFFFull)
        {
            outError = "malformed CSV tile data";
            return false;
        }
        if (outGids.size() >= expectedCells)
        {
            outError = "CSV tile data has more cells than the layer bounds";
            return false;
        }
        outGids.push_back(static_cast<uint32>(value));
        cursor = parsed.ptr;
    }
    if (outGids.size() != expectedCells)
    {
        outError = "CSV tile data has fewer cells than the layer bounds";
        return false;
    }
    return true;
}

/** Decodes one <data> or <chunk> payload into `expectedCells` GIDs. */
bool DecodeGidPayload(const pugi::xml_node& dataNode, const char* text, size_t expectedCells,
                      std::vector<uint32>& outGids, std::string& outError)
{
    const std::string encoding = dataNode.attribute("encoding").as_string();
    if (encoding == "csv")
        return ParseCsvGids(text, expectedCells, outGids, outError);
    if (encoding != "base64")
    {
        outError = encoding.empty()
                       ? std::string("XML-element tile data is not supported; use CSV or base64")
                       : "unsupported tile data encoding '" + encoding + "'";
        return false;
    }

    std::vector<uint8> bytes;
    if (!DecodeBase64(text, bytes, outError))
        return false;

    const std::string compression = dataNode.attribute("compression").as_string();
    const size_t expectedBytes = expectedCells * 4;
    if (compression == "zlib" || compression == "gzip")
    {
        std::vector<uint8> inflated;
        if (!Inflate(bytes, expectedBytes, inflated, outError))
            return false;
        bytes = std::move(inflated);
    }
    else if (!compression.empty())
    {
        outError = "unsupported tile data compression '" + compression + "'";
        return false;
    }

    if (bytes.size() != expectedBytes)
    {
        outError = "tile data byte count does not match the layer bounds";
        return false;
    }
    outGids.resize(expectedCells);
    for (size_t i = 0; i < expectedCells; ++i)
    {
        outGids[i] = static_cast<uint32>(bytes[i * 4]) |
                     (static_cast<uint32>(bytes[i * 4 + 1]) << 8) |
                     (static_cast<uint32>(bytes[i * 4 + 2]) << 16) |
                     (static_cast<uint32>(bytes[i * 4 + 3]) << 24);
    }
    return true;
}

// --- tileset parsing --------------------------------------------------------

void ParseTileProperties(TiledContext& ctx, const pugi::xml_node& tileNode, TileSetTile& tile)
{
    for (const pugi::xml_node& prop : tileNode.child("properties").children("property"))
    {
        const std::string name = prop.attribute("name").as_string();
        if (name.empty())
            continue;
        const std::string type = prop.attribute("type").as_string();
        const pugi::xml_attribute value = prop.attribute("value");
        if (type == "bool")
            tile.CustomData[name] = value.as_bool();
        else if (type == "int")
            tile.CustomData[name] = static_cast<int64>(value.as_llong());
        else if (type == "float")
        {
            const float64 number = value.as_double();
            if (std::isfinite(number))
                tile.CustomData[name] = number;
            else
                Warn(ctx, "Tiled import: non-finite custom property '" + name + "'; skipped");
        }
        else
            tile.CustomData[name] = std::string(value.as_string());
    }
}

void ParseTileCollision(TiledContext& ctx, const pugi::xml_node& tileNode, TileSetTile& tile,
                        float32 tileW, float32 tileH, const std::string& setName)
{
    for (const pugi::xml_node& object : tileNode.child("objectgroup").children("object"))
    {
        if (object.attribute("rotation").as_float(0.0f) != 0.0f)
        {
            Warn(ctx, "Tiled import: tileset '" + setName +
                          "': rotated collision objects are not supported; skipped");
            continue;
        }
        if (object.child("ellipse") || object.child("point"))
        {
            Warn(ctx, "Tiled import: tileset '" + setName +
                          "': ellipse/point collision objects are not supported; skipped");
            continue;
        }

        const float32 objX = object.attribute("x").as_float(0.0f);
        const float32 objY = object.attribute("y").as_float(0.0f);
        TileCollisionPolygon polygon;

        if (const pugi::xml_node polygonNode = object.child("polygon"))
        {
            // "x0,y0 x1,y1 ..." — coordinates relative to the object position.
            const std::string points = polygonNode.attribute("points").as_string();
            const char* cursor = points.c_str();
            const char* end = cursor + points.size();
            while (cursor < end &&
                   static_cast<int32>(polygon.Points.size()) <= kMaxImportPolygonPoints)
            {
                while (cursor < end && (*cursor == ' ' || *cursor == ','))
                    ++cursor;
                if (cursor >= end)
                    break;
                char* after = nullptr;
                const float32 px = std::strtof(cursor, &after);
                if (after == cursor || after >= end || *after != ',')
                    break;
                cursor = after + 1;
                const float32 py = std::strtof(cursor, &after);
                if (after == cursor)
                    break;
                cursor = after;
                polygon.Points.emplace_back((objX + px) / tileW, 1.0f - (objY + py) / tileH);
            }
            if (static_cast<int32>(polygon.Points.size()) > kMaxImportPolygonPoints)
            {
                Warn(ctx, "Tiled import: tileset '" + setName +
                              "': collision polygon exceeds the point limit; skipped");
                continue;
            }
        }
        else
        {
            const float32 w = object.attribute("width").as_float(0.0f);
            const float32 h = object.attribute("height").as_float(0.0f);
            if (w <= 0.0f || h <= 0.0f)
                continue;
            // Tiled rect origin is its top-left; V flips to this engine's +Y-up
            // tile-local frame.
            polygon.Points.emplace_back(objX / tileW, 1.0f - objY / tileH);
            polygon.Points.emplace_back((objX + w) / tileW, 1.0f - objY / tileH);
            polygon.Points.emplace_back((objX + w) / tileW, 1.0f - (objY + h) / tileH);
            polygon.Points.emplace_back(objX / tileW, 1.0f - (objY + h) / tileH);
        }

        const bool finite = std::all_of(polygon.Points.begin(), polygon.Points.end(),
            [](const auto& point) { return std::isfinite(point.x) && std::isfinite(point.y); });
        if (!finite)
            Warn(ctx, "Tiled import: tileset '" + setName + "': non-finite collision coordinates; skipped");
        else if (polygon.Points.size() >= 3)
            tile.CollisionPolygons.push_back(std::move(polygon));
    }
}

/**
 * Parses one <tileset> element body (inline in the .tmx, or the root of an
 * external .tsx) into an ImportedTileSet. `sourceFile` anchors relative image
 * paths — the .tsx for external tilesets, the .tmx for inline ones.
 */
bool ParseTileSetBody(TiledContext& ctx, const pugi::xml_node& node, uint32 firstGid,
                      const std::filesystem::path& sourceFile)
{
    const std::string name =
        node.attribute("name").as_string(PathText(sourceFile.stem()).c_str());
    const int32 tileW = node.attribute("tilewidth").as_int(0);
    const int32 tileH = node.attribute("tileheight").as_int(0);
    const int32 spacing = node.attribute("spacing").as_int(0);
    const int32 margin = node.attribute("margin").as_int(0);
    if (tileW <= 0 || tileH <= 0 || tileW > kMaxImportTilePixels || tileH > kMaxImportTilePixels ||
        spacing < 0 || margin < 0 || spacing > kMaxImportTilePixels || margin > kMaxImportTilePixels)
        return Fail(*ctx.Result, "Tiled import: tileset '" + name + "' has a degenerate tile size");

    const pugi::xml_node image = node.child("image");
    if (!image)
    {
        return Fail(*ctx.Result,
                    "Tiled import: tileset '" + name +
                        "' is an image-collection tileset (per-tile images), which is not "
                        "supported — re-export it as a single-atlas tileset");
    }
    const int32 imageW = image.attribute("width").as_int(0);
    const int32 imageH = image.attribute("height").as_int(0);
    if (imageW <= 0 || imageH <= 0 || imageW > (1 << 20) || imageH > (1 << 20))
        return Fail(*ctx.Result, "Tiled import: tileset '" + name + "' has invalid image bounds");

    const int32 columns = TileSetAsset::GridColumns(imageW, tileW, margin, spacing);
    const int32 rows = TileSetAsset::GridRows(imageH, tileH, margin, spacing);
    if (columns <= 0 || rows <= 0 ||
        static_cast<int64>(columns) * rows > kMaxImportTilesPerSet)
        return Fail(*ctx.Result, "Tiled import: tileset '" + name + "' slices to an unusable tile count");

    ImportedTileSet imported;
    imported.Guid = GUID::Generate();
    imported.Name = SanitizeAssetName(name);
    if (!ReserveImportName(ctx.OutputNames, imported.Name, ".tileset"))
        return Fail(*ctx.Result, "Tiled import: duplicate output filename '" + imported.Name +
                                     ".tileset'; rename the source tilesets to unique names");
    imported.ImagePath = ResolveRelativeTo(sourceFile, image.attribute("source").as_string());
    if (imported.ImagePath.empty())
    {
        return Fail(*ctx.Result, "Tiled import: tileset '" + name +
                                     "' names an atlas image outside its own directory");
    }

    GUID atlasGuid;
    if (ctx.ResolveTexture && *ctx.ResolveTexture)
        atlasGuid = (*ctx.ResolveTexture)(imported.ImagePath);
    if (atlasGuid.IsNull())
        Warn(ctx, "Tiled import: atlas image '" + PathText(imported.ImagePath) +
                      "' did not resolve to a texture; tileset '" + name + "' imports untextured");

    auto asset = std::make_unique<TileSetAsset>(
        imported.Guid, ctx.TmxPath.parent_path() / (imported.Name + ".tileset"));
    if (!ctx.Budget.Tiles(static_cast<uint64>(columns) * rows))
        return Fail(*ctx.Result, "Tiled import: exceeds the whole-import tile budget; split the source map");
    asset->SetAtlasTexture(atlasGuid, imageW, imageH);
    asset->SliceGrid(tileW, tileH, margin, spacing);

    const int32 declaredCount = node.attribute("tilecount").as_int(columns * rows);
    if (declaredCount != columns * rows)
        Warn(ctx, "Tiled import: tileset '" + name + "': declared tilecount " +
                      std::to_string(declaredCount) + " != sliced count " +
                      std::to_string(columns * rows));

    for (const pugi::xml_node& tileNode : node.children("tile"))
    {
        const int64 localId = tileNode.attribute("id").as_llong(-1);
        if (localId < 0 || localId >= static_cast<int64>(columns) * rows)
        {
            Warn(ctx, "Tiled import: tileset '" + name + "': out-of-range tile id " +
                          std::to_string(localId) + "; skipped");
            continue;
        }
        TileSetTile* tile = asset->FindTile(static_cast<uint32>(localId) + 1);
        if (!tile)
            continue;

        if (const pugi::xml_attribute probability = tileNode.attribute("probability"))
        {
            const float32 value = probability.as_float(1.0f);
            if (std::isfinite(value))
                tile->Probability = std::max(0.0f, value);
            else
                Warn(ctx, "Tiled import: non-finite tile probability; kept the default");
        }

        if (const pugi::xml_node animation = tileNode.child("animation"))
        {
            int32 frameCount = 0;
            for (const pugi::xml_node& frame : animation.children("frame"))
            {
                if (++frameCount > kMaxImportAnimationFrames)
                {
                    Warn(ctx, "Tiled import: tileset '" + name +
                                  "': animation exceeds the frame limit; truncated");
                    break;
                }
                const int64 frameId = frame.attribute("tileid").as_llong(-1);
                if (frameId < 0 || frameId >= static_cast<int64>(columns) * rows)
                    continue;
                const float32 duration = frame.attribute("duration").as_float(100.0f) / 1000.0f;
                if (!std::isfinite(duration))
                {
                    Warn(ctx, "Tiled import: non-finite animation duration; skipped");
                    continue;
                }
                tile->AnimationFrames.push_back(TileAnimationFrame{
                    static_cast<uint32>(frameId) + 1, std::max(0.0f, duration)});
            }
        }

        ParseTileCollision(ctx, tileNode, *tile, static_cast<float32>(tileW),
                           static_cast<float32>(tileH), name);
        ParseTileProperties(ctx, tileNode, *tile);
    }

    TiledTileSetRef ref;
    ref.FirstGid = firstGid;
    ref.TileCount = columns * rows;
    ref.Name = imported.Name;
    ref.ResultIndex = ctx.Result->TileSets.size();
    imported.Asset = std::move(asset);
    ctx.Result->TileSets.push_back(std::move(imported));
    ctx.TileSets.push_back(ref);
    return true;
}

bool ParseTileSetElement(TiledContext& ctx, const pugi::xml_node& node)
{
    const int64 firstGid = node.attribute("firstgid").as_llong(0);
    if (firstGid <= 0 || firstGid > static_cast<int64>(kTiledGidMask))
        return Fail(*ctx.Result, "Tiled import: tileset has a missing or invalid firstgid");
    if (static_cast<int32>(ctx.Result->TileSets.size()) >= kMaxImportTileSets)
        return Fail(*ctx.Result, "Tiled import: too many tilesets");

    if (const pugi::xml_attribute source = node.attribute("source"))
    {
        const std::filesystem::path tsxPath = ResolveRelativeTo(ctx.TmxPath, source.as_string());
        if (tsxPath.empty())
        {
            return Fail(*ctx.Result,
                        std::string("Tiled import: external tileset '") + source.as_string() +
                            "' resolves outside the map's own directory");
        }
        const SourceFileCharge charge = ctx.Budget.ReadFile(tsxPath);
        if (charge == SourceFileCharge::NotARegularFile)
            return Fail(*ctx.Result, "Tiled import: cannot read external tileset '" + PathText(tsxPath) +
                                         "': missing or not a regular file");
        if (charge == SourceFileCharge::OverBudget)
            return Fail(*ctx.Result, "Tiled import: exceeds the whole-import source byte budget; split the source map");
        pugi::xml_document tsxDoc;
        const pugi::xml_parse_result parsed = tsxDoc.load_file(tsxPath.c_str());
        if (!parsed)
            return Fail(*ctx.Result, "Tiled import: cannot parse external tileset '" +
                                         PathText(tsxPath) + "': " + parsed.description());
        if (!ChargeXmlItems(ctx.Budget, tsxDoc))
            return Fail(*ctx.Result, "Tiled import: exceeds the whole-import item or nested element budget; split the source map");
        const pugi::xml_node root = tsxDoc.child("tileset");
        if (!root)
            return Fail(*ctx.Result,
                        "Tiled import: '" + PathText(tsxPath) + "' has no <tileset> root");
        return ParseTileSetBody(ctx, root, static_cast<uint32>(firstGid), tsxPath);
    }
    return ParseTileSetBody(ctx, node, static_cast<uint32>(firstGid), ctx.TmxPath);
}

// --- layer import -----------------------------------------------------------

struct GidCell
{
    int32 X = 0; // engine cell coordinates
    int32 Y = 0;
    uint32 Gid = 0;
};

/** Tileset covering `gid`, or npos. TileSets are sorted by FirstGid. */
size_t FindTileSetForGid(const TiledContext& ctx, uint32 gid)
{
    size_t found = static_cast<size_t>(-1);
    for (size_t i = 0; i < ctx.TileSets.size(); ++i)
    {
        if (ctx.TileSets[i].FirstGid <= gid)
            found = i;
        else
            break;
    }
    if (found != static_cast<size_t>(-1))
    {
        const TiledTileSetRef& ref = ctx.TileSets[found];
        if (gid - ref.FirstGid >= static_cast<uint32>(ref.TileCount))
            return static_cast<size_t>(-1);
    }
    return found;
}

bool ImportTileLayer(TiledContext& ctx, const pugi::xml_node& layerNode, TileMapData& data)
{
    if (++ctx.LayerCount > kMaxImportLayers)
        return Fail(*ctx.Result, "Tiled import: too many layers");
    const std::string name = layerNode.attribute("name").as_string("Layer");

    const pugi::xml_node dataNode = layerNode.child("data");
    if (!dataNode)
    {
        Warn(ctx, "Tiled import: layer '" + name + "' has no data; skipped");
        return true;
    }

    // Collect the raw cells first: a layer may draw from several tilesets, and
    // the split into per-tileset sublayers needs the whole population known.
    std::vector<GidCell> cells;
    std::vector<uint32> gids;
    std::string error;
    bool warnedHexRotate = false;

    const auto appendCells = [&](const std::vector<uint32>& decoded, int32 originX, int32 originY,
                                 int32 width, int32 height, bool flipRows)
    {
        for (int32 row = 0; row < height; ++row)
        {
            for (int32 col = 0; col < width; ++col)
            {
                const uint32 gid = decoded[static_cast<size_t>(row) * width + col];
                if (gid == 0)
                    continue;
                if ((gid & kTiledRotateHex120) != 0 && !warnedHexRotate)
                {
                    warnedHexRotate = true;
                    Warn(ctx, "Tiled import: layer '" + name +
                                  "': hexagonal-rotation flag ignored");
                }
                GidCell cell;
                cell.X = originX + col;
                // Finite maps flip rows so the bottom row lands at Y = 0;
                // infinite maps negate around the origin row instead.
                cell.Y = flipRows ? (originY + (height - 1) - row) : -(originY + row);
                cell.Gid = gid;
                cells.push_back(cell);
            }
        }
    };

    if (ctx.Infinite)
    {
        // Header pass first: every chunk's bounds and origin are validated and
        // charged against one per-layer cell budget before a single payload is
        // decoded, so a file whose chunks sum past the ceiling costs nothing.
        struct ChunkHeader
        {
            pugi::xml_node Node;
            int32 X = 0;
            int32 Y = 0;
            int32 Width = 0;
            int32 Height = 0;
        };
        std::vector<ChunkHeader> headers;
        int64 budget = 0;
        for (const pugi::xml_node& chunk : dataNode.children("chunk"))
        {
            if (static_cast<int32>(headers.size()) >= kMaxImportChunksPerLayer)
                return Fail(*ctx.Result, "Tiled import: layer '" + name + "' has too many chunks");
            ChunkHeader header;
            header.Node = chunk;
            header.Width = chunk.attribute("width").as_int(0);
            header.Height = chunk.attribute("height").as_int(0);
            if (header.Width <= 0 || header.Height <= 0 ||
                header.Width > kMaxImportCellsPerAxis || header.Height > kMaxImportCellsPerAxis)
                return Fail(*ctx.Result,
                            "Tiled import: layer '" + name + "' has a chunk with invalid bounds");

            const int64 chunkX = chunk.attribute("x").as_llong(0);
            const int64 chunkY = chunk.attribute("y").as_llong(0);
            if (chunkX < -kMaxImportCellCoordinate || chunkX > kMaxImportCellCoordinate ||
                chunkY < -kMaxImportCellCoordinate || chunkY > kMaxImportCellCoordinate)
                return Fail(*ctx.Result, "Tiled import: layer '" + name +
                                             "' has a chunk origin outside the cell range");
            header.X = static_cast<int32>(chunkX);
            header.Y = static_cast<int32>(chunkY);

            budget += static_cast<int64>(header.Width) * header.Height;
            if (budget > kMaxImportCellsPerLayer)
                return Fail(*ctx.Result, "Tiled import: layer '" + name +
                                             "' exceeds the per-layer cell budget");
            headers.push_back(header);
        }

        if (!ctx.Budget.Cells(static_cast<uint64>(budget)))
            return Fail(*ctx.Result, "Tiled import: exceeds the whole-import cell budget; split the source map");
        for (const ChunkHeader& header : headers)
        {
            if (!DecodeGidPayload(dataNode, header.Node.text().get(),
                                  static_cast<size_t>(header.Width) * header.Height, gids, error))
                return Fail(*ctx.Result, "Tiled import: layer '" + name + "': " + error);
            appendCells(gids, header.X, header.Y, header.Width, header.Height, /*flipRows*/ false);
        }
    }
    else
    {
        const int32 width = layerNode.attribute("width").as_int(ctx.MapWidth);
        const int32 height = layerNode.attribute("height").as_int(ctx.MapHeight);
        if (width <= 0 || height <= 0 || width > kMaxImportCellsPerAxis ||
            height > kMaxImportCellsPerAxis ||
            static_cast<int64>(width) * height > kMaxImportCellsPerLayer)
            return Fail(*ctx.Result, "Tiled import: layer '" + name + "' has invalid bounds");
        if (!ctx.Budget.Cells(static_cast<uint64>(width) * height))
            return Fail(*ctx.Result, "Tiled import: exceeds the whole-import cell budget; split the source map");
        if (!DecodeGidPayload(dataNode, dataNode.text().get(),
                              static_cast<size_t>(width) * height, gids, error))
            return Fail(*ctx.Result, "Tiled import: layer '" + name + "': " + error);
        appendCells(gids, 0, 0, width, height, /*flipRows*/ true);
    }

    // Bucket by owning tileset.
    std::unordered_map<size_t, std::vector<GidCell>> buckets;
    int32 unresolved = 0;
    for (const GidCell& cell : cells)
    {
        const size_t tileSetIndex = FindTileSetForGid(ctx, cell.Gid & kTiledGidMask);
        if (tileSetIndex == static_cast<size_t>(-1))
        {
            ++unresolved;
            continue;
        }
        buckets[tileSetIndex].push_back(cell);
    }
    if (unresolved > 0)
        Warn(ctx, "Tiled import: layer '" + name + "': skipped " + std::to_string(unresolved) +
                      " cells whose GID matches no tileset");

    // One engine layer per tileset the source layer draws from.
    std::vector<size_t> order;
    order.reserve(buckets.size());
    for (const auto& [tileSetIndex, bucket] : buckets)
        order.push_back(tileSetIndex);
    std::sort(order.begin(), order.end());

    for (const size_t tileSetIndex : order)
    {
        const TiledTileSetRef& ref = ctx.TileSets[tileSetIndex];
        const std::string layerName =
            order.size() == 1 ? name : name + " (" + ref.Name + ")";
        const uint32 layerId = data.AddLayer(layerName, TileMapLayerType::Tiles);
        TileMapLayerData& layer = *data.FindLayer(layerId);
        layer.TileSetGuid = ctx.Result->TileSets[ref.ResultIndex].Guid;
        for (const GidCell& cell : buckets[tileSetIndex])
        {
            const uint32 tileId = (cell.Gid & kTiledGidMask) - ref.FirstGid + 1;
            // Tiled's H/V/diagonal semantics are this engine's H/V/Transpose
            // (same dihedral composition order); only the bit positions differ,
            // which MakeCell re-packs.
            if (!layer.FindChunk(ChunkOfCell(cell.X, cell.Y)) && !ctx.Budget.Chunk())
                return Fail(*ctx.Result, "Tiled import: exceeds the whole-import chunk budget; split the source map");
            layer.SetCell(cell.X, cell.Y,
                          MakeCell(tileId, (cell.Gid & kTiledFlipH) != 0,
                                   (cell.Gid & kTiledFlipV) != 0,
                                   (cell.Gid & kTiledFlipD) != 0));
        }
    }
    if (order.size() > 1)
        Warn(ctx, "Tiled import: layer '" + name + "' draws from " +
                      std::to_string(order.size()) +
                      " tilesets and was split into one sublayer per tileset");
    return true;
}

/**
 * Walks one <map> or <group> body. `depth` bounds the recursion: <group>
 * nesting is unbounded in the file format but costs a C++ frame per level here.
 */
bool ImportLayerContainer(TiledContext& ctx, const pugi::xml_node& parent, TileMapData& data,
                          int32 depth)
{
    if (depth > kMaxImportLayerDepth)
        return Fail(*ctx.Result, "Tiled import: layer groups are nested deeper than the limit");

    for (const pugi::xml_node& child : parent.children())
    {
        const std::string_view element = child.name();
        if (element == "layer")
        {
            if (!ImportTileLayer(ctx, child, data))
                return false;
        }
        else if (element == "group")
        {
            if (!ImportLayerContainer(ctx, child, data, depth + 1))
                return false;
        }
        else if (element == "objectgroup" || element == "imagelayer")
        {
            Warn(ctx, std::string("Tiled import: ") + child.name() + " '" +
                          child.attribute("name").as_string() + "' has no tilemap form; skipped");
        }
    }
    return true;
}

} // namespace

TileMapImportResult ImportTiledMap(const std::filesystem::path& tmxPath,
                                   const TextureGuidResolver& resolveTexture)
{
    TileMapImportResult result;
    TiledContext ctx;
    const SourceFileCharge charge = ctx.Budget.ReadFile(tmxPath);
    if (charge == SourceFileCharge::NotARegularFile)
    {
        Fail(result, "Tiled import: cannot read '" + PathText(tmxPath) + "': missing or not a regular file");
        return result;
    }
    if (charge == SourceFileCharge::OverBudget)
    {
        Fail(result, "Tiled import: exceeds the whole-import source byte budget; split the source map");
        return result;
    }
    pugi::xml_document doc;
    const pugi::xml_parse_result parsed = doc.load_file(tmxPath.c_str());
    if (!parsed)
    {
        Fail(result, "Tiled import: cannot parse '" + PathText(tmxPath) +
                         "': " + parsed.description());
        return result;
    }
    if (!ChargeXmlItems(ctx.Budget, doc))
    {
        Fail(result, "Tiled import: exceeds the whole-import item or nested element budget; split the source map");
        return result;
    }
    const pugi::xml_node map = doc.child("map");
    if (!map)
    {
        Fail(result, "Tiled import: '" + PathText(tmxPath) + "' has no <map> root");
        return result;
    }
    const std::string orientation = map.attribute("orientation").as_string();
    if (orientation != "orthogonal")
    {
        Fail(result, "Tiled import: orientation '" + orientation +
                         "' is not supported (orthogonal only)");
        return result;
    }

    ctx.TmxPath = tmxPath;
    ctx.ResolveTexture = &resolveTexture;
    ctx.Result = &result;
    ctx.Infinite = map.attribute("infinite").as_int(0) != 0;
    ctx.MapWidth = map.attribute("width").as_int(0);
    ctx.MapHeight = map.attribute("height").as_int(0);
    const int32 tileW = map.attribute("tilewidth").as_int(0);
    const int32 tileH = map.attribute("tileheight").as_int(0);
    if (tileW <= 0 || tileH <= 0)
    {
        Fail(result, "Tiled import: invalid map tile size");
        return result;
    }
    if (!ctx.Infinite && (ctx.MapWidth <= 0 || ctx.MapHeight <= 0 ||
                          ctx.MapWidth > kMaxImportCellsPerAxis ||
                          ctx.MapHeight > kMaxImportCellsPerAxis))
    {
        Fail(result, "Tiled import: invalid map bounds");
        return result;
    }

    for (const pugi::xml_node& tileSetNode : map.children("tileset"))
    {
        if (!ParseTileSetElement(ctx, tileSetNode))
            return result;
    }
    std::sort(ctx.TileSets.begin(), ctx.TileSets.end(),
              [](const TiledTileSetRef& a, const TiledTileSetRef& b)
              { return a.FirstGid < b.FirstGid; });

    ImportedTileMap imported;
    imported.Name = SanitizeAssetName(PathText(tmxPath.stem()));
    auto asset = std::make_unique<TileMapAsset>(
        GUID::Generate(), tmxPath.parent_path() / (imported.Name + ".tilemap"));
    // Cell width is one world unit; a non-square tile grid keeps its aspect.
    asset->Data().CellWidth = 1.0f;
    asset->Data().CellHeight = static_cast<float32>(tileH) / static_cast<float32>(tileW);

    if (!ImportLayerContainer(ctx, map, asset->Data(), /*depth*/ 0))
        return result;

    imported.Asset = std::move(asset);
    result.Maps.push_back(std::move(imported));
    result.Success = true;
    return result;
}

} // namespace Importers
} // namespace TileMap
} // namespace GameEngine
