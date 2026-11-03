#include "Thumbnails/TileThumbnailHandler.h"

#include "Thumbnails/ThumbnailImageCache.h"

#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "TileMap/TileCellEncoding.h"
#include "TileMap/TileMapChunk.h"

#include <gtest/gtest.h>

#include <chrono>
#include <fstream>
#include <future>
#include <regex>
#include <string>

using namespace GameEngine;
using namespace GameEngine::Editor;

namespace
{

constexpr int32 kTilePx = 16;
constexpr int32 kAtlasWidthPx = kTilePx * 2;
constexpr uint8 kLeftRed = 200;
constexpr uint8 kRightBlue = 180;

/** How long a composite may take before the test calls it hung, not slow. */
constexpr auto kThumbnailTimeout = std::chrono::seconds(20);

/**
 * @brief Drives TileThumbnailHandler against real files, the way the editor
 * does: an AssetManager over a project directory, GUIDs from the registry, and
 * a cache root the handler writes PNGs into.
 *
 * The handler is where both shipped thumbnail defects lived — a flip encoded so
 * the blit skipped it, and a format version that never reached the cache name —
 * and neither was reachable from the leaf composite tests, which never open a
 * file.
 */
class TileThumbnailHandlerTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        m_ProjectDir = std::filesystem::temp_directory_path() /
                       ("tilethumb_test_" + std::to_string(::testing::UnitTest::GetInstance()
                                                               ->current_test_info()
                                                               ->line()));
        std::filesystem::remove_all(m_ProjectDir);
        m_AssetsDir = m_ProjectDir / "Assets";
        m_CacheDir = m_ProjectDir / ".Editor" / "Thumbnails";
        std::filesystem::create_directories(m_AssetsDir);
        std::filesystem::create_directories(m_CacheDir);

        WriteAtlas();

        m_JobSystem = std::make_unique<JobSystem::WorkStealingThreadPool>(2);
        m_AssetManager = std::make_unique<AssetManager>();
        ASSERT_TRUE(m_AssetManager->Initialize(m_ProjectDir, m_JobSystem.get()));

