#include <gtest/gtest.h>

#include "TileMap/TileSetAsset.h"
#include "TileMap/TileSetDocument.h"

#include <filesystem>
#include <fstream>

// The Tile Set Editor's file lifecycle. The defect these lock out is total and
// silent: binding the panel to an atlas texture used to start a blank asset and
// write it over the .tileset already sitting at that path, so every authored
// collision polygon, collision mode, physics material and terrain annotation in
// it was gone with no warning and no undo.

using namespace GameEngine;

namespace
{

std::filesystem::path MakeTempDir(const char* name)
{
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "GETileSetDocumentTests" / name;
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

/** A .tileset on disk carrying all three kinds of per-tile authoring. */
std::filesystem::path WriteAuthoredTileSet(const std::filesystem::path& dir,
                                           const GUID& materialGuid)
{
    const std::filesystem::path path = dir / "atlas.tileset";
    TileMap::TileSetAsset asset(GUID::Generate(), path);
    asset.SetAtlasTexture(GUID::Generate(), 64, 64);
    asset.SliceGrid(16, 16, 0, 0); // 4x4 = 16 tiles

    TileMap::TileSetTile* tile = asset.FindTile(6);
    TileMap::TileCollisionPolygon polygon;
    polygon.Points = {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 0.5f}};
    tile->CollisionPolygons.push_back(polygon);
    tile->CollisionMode = TileMap::TileCollisionMode::OneWay;
    tile->PhysicsMaterial = materialGuid;

    Vector<uint8> bytes;
    EXPECT_TRUE(asset.SaveToData(bytes));
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    out.close();
    return path;
}

} // namespace

TEST(TileSetDocument, AtlasTexturePathBecomesTheTileSetPath)
{
    EXPECT_EQ(Editor::TileSetDocument::PathForAtlasTexture("Assets/Tiles/forest_tiles.png"),
              std::filesystem::path("Assets/Tiles/forest_tiles.tileset"));
    EXPECT_TRUE(Editor::TileSetDocument::PathForAtlasTexture({}).empty());
}

// The data-loss regression: re-binding to the same atlas must READ the file
// that is already there, and the save that follows must not blank it.
TEST(TileSetDocument, ReBindingToAnAuthoredTileSetKeepsCollisionModeAndMaterial)
{
    const std::filesystem::path dir = MakeTempDir("rebind");
    const GUID material = GUID::Generate();
    const std::filesystem::path path = WriteAuthoredTileSet(dir, material);

    Editor::TileSetDocument document;
    ASSERT_EQ(document.OpenOrCreate(GUID::Generate(), path),
              Editor::TileSetDocument::OpenOutcome::Opened);

    // Exactly what pressing Apply & Save does to a freshly bound document.
    document.Asset()->SliceGrid(16, 16, 0, 0);
    ASSERT_EQ(document.Save(), Editor::TileSetDocument::SaveOutcome::Written);

    Editor::TileSetDocument reread;
    ASSERT_EQ(reread.OpenOrCreate(GUID::Generate(), path),
              Editor::TileSetDocument::OpenOutcome::Opened);
    const TileMap::TileSetTile* tile = reread.Asset()->FindTile(6);
    ASSERT_NE(tile, nullptr);
    EXPECT_EQ(tile->CollisionPolygons.size(), 1u);
    EXPECT_EQ(tile->CollisionMode, TileMap::TileCollisionMode::OneWay);
    EXPECT_EQ(tile->PhysicsMaterial, material);
}

