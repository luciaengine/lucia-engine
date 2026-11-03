#include "TileMap/TileMapSourceImport.h"

#include "TestTempDir.h"
#include "TileMap/Importers/TiledImporter.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

using namespace GameEngine;
using namespace GameEngine::Editor;
using namespace GameEngine::TileMap::Importers;

namespace
{

constexpr const char* kExistingBytes = "painted after the first import";

std::string ReadWrittenBytes(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    std::stringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

/** Imports a one-cell map, `source.tmx` with inline tileset `s`, written into `folder`. */
TileMapImportResult ImportOneCellMap(const std::filesystem::path& folder)
{
    const auto source = folder / "source.tmx";
    std::ofstream(source) << R"(<map orientation="orthogonal" width="1" height="1" tilewidth="16" tileheight="16">)"
                          << R"(<tileset firstgid="1" name="s" tilewidth="16" tileheight="16">)"
                          << R"(<image source="t.png" width="16" height="16"/></tileset>)"
                          << R"(<layer id="1" name="L" width="1" height="1"><data encoding="csv">1</data></layer>)"
                          << "</map>";
    return ImportTiledMap(source, [](const std::filesystem::path&) { return GUID::Generate(); });
}

TileMapImportRegistrar CountingRegistrar(int& calls)
{
    return [&calls](const std::filesystem::path&) {
        ++calls;
        return GUID::Generate();
    };
}

} // namespace

TEST(TileMapSourceImportTests, RefusedImportLeavesExistingOutputByteIdentical)
{
    TestUtils::ScopedTempDir temp(TestUtils::MakeUniqueTempDirectory("tilemap_import_refused"));
    TileMapImportResult imported = ImportOneCellMap(temp.Path());
    ASSERT_TRUE(imported.Success) << imported.Error;
    const auto existingMap = temp.Path() / "source.tilemap";
    std::ofstream(existingMap, std::ios::binary) << kExistingBytes;

    int calls = 0;
    std::string error;
    TileMapImportSummary summary;
    EXPECT_FALSE(WriteTileMapImport(imported, /*overwrite*/ false, CountingRegistrar(calls), error, &summary));
    EXPECT_NE(error.find("source.tilemap"), std::string::npos) << error;
    ASSERT_EQ(summary.ExistingOutputs.size(), 1u);
    EXPECT_EQ(summary.ExistingOutputs[0], existingMap);
    EXPECT_EQ(ReadWrittenBytes(existingMap), kExistingBytes);
    EXPECT_FALSE(std::filesystem::exists(temp.Path() / "s.tileset")) << "a refused import writes nothing";
    EXPECT_EQ(calls, 0);
}

TEST(TileMapSourceImportTests, ConfirmedOverwriteReplacesTheExistingOutput)
{
    TestUtils::ScopedTempDir temp(TestUtils::MakeUniqueTempDirectory("tilemap_import_overwrite"));
    TileMapImportResult imported = ImportOneCellMap(temp.Path());
    ASSERT_TRUE(imported.Success) << imported.Error;
    const auto existingMap = temp.Path() / "source.tilemap";
    std::ofstream(existingMap, std::ios::binary) << kExistingBytes;

    int calls = 0;
    std::string error;
    TileMapImportSummary summary;
    ASSERT_TRUE(WriteTileMapImport(imported, /*overwrite*/ true, CountingRegistrar(calls), error, &summary)) << error;
    EXPECT_NE(ReadWrittenBytes(existingMap), kExistingBytes);
    EXPECT_TRUE(std::filesystem::exists(temp.Path() / "s.tileset"));
    EXPECT_EQ(calls, 2);
    EXPECT_TRUE(summary.ExistingOutputs.empty());
}

TEST(TileMapSourceImportTests, RefusesDanglingOutputSymlinkBeforeWritingOtherOutputs)
{
    TestUtils::ScopedTempDir temp(TestUtils::MakeUniqueTempDirectory("tilemap_import_symlink"));
    auto imported = ImportOneCellMap(temp.Path());
    ASSERT_TRUE(imported.Success) << imported.Error;
    const auto map = temp.Path() / "source.tilemap";
    std::error_code ec;
    std::filesystem::create_symlink(temp.Path() / "missing.tilemap", map, ec);
    if (ec)
        GTEST_SKIP() << "Cannot create symlink: " << ec.message();
    int calls = 0;
    std::string error;
    TileMapImportSummary summary;
    EXPECT_FALSE(WriteTileMapImport(imported, false, CountingRegistrar(calls), error, &summary));
    EXPECT_EQ(summary.ExistingOutputs.size(), 1u);
    EXPECT_TRUE(std::filesystem::is_symlink(map));
    EXPECT_FALSE(std::filesystem::exists(temp.Path() / "s.tileset"));
    EXPECT_EQ(calls, 0);
}

TEST(TileMapSourceImportTests, ReusedSummaryDoesNotRetainRefusedOutputsAfterConfirmation)
{
    TestUtils::ScopedTempDir temp(TestUtils::MakeUniqueTempDirectory("tilemap_import_retry"));
    auto imported = ImportOneCellMap(temp.Path());
    ASSERT_TRUE(imported.Success) << imported.Error;
    std::ofstream(temp.Path() / "source.tilemap") << kExistingBytes;
    int calls = 0;
    std::string error;
    TileMapImportSummary summary;
    ASSERT_FALSE(WriteTileMapImport(imported, false, CountingRegistrar(calls), error, &summary));
    ASSERT_FALSE(summary.ExistingOutputs.empty());
    ASSERT_TRUE(WriteTileMapImport(imported, true, CountingRegistrar(calls), error, &summary)) << error;
    EXPECT_TRUE(summary.ExistingOutputs.empty());
    EXPECT_TRUE(error.empty());
    EXPECT_EQ(summary.TileSetPaths.size(), 1u);
    EXPECT_EQ(summary.MapPaths.size(), 1u);
}

TEST(TileMapSourceImportTests, SummaryReportsPublishedOutputWhenRegistrationFails)
{
    TestUtils::ScopedTempDir temp(TestUtils::MakeUniqueTempDirectory("tilemap_import_partial"));
    auto imported = ImportOneCellMap(temp.Path());
    ASSERT_TRUE(imported.Success) << imported.Error;
    TileMapImportSummary summary;
    std::string error;
    ASSERT_FALSE(WriteTileMapImport(imported, false, [](const auto&) { return GUID(); }, error, &summary));
    ASSERT_EQ(summary.TileSetPaths.size(), 1u);
    EXPECT_TRUE(std::filesystem::exists(summary.TileSetPaths[0]));
    EXPECT_TRUE(summary.MapPaths.empty());
}