        m_AtlasGuid = Register(m_AssetsDir / "atlas.png");
        ASSERT_FALSE(m_AtlasGuid.IsNull());
        WriteTileSet();
        m_TileSetGuid = Register(m_AssetsDir / "verify.tileset");
        ASSERT_FALSE(m_TileSetGuid.IsNull());
    }

    void TearDown() override
    {
        if (m_AssetManager)
            m_AssetManager->Shutdown();
        m_AssetManager.reset();
        m_JobSystem.reset();
        std::error_code ec;
        std::filesystem::remove_all(m_ProjectDir, ec);
    }

    GUID Register(const std::filesystem::path& path)
    {
        auto result = m_AssetManager->GetRegistry().RegisterAssetByPath(path);
        return result.IsOk() ? result.Value() : GUID::Null();
    }

    /** Two 16px tiles: the left one split red|blue, the right one opaque green. */
    void WriteAtlas() const
    {
        ThumbnailImage atlas = MakeTransparent(kAtlasWidthPx, kTilePx);
        for (int32 y = 0; y < kTilePx; ++y)
        {
            for (int32 x = 0; x < kAtlasWidthPx; ++x)
            {
                const size_t i = (static_cast<size_t>(y) * kAtlasWidthPx + x) * 4u;
                if (x >= kTilePx)
                    atlas.Pixels[i + 1] = 255; // green
                else if (x < kTilePx / 2)
                    atlas.Pixels[i + 0] = kLeftRed;
                else
                    atlas.Pixels[i + 2] = kRightBlue;
                atlas.Pixels[i + 3] = 255;
            }
        }
        ASSERT_TRUE(WritePng(m_AssetsDir / "atlas.png", atlas));
    }

    void WriteTileSet() const
    {
        std::ofstream out(m_AssetsDir / "verify.tileset");
        out << R"({"schemaVersion":1,"assetType":"TileSet","atlasTexture":")"
            << m_AtlasGuid.ToString() << R"(","atlasWidthPx":)" << kAtlasWidthPx
            << R"(,"atlasHeightPx":)" << kTilePx << R"(,"tileWidthPx":)" << kTilePx
            << R"(,"tileHeightPx":)" << kTilePx
            << R"(,"marginPx":0,"spacingPx":0,"columns":2,"rows":1,"tiles":[)"
            << R"({"id":1,"u0":0.0,"v0":0.0,"u1":0.5,"v1":1.0},)"
            << R"({"id":2,"u0":0.5,"v0":0.0,"u1":1.0,"v1":1.0}]})";
    }

    /** A one-chunk map whose row 0 holds `cells`, left to right. */
    void WriteTileMap(const std::vector<TileMap::TileCell>& cells,
                      const std::string& tileSetGuid) const
    {
        std::string rows;
        for (int32 y = 0; y < TileMap::kChunkSize; ++y)
        {
            std::string row;
            for (int32 x = 0; x < TileMap::kChunkSize; ++x)
            {
                const bool painted = y == 0 && x < static_cast<int32>(cells.size());
                row += (x == 0 ? "" : ",") +
                       std::to_string(painted ? cells[static_cast<size_t>(x)] : 0u);
            }
            rows += (y == 0 ? "\"" : ",\"") + row + "\"";
        }
        std::ofstream out(m_AssetsDir / "verify.tilemap");
        out << R"({"schemaVersion":1,"assetType":"TileMap","cellWidth":16,"cellHeight":16,)"
            << R"("nextLayerId":2,"layers":[{"id":1,"name":"Layer","type":"Tiles","tileset":")"
            << tileSetGuid << R"(","chunks":[{"x":0,"y":0,"rows":[)" << rows << R"(]}]}]})";
    }

    /** Blocks until the handler reports a result, so a hang fails as a hang. */
    std::string RequestAndWait(TileThumbnailHandler& handler, const std::filesystem::path& asset)
    {
        std::promise<std::string> promise;
        std::future<std::string> ready = promise.get_future();
        const std::string immediate = handler.GetOrRequest(
            asset, 128, [&promise](const std::string& path) { promise.set_value(path); });
        if (!immediate.empty())
            return immediate;
        if (ready.wait_for(kThumbnailTimeout) != std::future_status::ready)
        {
            ADD_FAILURE() << "thumbnail request never completed";
            return {};
        }
        return ready.get();
    }

    /** The same wait for a single-layer preview. */
    std::string RequestLayerAndWait(TileThumbnailHandler& handler,
                                    const std::filesystem::path& asset, uint32_t layerId)
    {
        std::promise<std::string> promise;
        std::future<std::string> ready = promise.get_future();
        const std::string immediate = handler.GetOrRequestLayer(
            asset, layerId, [&promise](const std::string& path) { promise.set_value(path); });
        if (!immediate.empty())
            return immediate;
        if (ready.wait_for(kThumbnailTimeout) != std::future_status::ready)
        {
            ADD_FAILURE() << "layer thumbnail request never completed";
            return {};
        }
        return ready.get();
    }

    /** Held by pointer: the handler owns a mutex, so it neither copies nor moves. */
    std::unique_ptr<TileThumbnailHandler> MakeHandler() const
    {
        auto handler = std::make_unique<TileThumbnailHandler>(*m_JobSystem, m_AssetManager.get());
        handler->SetAssetsRoot(m_AssetsDir);
        handler->SetCacheRoot(m_CacheDir);
        return handler;
    }

    std::filesystem::path m_ProjectDir;
    std::filesystem::path m_AssetsDir;
    std::filesystem::path m_CacheDir;
    std::unique_ptr<JobSystem::WorkStealingThreadPool> m_JobSystem;
    std::unique_ptr<AssetManager> m_AssetManager;
    GUID m_AtlasGuid;
    GUID m_TileSetGuid;
};