// Two panels on one file, or an external edit between open and save: this
// document's copy has none of the other side's changes in it, so writing would
// drop them silently. The refusal is what makes the loss impossible.
TEST(TileSetDocument, SaveRefusesWhenTheFileChangedOnDiskSinceItWasRead)
{
    const std::filesystem::path dir = MakeTempDir("external");
    const std::filesystem::path path = WriteAuthoredTileSet(dir, GUID::Generate());

    Editor::TileSetDocument document;
    ASSERT_EQ(document.OpenOrCreate(GUID::Generate(), path),
              Editor::TileSetDocument::OpenOutcome::Opened);
    EXPECT_FALSE(document.HasChangedOnDisk());

    // Someone else writes the file. A second authored write is the realistic
    // shape; the bytes only have to differ.
    Editor::TileSetDocument other;
    ASSERT_EQ(other.OpenOrCreate(GUID::Generate(), path),
              Editor::TileSetDocument::OpenOutcome::Opened);
    other.Asset()->SliceGrid(32, 32, 0, 0);
    ASSERT_EQ(other.Save(), Editor::TileSetDocument::SaveOutcome::Written);

    EXPECT_TRUE(document.HasChangedOnDisk());
    EXPECT_EQ(document.Save(), Editor::TileSetDocument::SaveOutcome::ChangedOnDisk);
    // The other side's write is still the file.
    Editor::TileSetDocument reread;
    ASSERT_EQ(reread.OpenOrCreate(GUID::Generate(), path),
              Editor::TileSetDocument::OpenOutcome::Opened);
    EXPECT_EQ(reread.Asset()->Columns(), 2);

    // The author can still choose to make their copy win.
    document.AcceptDiskState();
    EXPECT_FALSE(document.HasChangedOnDisk());
    EXPECT_EQ(document.Save(), Editor::TileSetDocument::SaveOutcome::Written);
}

// A document bound to a path with no file behind it must notice a file
// appearing there — otherwise "create" silently becomes "overwrite".
TEST(TileSetDocument, SaveRefusesWhenAFileAppearedAtAPathThatWasEmpty)
{
    const std::filesystem::path dir = MakeTempDir("appeared");
    Editor::TileSetDocument document;
    ASSERT_EQ(document.OpenOrCreate(GUID::Generate(), dir / "atlas.tileset"),
              Editor::TileSetDocument::OpenOutcome::Created);

    (void)WriteAuthoredTileSet(dir, GUID::Generate()); // writes dir/atlas.tileset

    EXPECT_TRUE(document.HasChangedOnDisk());
    EXPECT_EQ(document.Save(), Editor::TileSetDocument::SaveOutcome::ChangedOnDisk);
}

// Consecutive saves from one document are not "external" changes to itself.
TEST(TileSetDocument, RepeatedSavesFromTheSameDocumentSucceed)
{
    const std::filesystem::path dir = MakeTempDir("resave");
    const std::filesystem::path path = WriteAuthoredTileSet(dir, GUID::Generate());

    Editor::TileSetDocument document;
    ASSERT_EQ(document.OpenOrCreate(GUID::Generate(), path),
              Editor::TileSetDocument::OpenOutcome::Opened);
    EXPECT_EQ(document.Save(), Editor::TileSetDocument::SaveOutcome::Written);
    document.Asset()->SliceGrid(32, 32, 0, 0);
    EXPECT_EQ(document.Save(), Editor::TileSetDocument::SaveOutcome::Written);
    EXPECT_EQ(document.Save(), Editor::TileSetDocument::SaveOutcome::Written);
}

// A grid that slices past the tile cap is refused — and the refusal must not be
// destructive either. Emptying the tile table here would leave the author one
// corrected grid away from writing an authoring-free tileset over their file.
TEST(TileSetDocument, ARefusedOversizeSliceLeavesTheAuthoredTilesIntact)
{
    const std::filesystem::path dir = MakeTempDir("oversize");
    const GUID material = GUID::Generate();
    const std::filesystem::path path = WriteAuthoredTileSet(dir, material);

    Editor::TileSetDocument document;
    ASSERT_EQ(document.OpenOrCreate(GUID::Generate(), path),
              Editor::TileSetDocument::OpenOutcome::Opened);
    TileMap::TileSetAsset* asset = document.Asset();
    // 64x64 atlas at 1px tiles is 4096 tiles; the cap bites well below 1px on a
    // large atlas, so grow the atlas rather than shrink the tile past 1.
    asset->SetAtlasTexture(asset->AtlasTexture(), 4096, 4096);
    ASSERT_FALSE(asset->SliceGrid(1, 1, 0, 0)); // 16.7M tiles

    // Still the tile table it was opened with, authoring and all.
    EXPECT_EQ(asset->Columns(), 4);
    EXPECT_EQ(asset->Rows(), 4);
    const TileMap::TileSetTile* tile = asset->FindTile(6);
    ASSERT_NE(tile, nullptr);
    EXPECT_EQ(tile->CollisionPolygons.size(), 1u);
    EXPECT_EQ(tile->PhysicsMaterial, material);
}

