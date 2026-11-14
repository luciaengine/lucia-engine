#include <gtest/gtest.h>

#include "TileMap/TileMapData.h"

// Undoing a layer removal. The trap this locks out is not the removal itself but
// what an undo that restores the layer under a FRESH id would do to the rest of
// the stack: layer ids are minted from a monotonic counter and never reused, so
// every already-recorded edit still names the old id, resolves no layer, and
// silently no-ops. Ctrl+Z would look broken while the stack kept counting.

using namespace GameEngine;

namespace
{

/** A three-layer map with a painted cell on each, ids 1..3. */
TileMap::TileMapData MakeThreeLayerMap()
{
    TileMap::TileMapData data;
    for (int i = 0; i < 3; ++i)
    {
        const uint32 id = data.AddLayer("Layer " + std::to_string(i + 1),
                                        TileMap::TileMapLayerType::Tiles);
        data.FindLayer(id)->SetCell(i, i, TileMap::MakeCell(static_cast<uint32>(10 + i)));
    }
    return data;
}

} // namespace

TEST(TileMapLayerRestore, RemovedLayerComesBackAtItsIndexUnderItsOriginalId)
{
    TileMap::TileMapData data = MakeThreeLayerMap();

    const TileMap::TileMapLayerData removed = data.Layers()[1];
    const uint32 removedId = removed.Id;
    ASSERT_TRUE(data.RemoveLayer(removedId));
    ASSERT_EQ(data.Layers().size(), 2u);
    EXPECT_EQ(data.FindLayer(removedId), nullptr);

    ASSERT_TRUE(data.InsertLayer(1, removed));
    ASSERT_EQ(data.Layers().size(), 3u);
    EXPECT_EQ(data.Layers()[1].Id, removedId);
    // The cells came back with it, not just the layer.
    EXPECT_EQ(data.Layers()[1].GetCell(1, 1), TileMap::MakeCell(11u));
    // Neighbours kept their order.
    EXPECT_EQ(data.Layers()[0].Id, 1u);
    EXPECT_EQ(data.Layers()[2].Id, 3u);
}

// The id is the whole point: a command recorded against it must still resolve
// after the undo.
TEST(TileMapLayerRestore, AnEditRecordedAgainstTheOldIdResolvesAgainAfterRestore)
{
    TileMap::TileMapData data = MakeThreeLayerMap();
    const TileMap::TileMapLayerData removed = data.Layers()[1];
    const uint32 recordedLayerId = removed.Id;

    ASSERT_TRUE(data.RemoveLayer(recordedLayerId));
    ASSERT_EQ(data.FindLayer(recordedLayerId), nullptr); // the stale-command window
    ASSERT_TRUE(data.InsertLayer(1, removed));

    TileMap::TileMapLayerData* layer = data.FindLayer(recordedLayerId);
    ASSERT_NE(layer, nullptr);
    layer->SetCell(5, 5, TileMap::MakeCell(99u));
    EXPECT_EQ(data.FindLayer(recordedLayerId)->GetCell(5, 5), TileMap::MakeCell(99u));
}

// A layer added while the removal stood must never be able to take the restored
// layer's id — that would make two commands point at one layer.
TEST(TileMapLayerRestore, IdsMintedWhileTheLayerWasGoneDoNotCollideWithIt)
{
    TileMap::TileMapData data = MakeThreeLayerMap();
    const TileMap::TileMapLayerData removed = data.Layers()[2]; // the highest id
    ASSERT_TRUE(data.RemoveLayer(removed.Id));

    const uint32 minted = data.AddLayer("New", TileMap::TileMapLayerType::Tiles);
    EXPECT_NE(minted, removed.Id);

    ASSERT_TRUE(data.InsertLayer(data.Layers().size(), removed));
    EXPECT_NE(data.AddLayer("Newer", TileMap::TileMapLayerType::Tiles), removed.Id);
    EXPECT_GT(data.NextLayerId(), removed.Id);
}

TEST(TileMapLayerRestore, RestoringOverALiveIdOrPastTheEndIsRefused)
{
    TileMap::TileMapData data = MakeThreeLayerMap();
    const TileMap::TileMapLayerData live = data.Layers()[0];

    EXPECT_FALSE(data.InsertLayer(0, live)); // id 1 is still present
    EXPECT_EQ(data.Layers().size(), 3u);

    TileMap::TileMapLayerData removed = data.Layers()[1];
    ASSERT_TRUE(data.RemoveLayer(removed.Id));
    EXPECT_FALSE(data.InsertLayer(data.Layers().size() + 1, removed));
    EXPECT_EQ(data.Layers().size(), 2u);
}