/** The red channel at a fraction across the first cell of a generated PNG. */
uint8 RedAt(const ThumbnailImage& image, float32 acrossFirstCell, int32 cellWidth)
{
    const int32 x = static_cast<int32>(static_cast<float32>(cellWidth) * acrossFirstCell);
    const size_t index =
        (static_cast<size_t>(image.Height / 2) * static_cast<size_t>(image.Width) +
         static_cast<size_t>(x)) * 4u;
    return index < image.Pixels.size() ? image.Pixels[index] : 0u;
}

} // namespace

TEST_F(TileThumbnailHandlerTest, ATileSetPreviewIsTheSlicedAtlas)
{
    const auto handler = MakeHandler();
    const std::string path = RequestAndWait(*handler, m_AssetsDir / "verify.tileset");
    ASSERT_FALSE(path.empty());

    const ThumbnailImage image = DecodeImage(path);
    EXPECT_EQ(image.Width, kAtlasWidthPx);
    EXPECT_EQ(image.Height, kTilePx);
    EXPECT_EQ(RedAt(image, 0.25f, kTilePx), kLeftRed);
}

TEST_F(TileThumbnailHandlerTest, AFlippedCellIsDrawnMirroredNotDropped)
{
    // The defect this locks: a flip used to leave the cell blank entirely.
    WriteTileMap({TileMap::MakeCell(1), TileMap::MakeCell(1, /*flipH=*/true)},
                 m_TileSetGuid.ToString());
    const auto handler = MakeHandler();
    const std::string path = RequestAndWait(*handler, m_AssetsDir / "verify.tilemap");
    ASSERT_FALSE(path.empty());

    const ThumbnailImage image = DecodeImage(path);
    ASSERT_FALSE(image.IsEmpty());
    const int32 cellWidth = image.Width / 2;

    // Plain cell: red on its left. Flipped cell: red on its RIGHT.
    EXPECT_EQ(RedAt(image, 0.25f, cellWidth), kLeftRed);
    EXPECT_EQ(RedAt(image, 1.75f, cellWidth), kLeftRed);
    EXPECT_EQ(RedAt(image, 1.25f, cellWidth), 0u) << "flipped cell drew unmirrored";
}

TEST_F(TileThumbnailHandlerTest, ATransposedCellIsDrawnAcrossTheDiagonal)
{
    WriteTileMap({TileMap::MakeCell(1, false, false, /*transpose=*/true)},
                 m_TileSetGuid.ToString());
    const auto handler = MakeHandler();
    const std::string path = RequestAndWait(*handler, m_AssetsDir / "verify.tilemap");
    ASSERT_FALSE(path.empty());

    const ThumbnailImage image = DecodeImage(path);
    ASSERT_FALSE(image.IsEmpty());
    // The tile's vertical red|blue split becomes horizontal: red at the bottom.
    const size_t topRow = (static_cast<size_t>(image.Width) / 4u) * 4u;
    const size_t bottomRow = ((static_cast<size_t>(image.Height - 1) * image.Width) +
                              static_cast<size_t>(image.Width) / 4u) * 4u;
    EXPECT_EQ(image.Pixels[topRow], 0u);
    EXPECT_EQ(image.Pixels[bottomRow], kLeftRed);
}

TEST_F(TileThumbnailHandlerTest, EditingAnAtlasInvalidatesTheMapsDrawnWithIt)
{
    WriteTileMap({TileMap::MakeCell(1)}, m_TileSetGuid.ToString());
    const auto handler = MakeHandler();
    const std::string first = RequestAndWait(*handler, m_AssetsDir / "verify.tilemap");
    ASSERT_FALSE(first.empty());

    // The atlas is two assets removed from the map, and touching it must still
    // move the map's stamp — the dependency chain the cache name is built on.
    std::filesystem::last_write_time(m_AssetsDir / "atlas.png",
                                     std::filesystem::file_time_type::clock::now() +
                                         std::chrono::seconds(5));
    const auto fresh = MakeHandler();
    const std::string second = RequestAndWait(*fresh, m_AssetsDir / "verify.tilemap");
    ASSERT_FALSE(second.empty());
    EXPECT_NE(first, second);
}