TEST(TileSetDocument, BindingToAPathWithNoFileStartsAnEmptyTileSet)
{
    const std::filesystem::path dir = MakeTempDir("create");
    Editor::TileSetDocument document;
    ASSERT_EQ(document.OpenOrCreate(GUID::Generate(), dir / "fresh.tileset"),
              Editor::TileSetDocument::OpenOutcome::Created);
    ASSERT_NE(document.Asset(), nullptr);
    EXPECT_TRUE(document.Asset()->Tiles().empty());
    EXPECT_FALSE(std::filesystem::exists(dir / "fresh.tileset")); // binding writes nothing
}

// A file the editor cannot read is the one case where overwriting is
// unrecoverable, so binding fails rather than replacing it with a blank asset.
TEST(TileSetDocument, AnUnreadableFileLeavesTheDocumentUnbound)
{
    const std::filesystem::path dir = MakeTempDir("malformed");
    const std::filesystem::path path = dir / "broken.tileset";
    std::ofstream(path) << "{ this is not json";

    Editor::TileSetDocument document;
    EXPECT_EQ(document.OpenOrCreate(GUID::Generate(), path),
              Editor::TileSetDocument::OpenOutcome::Failed);
    EXPECT_FALSE(document.IsOpen());
}

TEST(TileSetDocument, AuthoredDataIsDetectedAndAnEmptySliceIsNot)
{
    const std::filesystem::path dir = MakeTempDir("authored");
    const std::filesystem::path path = WriteAuthoredTileSet(dir, GUID::Generate());

    Editor::TileSetDocument authored;
    ASSERT_EQ(authored.OpenOrCreate(GUID::Generate(), path),
              Editor::TileSetDocument::OpenOutcome::Opened);
    EXPECT_TRUE(authored.HasAuthoredTileData());

    Editor::TileSetDocument fresh;
    ASSERT_EQ(fresh.OpenOrCreate(GUID::Generate(), dir / "fresh.tileset"),
              Editor::TileSetDocument::OpenOutcome::Created);
    fresh.Asset()->SetAtlasTexture(GUID::Generate(), 64, 64);
    fresh.Asset()->SliceGrid(16, 16, 0, 0);
    EXPECT_FALSE(fresh.HasAuthoredTileData());
}

// Tile ids are grid positions, so only a grid that slices to a different shape
// moves authoring onto tiles it no longer describes. Re-saving the same grid,
// and any grid change on an unauthored tile set, are not destructive.
TEST(TileSetDocument, OnlyAGridThatReshapesTheTileTableCountsAsDestructive)
{
    const std::filesystem::path dir = MakeTempDir("reslice");
    const std::filesystem::path path = WriteAuthoredTileSet(dir, GUID::Generate());

    Editor::TileSetDocument document;
    ASSERT_EQ(document.OpenOrCreate(GUID::Generate(), path),
              Editor::TileSetDocument::OpenOutcome::Opened);

    EXPECT_FALSE(document.ReSliceDropsAuthoring(16, 16, 0, 0)); // the grid it was authored on
    EXPECT_TRUE(document.ReSliceDropsAuthoring(32, 32, 0, 0));  // 4x4 tiles => 2x2
    EXPECT_TRUE(document.ReSliceDropsAuthoring(16, 16, 8, 0));  // margin drops a column and row

    Editor::TileSetDocument fresh;
    ASSERT_EQ(fresh.OpenOrCreate(GUID::Generate(), dir / "fresh.tileset"),
              Editor::TileSetDocument::OpenOutcome::Created);
    fresh.Asset()->SetAtlasTexture(GUID::Generate(), 64, 64);
    fresh.Asset()->SliceGrid(16, 16, 0, 0);
    EXPECT_FALSE(fresh.ReSliceDropsAuthoring(32, 32, 0, 0)); // nothing authored to lose
}
