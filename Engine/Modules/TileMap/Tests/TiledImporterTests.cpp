#include "TileMap/Importers/TiledImporter.h"

#include "TestPaths.h"
#include "TestTempDir.h"
#include "TileMap/TileCellEncoding.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <utility>

using namespace GameEngine;
using namespace GameEngine::TileMap;
using namespace GameEngine::TileMap::Importers;
using GameEngine::TileMapTests::FixtureDir;

namespace
{

constexpr const char* kAtlasGuidText = "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee";

TextureGuidResolver FixedResolver()
{
    return [](const std::filesystem::path&) { return GUID(kAtlasGuidText); };
}

std::filesystem::path WriteTemp(const std::string& name, const std::string& contents)
{
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / ("TiledImporterTests_" + name);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << contents;
    return path;
}

std::string ReadAll(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    std::stringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

/** A 2x1 orthogonal map around one CSV payload, for hostile-input variants. */
std::string TinyMap(const std::string& mapAttrs, const std::string& dataAttrs,
                    const std::string& payload)
{
    return "<?xml version=\"1.0\"?>\n"
           "<map orientation=\"orthogonal\" width=\"2\" height=\"1\" tilewidth=\"16\" "
           "tileheight=\"16\" " + mapAttrs + ">\n"
           " <tileset firstgid=\"1\" name=\"s\" tilewidth=\"16\" tileheight=\"16\">"
           "<image source=\"t.png\" width=\"64\" height=\"32\"/></tileset>\n"
           " <layer id=\"1\" name=\"L\" width=\"2\" height=\"1\">\n"
           "  <data " + dataAttrs + ">" + payload + "</data>\n"
           " </layer>\n"
           "</map>\n";
}

} // namespace

TEST(TiledImporterTests, AllEightDihedralOrientationsMapVerbatim)
{
    const TileMapImportResult result =
        ImportTiledMap(FixtureDir() / "basic.tmx", FixedResolver());
    ASSERT_TRUE(result.Success) << result.Error;
    ASSERT_EQ(result.Maps.size(), 1u);
    const TileMapData& data = result.Maps[0].Asset->Data();
    ASSERT_EQ(data.Layers().size(), 1u);
    const TileMapLayerData& layer = data.Layers()[0];

    // Tiled GID flag bits are this engine's cell bits; row 0 (top) lands at
    // engine Y = 2. One case per element of the dihedral group.
    EXPECT_EQ(layer.GetCell(0, 2), MakeCell(1));
    EXPECT_EQ(layer.GetCell(1, 2), MakeCell(1, true, false, false));
    EXPECT_EQ(layer.GetCell(2, 2), MakeCell(1, false, true, false));
    EXPECT_EQ(layer.GetCell(3, 2), MakeCell(1, true, true, false));
    EXPECT_EQ(layer.GetCell(0, 1), MakeCell(1, false, false, true));
    EXPECT_EQ(layer.GetCell(1, 1), MakeCell(1, true, false, true));
    EXPECT_EQ(layer.GetCell(2, 1), MakeCell(1, false, true, true));
    EXPECT_EQ(layer.GetCell(3, 1), MakeCell(1, true, true, true));

    EXPECT_EQ(layer.GetCell(0, 0), MakeCell(2));
    EXPECT_EQ(layer.GetCell(1, 0), kEmptyCell);
    EXPECT_EQ(layer.GetCell(3, 0), MakeCell(3));
}

TEST(TiledImporterTests, TilesetAnimationCollisionAndProperties)
{
    const TileMapImportResult result =
        ImportTiledMap(FixtureDir() / "basic.tmx", FixedResolver());
    ASSERT_TRUE(result.Success) << result.Error;
    ASSERT_EQ(result.TileSets.size(), 1u);
    const TileSetAsset& tileSet = *result.TileSets[0].Asset;
    EXPECT_EQ(tileSet.AtlasTexture(), GUID(kAtlasGuidText));
    EXPECT_EQ(tileSet.Columns(), 4);
    EXPECT_EQ(tileSet.Rows(), 2);

    // Tiled local tile 0 = our tile 1: animation frames (ms -> s, local id + 1),
    // rect collision in the +Y-up tile-local frame, bool property.
    const TileSetTile* tile1 = tileSet.FindTile(1);
    ASSERT_NE(tile1, nullptr);
    ASSERT_EQ(tile1->AnimationFrames.size(), 2u);
    EXPECT_EQ(tile1->AnimationFrames[0].TileId, 1u);
    EXPECT_FLOAT_EQ(tile1->AnimationFrames[0].DurationSeconds, 0.25f);
    EXPECT_EQ(tile1->AnimationFrames[1].TileId, 2u);

    ASSERT_EQ(tile1->CollisionPolygons.size(), 1u);
    const auto& rect = tile1->CollisionPolygons[0].Points;
    ASSERT_EQ(rect.size(), 4u);
    EXPECT_FLOAT_EQ(rect[0].x, 0.0f);
    EXPECT_FLOAT_EQ(rect[0].y, 0.5f); // Tiled y=8 of 16, from the top
    EXPECT_FLOAT_EQ(rect[2].x, 1.0f);
    EXPECT_FLOAT_EQ(rect[2].y, 0.0f);

    ASSERT_TRUE(tile1->CustomData.contains("slippery"));
    EXPECT_TRUE(std::get<bool>(tile1->CustomData.at("slippery")));

    // Tiled local tile 2 = our tile 3: probability and a polygon whose points
    // are relative to the object position.
    const TileSetTile* tile3 = tileSet.FindTile(3);
    ASSERT_NE(tile3, nullptr);
    EXPECT_FLOAT_EQ(tile3->Probability, 0.5f);
    ASSERT_EQ(tile3->CollisionPolygons.size(), 1u);
    const auto& triangle = tile3->CollisionPolygons[0].Points;
    ASSERT_EQ(triangle.size(), 3u);
    EXPECT_FLOAT_EQ(triangle[0].x, 0.0f);
    EXPECT_FLOAT_EQ(triangle[0].y, 0.0f);
    EXPECT_FLOAT_EQ(triangle[1].x, 1.0f);
    EXPECT_FLOAT_EQ(triangle[1].y, 0.0f);
    EXPECT_FLOAT_EQ(triangle[2].x, 1.0f);
    EXPECT_FLOAT_EQ(triangle[2].y, 1.0f);
}

TEST(TiledImporterTests, GoldenMapSerialization)
{
    const TileMapImportResult result =
        ImportTiledMap(FixtureDir() / "basic.tmx", FixedResolver());
    ASSERT_TRUE(result.Success) << result.Error;

    TileMapAsset expected(GUID::Generate(), "golden.tilemap");
    TileMapData& data = expected.Data();
    data.CellWidth = 1.0f;
    data.CellHeight = 1.0f;
    const uint32 layerId = data.AddLayer("Ground", TileMapLayerType::Tiles);
    TileMapLayerData& layer = *data.FindLayer(layerId);
    layer.TileSetGuid = result.TileSets[0].Guid;
    layer.SetCell(0, 2, MakeCell(1));
    layer.SetCell(1, 2, MakeCell(1, true, false, false));
    layer.SetCell(2, 2, MakeCell(1, false, true, false));
    layer.SetCell(3, 2, MakeCell(1, true, true, false));
    layer.SetCell(0, 1, MakeCell(1, false, false, true));
    layer.SetCell(1, 1, MakeCell(1, true, false, true));
    layer.SetCell(2, 1, MakeCell(1, false, true, true));
    layer.SetCell(3, 1, MakeCell(1, true, true, true));
    layer.SetCell(0, 0, MakeCell(2));
    layer.SetCell(3, 0, MakeCell(3));

    Vector<uint8> expectedBytes;
    Vector<uint8> importedBytes;
    ASSERT_TRUE(expected.SaveToData(expectedBytes));
    ASSERT_TRUE(result.Maps[0].Asset->SaveToData(importedBytes));
    EXPECT_EQ(std::string(importedBytes.begin(), importedBytes.end()),
              std::string(expectedBytes.begin(), expectedBytes.end()));
}

TEST(TiledImporterTests, MixedTilesetLayerSplitsPerTileset)
{
    const TileMapImportResult result =
        ImportTiledMap(FixtureDir() / "multiset.tmx", FixedResolver());
    ASSERT_TRUE(result.Success) << result.Error;
    ASSERT_EQ(result.TileSets.size(), 2u);
    const TileMapData& data = result.Maps[0].Asset->Data();
    ASSERT_EQ(data.Layers().size(), 2u);

    EXPECT_EQ(data.Layers()[0].Name, "Mixed (setA)");
    EXPECT_EQ(data.Layers()[0].TileSetGuid, result.TileSets[0].Guid);
    EXPECT_EQ(data.Layers()[0].GetCell(0, 0), MakeCell(1));
    EXPECT_EQ(data.Layers()[0].GetCell(1, 0), kEmptyCell);

    EXPECT_EQ(data.Layers()[1].Name, "Mixed (setB)");
    EXPECT_EQ(data.Layers()[1].TileSetGuid, result.TileSets[1].Guid);
    EXPECT_EQ(data.Layers()[1].GetCell(1, 0), MakeCell(2)); // gid 4 - firstgid 3 + 1

    for (const std::string& warning : result.Warnings)
    {
        if (warning.find("split into one sublayer") != std::string::npos)
            return;
    }
    FAIL() << "expected a layer-split warning";
}

TEST(TiledImporterTests, PlainBase64Decodes)
{
    const TileMapImportResult result =
        ImportTiledMap(FixtureDir() / "b64plain.tmx", FixedResolver());
    ASSERT_TRUE(result.Success) << result.Error;
    const TileMapLayerData& layer = result.Maps[0].Asset->Data().Layers()[0];
    EXPECT_EQ(layer.GetCell(0, 0), MakeCell(1));
    EXPECT_EQ(layer.GetCell(1, 0), MakeCell(2));
}

TEST(TiledImporterTests, InfiniteMapZlibChunkDecodes)
{
    const TileMapImportResult result =
        ImportTiledMap(FixtureDir() / "infinite_b64zlib.tmx", FixedResolver());
    ASSERT_TRUE(result.Success) << result.Error;
    const TileMapLayerData& layer = result.Maps[0].Asset->Data().Layers()[0];
    // Chunk origin (-16, 0); infinite maps negate Y around the origin row.
    EXPECT_EQ(layer.GetCell(-16, 0), MakeCell(1));
    EXPECT_EQ(layer.GetCell(-1, -15), MakeCell(2));
}

TEST(TiledImporterTests, ZlibPayloadOfExactlyTheLayerBoundsDecodes)
{
    // zlib of the little-endian GIDs 1, 2: the control for the two hostile payloads below.
    const TileMapImportResult result = ImportTiledMap(
        WriteTemp("zlib_exact.tmx",
                  TinyMap("infinite=\"0\"", "encoding=\"base64\" compression=\"zlib\"", "eNpjZGBgYAJiAAAYAAQ=")),
        FixedResolver());
    ASSERT_TRUE(result.Success) << result.Error;
    const TileMapLayerData& layer = result.Maps[0].Asset->Data().Layers()[0];
    EXPECT_EQ(layer.GetCell(0, 0), MakeCell(1));
    EXPECT_EQ(layer.GetCell(1, 0), MakeCell(2));
}

TEST(TiledImporterTests, GzipPayloadOfExactlyTheLayerBoundsDecodes)
{
    const TileMapImportResult result = ImportTiledMap(
        WriteTemp("gzip_exact.tmx",
                  TinyMap("infinite=\"0\"", "encoding=\"base64\" compression=\"gzip\"",
                          "H4sIAAAAAAACE2NkYGBgAmIAfBeBAwgAAAA=")),
        FixedResolver());
    ASSERT_TRUE(result.Success) << result.Error;
    const TileMapLayerData& layer = result.Maps[0].Asset->Data().Layers()[0];
    EXPECT_EQ(layer.GetCell(0, 0), MakeCell(1));
    EXPECT_EQ(layer.GetCell(1, 0), MakeCell(2));
}

TEST(TiledImporterTests, HostileOverInflatingZlibPayloadFails)
{
    // 64 KiB of zeros against a 2x1 layer (8 bytes): inflation stops at the layer bounds.
    const TileMapImportResult result = ImportTiledMap(
        WriteTemp("zlib_bomb.tmx",
                  TinyMap("infinite=\"0\"", "encoding=\"base64\" compression=\"zlib\"",
                          "eNrtwQEBAAAAgJD+r+4ICgAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
                          "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAGoADwAB")),
        FixedResolver());
    EXPECT_FALSE(result.Success);
    EXPECT_NE(result.Error.find("does not match the layer bounds"), std::string::npos) << result.Error;
    EXPECT_TRUE(result.Maps.empty());
}

TEST(TiledImporterTests, HostileTruncatedZlibPayloadFails)
{
    // The first half of the control payload's stream.
    const TileMapImportResult result = ImportTiledMap(
        WriteTemp("zlib_truncated.tmx",
                  TinyMap("infinite=\"0\"", "encoding=\"base64\" compression=\"zlib\"", "eNpjZGBgYA==")),
        FixedResolver());
    EXPECT_FALSE(result.Success);
    EXPECT_NE(result.Error.find("corrupt"), std::string::npos) << result.Error;
    EXPECT_TRUE(result.Maps.empty());
}

TEST(TiledImporterTests, HostileHugeMapBoundsFail)
{
    const auto path = WriteTemp(
        "huge.tmx", TinyMap("infinite=\"0\"", "encoding=\"csv\"", "1,2"));
    // Patch the bounds to hostile values.
    std::string text = ReadAll(path);
    const size_t at = text.find("width=\"2\"");
    ASSERT_NE(at, std::string::npos);
    text.replace(at, 9, "width=\"999999999\"");
    const auto hostile = WriteTemp("huge2.tmx", text);
    const TileMapImportResult result = ImportTiledMap(hostile, FixedResolver());
    EXPECT_FALSE(result.Success);
    EXPECT_NE(result.Error.find("invalid map bounds"), std::string::npos);
}

TEST(TiledImporterTests, HostileNegativeTileSizeFails)
{
    const auto path = WriteTemp("negsize.tmx",
                                "<?xml version=\"1.0\"?>\n"
                                "<map orientation=\"orthogonal\" width=\"2\" height=\"1\" "
                                "tilewidth=\"-16\" tileheight=\"16\"></map>\n");
    const TileMapImportResult result = ImportTiledMap(path, FixedResolver());
    EXPECT_FALSE(result.Success);
    EXPECT_NE(result.Error.find("tile size"), std::string::npos);
}

TEST(TiledImporterTests, HostileMalformedCsvFails)
{
    const auto path =
        WriteTemp("badcsv.tmx", TinyMap("infinite=\"0\"", "encoding=\"csv\"", "1,banana"));
    const TileMapImportResult result = ImportTiledMap(path, FixedResolver());
    EXPECT_FALSE(result.Success);
    EXPECT_NE(result.Error.find("CSV"), std::string::npos);
}

TEST(TiledImporterTests, HostileCsvCellCountMismatchFails)
{
    const auto path =
        WriteTemp("shortcsv.tmx", TinyMap("infinite=\"0\"", "encoding=\"csv\"", "1"));
    const TileMapImportResult result = ImportTiledMap(path, FixedResolver());
    EXPECT_FALSE(result.Success);
    EXPECT_NE(result.Error.find("fewer cells"), std::string::npos);
}

TEST(TiledImporterTests, HostileZstdCompressionRejected)
{
    const auto path = WriteTemp(
        "zstd.tmx",
        TinyMap("infinite=\"0\"", "encoding=\"base64\" compression=\"zstd\"", "AQAAAAIAAAA="));
    const TileMapImportResult result = ImportTiledMap(path, FixedResolver());
    EXPECT_FALSE(result.Success);
    EXPECT_NE(result.Error.find("zstd"), std::string::npos);
}

TEST(TiledImporterTests, HostileInvalidBase64Fails)
{
    const auto path = WriteTemp(
        "badb64.tmx", TinyMap("infinite=\"0\"", "encoding=\"base64\"", "!!!not-base64!!!"));
    const TileMapImportResult result = ImportTiledMap(path, FixedResolver());
    EXPECT_FALSE(result.Success);
    EXPECT_NE(result.Error.find("base64"), std::string::npos);
}

TEST(TiledImporterTests, HostileTruncatedXmlFails)
{
    const std::string full = ReadAll(FixtureDir() / "basic.tmx");
    ASSERT_GT(full.size(), 100u);
    const auto path = WriteTemp("truncated.tmx", full.substr(0, full.size() / 2));
    const TileMapImportResult result = ImportTiledMap(path, FixedResolver());
    EXPECT_FALSE(result.Success);
    EXPECT_NE(result.Error.find("cannot parse"), std::string::npos);
}

TEST(TiledImporterTests, GidOutsideEveryTilesetSkippedWithWarning)
{
    const auto path =
        WriteTemp("badgid.tmx", TinyMap("infinite=\"0\"", "encoding=\"csv\"", "1,999"));
    const TileMapImportResult result = ImportTiledMap(path, FixedResolver());
    ASSERT_TRUE(result.Success) << result.Error;
    const TileMapLayerData& layer = result.Maps[0].Asset->Data().Layers()[0];
    EXPECT_EQ(layer.GetCell(0, 0), MakeCell(1));
    EXPECT_EQ(layer.GetCell(1, 0), kEmptyCell);
    for (const std::string& warning : result.Warnings)
    {
        if (warning.find("matches no tileset") != std::string::npos)
            return;
    }
    FAIL() << "expected an unresolved-GID warning";
}

TEST(TiledImporterTests, ImageCollectionTilesetRejected)
{
    const auto path = WriteTemp(
        "collection.tmx",
        "<?xml version=\"1.0\"?>\n"
        "<map orientation=\"orthogonal\" width=\"1\" height=\"1\" tilewidth=\"16\" "
        "tileheight=\"16\">\n"
        " <tileset firstgid=\"1\" name=\"c\" tilewidth=\"16\" tileheight=\"16\">\n"
        "  <tile id=\"0\"><image source=\"a.png\" width=\"16\" height=\"16\"/></tile>\n"
        " </tileset>\n"
        " <layer id=\"1\" name=\"L\" width=\"1\" height=\"1\"><data encoding=\"csv\">1</data></layer>\n"
        "</map>\n");
    const TileMapImportResult result = ImportTiledMap(path, FixedResolver());
    EXPECT_FALSE(result.Success);
    EXPECT_NE(result.Error.find("image-collection"), std::string::npos);
}

TEST(TiledImporterTests, NonOrthogonalOrientationRejected)
{
    const auto path = WriteTemp("iso.tmx",
                                "<?xml version=\"1.0\"?>\n"
                                "<map orientation=\"isometric\" width=\"1\" height=\"1\" "
                                "tilewidth=\"16\" tileheight=\"8\"></map>\n");
    const TileMapImportResult result = ImportTiledMap(path, FixedResolver());
    EXPECT_FALSE(result.Success);
    EXPECT_NE(result.Error.find("orthogonal"), std::string::npos);
}

TEST(TiledImporterTests, DeeplyNestedGroupsAreRejectedNotRecursedInto)
{
    // <group> nesting drives one C++ recursion level each; without a depth
    // ceiling this exhausts the stack instead of failing.
    constexpr int32 kDepth = 100000;
    std::string open;
    std::string close;
    open.reserve(static_cast<size_t>(kDepth) * 7);
    close.reserve(static_cast<size_t>(kDepth) * 8);
    for (int32 i = 0; i < kDepth; ++i)
    {
        open += "<group>";
        close += "</group>";
    }
    const auto path = WriteTemp("nested_groups.tmx",
                                "<?xml version=\"1.0\"?>\n"
                                "<map orientation=\"orthogonal\" width=\"1\" height=\"1\" "
                                "tilewidth=\"16\" tileheight=\"16\">\n" +
                                    open + close + "</map>\n");
    const TileMapImportResult result = ImportTiledMap(path, FixedResolver());
    EXPECT_FALSE(result.Success);
    EXPECT_NE(result.Error.find("nested"), std::string::npos) << result.Error;
}

namespace
{

/** An infinite-map layer around a caller-supplied run of <chunk> elements. */
std::string InfiniteMapWithChunks(const std::string& chunks)
{
    return "<?xml version=\"1.0\"?>\n"
           "<map orientation=\"orthogonal\" infinite=\"1\" tilewidth=\"16\" tileheight=\"16\">\n"
           " <tileset firstgid=\"1\" name=\"s\" tilewidth=\"16\" tileheight=\"16\">"
           "<image source=\"t.png\" width=\"64\" height=\"32\"/></tileset>\n"
           " <layer id=\"1\" name=\"L\">\n"
           "  <data encoding=\"csv\">" + chunks + "</data>\n"
           " </layer>\n"
           "</map>\n";
}

} // namespace

TEST(TiledImporterTests, InfiniteLayerChunksShareOnePerLayerCellBudget)
{
    // Each chunk is individually inside the per-chunk ceiling; their sum is
    // four times the per-layer ceiling, which is the amplification a small
    // compressed .tmx turns into gigabytes of cells.
    constexpr int32 kChunkDim = 1024;
    std::string csv;
    csv.reserve(static_cast<size_t>(kChunkDim) * kChunkDim * 2);
    for (int64 i = 0; i < static_cast<int64>(kChunkDim) * kChunkDim; ++i)
        csv += (i == 0) ? "1" : ",1";

    std::string chunks;
    for (int32 i = 0; i < 5; ++i)
    {
        chunks += "<chunk x=\"" + std::to_string(i * kChunkDim) +
                  "\" y=\"0\" width=\"1024\" height=\"1024\">" + csv + "</chunk>";
    }

    const auto path = WriteTemp("chunk_bomb.tmx", InfiniteMapWithChunks(chunks));
    const TileMapImportResult result = ImportTiledMap(path, FixedResolver());
    EXPECT_FALSE(result.Success);
    EXPECT_NE(result.Error.find("cell budget"), std::string::npos) << result.Error;
}

TEST(TiledImporterTests, ChunkOriginsOutsideTheCellCoordinateRangeAreRejected)
{
    // origin + dimension must not overflow the int32 cell coordinate.
    for (const char* origin : {"2147483647", "-2147483648"})
    {
        const std::string chunk = std::string("<chunk x=\"") + origin +
                                  "\" y=\"0\" width=\"2\" height=\"2\">1,1,1,1</chunk>";
        const auto path = WriteTemp("chunk_origin.tmx", InfiniteMapWithChunks(chunk));
        const TileMapImportResult result = ImportTiledMap(path, FixedResolver());
        EXPECT_FALSE(result.Success) << origin;
        EXPECT_NE(result.Error.find("chunk origin"), std::string::npos) << result.Error;
    }
}

TEST(TiledImporterTests, ExternalTileSetOutsideTheSourceDirectoryIsRejected)
{
    const auto path = WriteTemp(
        "escape_tsx.tmx",
        "<?xml version=\"1.0\"?>\n"
        "<map orientation=\"orthogonal\" width=\"1\" height=\"1\" tilewidth=\"16\" "
        "tileheight=\"16\">\n"
        " <tileset firstgid=\"1\" source=\"../../../../../../etc/hosts\"/>\n"
        " <layer id=\"1\" name=\"L\" width=\"1\" height=\"1\">"
        "<data encoding=\"csv\">1</data></layer>\n"
        "</map>\n");
    const TileMapImportResult result = ImportTiledMap(path, FixedResolver());
    EXPECT_FALSE(result.Success);
    EXPECT_NE(result.Error.find("outside"), std::string::npos) << result.Error;
}

TEST(TiledImporterTests, AbsoluteAtlasImagePathIsRejected)
{
    const auto path = WriteTemp(
        "abs_image.tmx",
        "<?xml version=\"1.0\"?>\n"
        "<map orientation=\"orthogonal\" width=\"1\" height=\"1\" tilewidth=\"16\" "
        "tileheight=\"16\">\n"
        " <tileset firstgid=\"1\" name=\"s\" tilewidth=\"16\" tileheight=\"16\">"
        "<image source=\"/etc/hosts\" width=\"64\" height=\"32\"/></tileset>\n"
        " <layer id=\"1\" name=\"L\" width=\"1\" height=\"1\">"
        "<data encoding=\"csv\">1</data></layer>\n"
        "</map>\n");
    const TileMapImportResult result = ImportTiledMap(path, FixedResolver());
    EXPECT_FALSE(result.Success);
    EXPECT_NE(result.Error.find("outside"), std::string::npos) << result.Error;
}

TEST(TiledImporterTests, SanitizedAndCaseOnlyTileSetNamesCannotOverwriteEachOther)
{
    for (const auto& names : {std::pair{"A/B", "A:B"}, std::pair{"Terrain", "terrain"}})
    {
        const std::string body = "<map orientation=\"orthogonal\" width=\"1\" height=\"1\" tilewidth=\"16\" tileheight=\"16\">"
            "<tileset firstgid=\"1\" name=\"" + std::string(names.first) + "\" tilewidth=\"16\" tileheight=\"16\">"
            "<image source=\"a.png\" width=\"16\" height=\"16\"/></tileset>"
            "<tileset firstgid=\"2\" name=\"" + names.second + "\" tilewidth=\"16\" tileheight=\"16\">"
            "<image source=\"b.png\" width=\"16\" height=\"16\"/></tileset></map>";
        int calls = 0;
        const auto result = ImportTiledMap(WriteTemp("output_collision.tmx", body),
            [&calls](const std::filesystem::path&) { ++calls; return GUID(kAtlasGuidText); });
        EXPECT_FALSE(result.Success);
        EXPECT_NE(result.Error.find("duplicate output filename"), std::string::npos);
        EXPECT_EQ(calls, 1) << "the colliding second output must be refused before resolving its atlas";
        EXPECT_TRUE(result.TileSets.empty());
        EXPECT_TRUE(result.Maps.empty());
    }
}

TEST(TiledImporterTests, SymlinkOutsideSourceCannotNameAnExternalTileSetOrAtlas)
{
    TestUtils::ScopedTempDir temp(TestUtils::MakeUniqueTempDirectory("tiled_symlink"));
    const auto source = temp.Path() / "source";
    const auto outside = temp.Path() / "outside";
    std::filesystem::create_directories(source);
    std::filesystem::create_directories(outside);
    std::error_code error;
    std::filesystem::create_directory_symlink(outside, source / "link", error);
    if (error)
        GTEST_SKIP() << "Creating a directory symlink is unavailable: " << error.message();
    std::ofstream(outside / "set.tsx") <<
        R"(<tileset name="Outside" tilewidth="16" tileheight="16"><image source="outside.png" width="16" height="16"/></tileset>)";
    const std::string prefix = R"(<map orientation="orthogonal" width="1" height="1" tilewidth="16" tileheight="16">)";
    for (const char* set : {
        R"(<tileset firstgid="1" source="link/set.tsx"/>)",
        R"(<tileset firstgid="1" name="Atlas" tilewidth="16" tileheight="16"><image source="link/missing.png" width="16" height="16"/></tileset>)"})
    {
        const auto file = source / "source.tmx";
        std::ofstream(file) << prefix << set << "</map>";
        int calls = 0;
        const auto result = ImportTiledMap(file, [&calls](const std::filesystem::path&) {
            ++calls;
            return GUID(kAtlasGuidText);
        });
        EXPECT_FALSE(result.Success);
        EXPECT_NE(result.Error.find("outside"), std::string::npos);
        EXPECT_EQ(calls, 0);
        EXPECT_TRUE(result.TileSets.empty());
        EXPECT_TRUE(result.Maps.empty());
    }
}

TEST(TiledImporterTests, NonFiniteCollisionAndAnimationDataNeverReachAssets)
{
    const auto path = WriteTemp("nonfinite.tmx", R"(
      <map orientation="orthogonal" width="1" height="1" tilewidth="16" tileheight="16">
        <tileset firstgid="1" name="Finite" tilewidth="16" tileheight="16">
          <image source="atlas.png" width="16" height="16"/>
          <tile id="0" probability="1e999"><animation><frame tileid="0" duration="1e999"/></animation>
            <objectgroup>
              <object x="nan" y="0" width="16" height="16"/>
              <object x="0" y="0"><polygon points="0,0 1e999,0 0,1"/></object>
              <object x="0" y="0" width="16" height="16"/>
            </objectgroup>
            <properties><property name="bad" type="float" value="1e999"/></properties>
          </tile>
        </tileset>
      </map>)");
    const auto result = ImportTiledMap(path, FixedResolver());
    ASSERT_TRUE(result.Success) << result.Error;
    ASSERT_EQ(result.TileSets.size(), 1u);
    const auto* tile = result.TileSets[0].Asset->FindTile(1);
    ASSERT_NE(tile, nullptr);
    EXPECT_EQ(tile->CollisionPolygons.size(), 1u);
    EXPECT_TRUE(tile->AnimationFrames.empty());
    EXPECT_EQ(tile->Probability, 1.0f);
    EXPECT_FALSE(tile->CustomData.contains("bad"));
    EXPECT_FALSE(result.Warnings.empty());
}

TEST(TiledImporterTests, SparseCellsShareOneOutputChunkBudget)
{
    std::string chunks;
    for (uint64 i = 0; i <= kMaxImportOutputChunks; ++i)
        chunks += "<chunk x=\"" + std::to_string(i * kChunkSize) +
            "\" y=\"0\" width=\"1\" height=\"1\">1</chunk>";
    const auto result = ImportTiledMap(WriteTemp("sparse_chunk_budget.tmx", InfiniteMapWithChunks(chunks)), FixedResolver());
    EXPECT_FALSE(result.Success);
    EXPECT_NE(result.Error.find("whole-import chunk budget"), std::string::npos);
    EXPECT_TRUE(result.TileSets.empty());
    EXPECT_TRUE(result.Maps.empty());
}

TEST(TiledImporterTests, SourceThatIsNotARegularFileIsRefusedBeforeReading)
{
    TestUtils::ScopedTempDir temp(TestUtils::MakeUniqueTempDirectory("tiled_not_regular"));
    const auto directory = temp.Path() / "folder.tmx";
    std::filesystem::create_directories(directory);
    const auto result = ImportTiledMap(directory, FixedResolver());
    EXPECT_FALSE(result.Success);
    EXPECT_NE(result.Error.find("not a regular file"), std::string::npos) << result.Error;

    std::filesystem::create_directories(temp.Path() / "set.tsx");
    const auto file = temp.Path() / "source.tmx";
    std::ofstream(file) << R"(<map orientation="orthogonal" width="1" height="1" tilewidth="16" tileheight="16">)"
                        << R"(<tileset firstgid="1" source="set.tsx"/></map>)";
    const auto external = ImportTiledMap(file, FixedResolver());
    EXPECT_FALSE(external.Success);
    EXPECT_NE(external.Error.find("not a regular file"), std::string::npos) << external.Error;
}

TEST(TiledImporterTests, MapUnderANonAsciiFolderImports)
{
    // Characters outside any single Windows code page, in the folder and in the file name.
    TestUtils::ScopedTempDir temp(TestUtils::MakeUniqueTempDirectory("tiled_non_ascii"));
    const auto folder = temp.Path() / std::filesystem::path(u8"Karten\u00e4\u65e5\u672c");
    std::filesystem::create_directories(folder);
    const auto map = folder / std::filesystem::path(u8"K\u00e4rta.tmx");
    std::filesystem::copy_file(FixtureDir() / "basic.tmx", map);
    std::filesystem::copy_file(FixtureDir() / "terrain.tsx", folder / "terrain.tsx");
    const auto result = ImportTiledMap(map, FixedResolver());
    ASSERT_TRUE(result.Success) << result.Error;
    ASSERT_EQ(result.Maps.size(), 1u);
    EXPECT_EQ(result.Maps[0].Asset->GetPath().parent_path(), folder);
    ASSERT_EQ(result.TileSets.size(), 1u);

    // A refusal names the file in UTF-8 instead of throwing.
    std::ofstream(map, std::ios::trunc) << "<map";
    const auto broken = ImportTiledMap(map, FixedResolver());
    EXPECT_FALSE(broken.Success);
    EXPECT_NE(broken.Error.find("\xE6\x97\xA5\xE6\x9C\xAC"), std::string::npos) << broken.Error;
}

TEST(TiledImporterTests, SourceByteBudgetIsCheckedBeforeParsing)
{
    TestUtils::ScopedTempDir temp(TestUtils::MakeUniqueTempDirectory("tiled_source_budget"));
    const auto path = temp.Path() / "large.tmx";
    std::ofstream(path) << "<map/>";
    std::filesystem::resize_file(path, kMaxImportSourceBytes + 1);
    const auto result = ImportTiledMap(path, FixedResolver());
    EXPECT_FALSE(result.Success);
    EXPECT_NE(result.Error.find("whole-import source byte budget"), std::string::npos);
    EXPECT_TRUE(result.TileSets.empty());
    EXPECT_TRUE(result.Maps.empty());
}