TEST_F(TileThumbnailHandlerTest, TheFormatVersionIsPartOfTheCacheName)
{
    // Regression lock: the constant shipped once declared but never folded into
    // the name, so the drawing fix it accompanied reached no existing cache
    // file. Reading the version off the name keeps this independent of how the
    // rest of the name is built.
    WriteTileMap({TileMap::MakeCell(1)}, m_TileSetGuid.ToString());
    const auto handler = MakeHandler();
    const std::string produced = RequestAndWait(*handler, m_AssetsDir / "verify.tilemap");
    ASSERT_FALSE(produced.empty());

    const std::string name = std::filesystem::path(produced).filename().string();
    EXPECT_NE(name.find("v" + std::to_string(kTileThumbnailFormatVersion) + "_"),
              std::string::npos)
        << "cache name " << name << " does not say which pipeline drew it";
}

TEST_F(TileThumbnailHandlerTest, AThumbnailFromAnOlderFormatIsSweptNotLeftBehind)
{
    // What the version is for: bumping it must not strand the files the old one
    // wrote, or every project accumulates a dead thumbnail per asset per bump.
    WriteTileMap({TileMap::MakeCell(1)}, m_TileSetGuid.ToString());
    const auto handler = MakeHandler();
    const std::string produced = RequestAndWait(*handler, m_AssetsDir / "verify.tilemap");
    ASSERT_FALSE(produced.empty());

    // Same asset, a version ago: the name a previous pipeline would have used.
    const std::string current = std::filesystem::path(produced).filename().string();
    const std::string previous = std::regex_replace(
        current, std::regex("v" + std::to_string(kTileThumbnailFormatVersion) + "_"),
        "v" + std::to_string(kTileThumbnailFormatVersion - 1) + "_");
    ASSERT_NE(previous, current);
    { std::ofstream stale(m_CacheDir / previous); stale << "stale"; }
    ASSERT_TRUE(std::filesystem::exists(m_CacheDir / previous)) << "fixture wrote no stale file";

    // Regenerating sweeps everything sharing the asset's identity prefix.
    std::filesystem::last_write_time(m_AssetsDir / "atlas.png",
                                     std::filesystem::file_time_type::clock::now() +
                                         std::chrono::seconds(5));
    const auto fresh = MakeHandler();
    ASSERT_FALSE(RequestAndWait(*fresh, m_AssetsDir / "verify.tilemap").empty());
    EXPECT_FALSE(std::filesystem::exists(m_CacheDir / previous));
}

TEST_F(TileThumbnailHandlerTest, AWholeMapPreviewDoesNotSweepThePerLayerPreviews)
{
    // The whole-map and per-layer names share the asset hash, so the sweep can
    // only tell them apart if neither prefix is a prefix of the other. Both
    // files here are current: nothing about the layer preview is stale, and a
    // sweep that removes it takes the previews the layer rows are showing.
    WriteTileMap({TileMap::MakeCell(1)}, m_TileSetGuid.ToString());
    const auto handler = MakeHandler();

    const std::string layerPreview =
        RequestLayerAndWait(*handler, m_AssetsDir / "verify.tilemap", 1);
    ASSERT_FALSE(layerPreview.empty());

    const std::string wholeMap = RequestAndWait(*handler, m_AssetsDir / "verify.tilemap");
    ASSERT_FALSE(wholeMap.empty());
    ASSERT_NE(wholeMap, layerPreview);

    EXPECT_TRUE(std::filesystem::exists(layerPreview));
}

TEST_F(TileThumbnailHandlerTest, AMapWhoseTileSetIsMissingReportsNoPreview)
{
    WriteTileMap({TileMap::MakeCell(1)}, GUID::Null().ToString());
    const auto handler = MakeHandler();
    EXPECT_TRUE(RequestAndWait(*handler, m_AssetsDir / "verify.tilemap").empty());

    // Nothing half-written left behind for the asset grid to pick up.
    EXPECT_TRUE(std::filesystem::is_empty(m_CacheDir));
}
