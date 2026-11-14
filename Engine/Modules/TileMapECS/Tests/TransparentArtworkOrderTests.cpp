#include "Engine/Rendering/SortedTransparentComposition.h"
#include "Engine/Rendering/2D/SpriteRenderFeature.h"
#include "Engine/Rendering/RenderServices.h"
#include "TileMapECS/TileMapRenderFeature.h"

#include <gtest/gtest.h>

using namespace GameEngine;
using namespace GameEngine::Engine::Renderer;

TEST(TransparentArtworkOrder, SharedAtlasSpritesSplitAtDepthBoundaries)
{
    RenderSortKey back{}, front{};
    back.Depth = 10;
    front.Depth = -10;
    SpriteBatch batch{};
    batch.Sort = back;
    EXPECT_FALSE(CanAppendSpriteBatch(batch, {}, {}, front, false));
    EXPECT_TRUE(CanAppendSpriteBatch(batch, {}, {}, back, false));
}

TEST(TransparentArtworkOrder, AuthoredPriorityPrecedesStableFamilyTie)
{
    std::vector<SortedTransparentCommand> draws(3);
    draws[0].Family = 1;
    draws[0].Sorting.Layer = 2;
    draws[1].Family = 2;
    draws[1].Sorting.Layer = 1;
    draws[2].Family = 3;
    draws[2].Sorting.Layer = 1;
    SortedTransparentRunPartition partition;
    std::vector<uint32> meshOrder;
    std::vector<TransparentCompositionSlot> slots;
    std::pmr::monotonic_buffer_resource scratch;
    BuildTransparentComposition({}, {}, {}, draws, scratch, partition, meshOrder, slots, false);
    ASSERT_EQ(slots.size(), 3u);
    EXPECT_EQ(slots[0].Index, 1u);
    EXPECT_EQ(slots[1].Index, 2u);
    EXPECT_EQ(slots[2].Index, 0u);
    std::swap(draws[0], draws[2]);
    BuildTransparentComposition({}, {}, {}, draws, scratch, partition, meshOrder, slots, false);
    EXPECT_EQ(draws[slots[0].Index].Family, 2u);
    EXPECT_EQ(draws[slots[1].Index].Family, 3u);
    EXPECT_EQ(draws[slots[2].Index].Family, 1u);
}

TEST(TransparentArtworkOrder, ExtractedArtworkOnlyAppearsInItsOwningWorld)
{
    RenderServices services;
    SpriteRenderFeature sprites(services);
    TileMapECS::TileMapRenderFeature tiles(services);
    std::vector<SpriteDraw> spriteDraws(1);
    sprites.SetSpriteDraws(17, spriteDraws);
    tiles.SetLayerDraws(17, std::vector<TileMapECS::TileMapLayerDraw>(1));
    EXPECT_TRUE(sprites.HasSpriteDraws(17));
    EXPECT_TRUE(tiles.HasLayerDraws(17));
    EXPECT_FALSE(sprites.HasSpriteDraws(23));
    EXPECT_FALSE(tiles.HasLayerDraws(23));
    EXPECT_TRUE(sprites.HasSpriteDraws(0));
    EXPECT_TRUE(tiles.HasLayerDraws(0));
    spriteDraws.clear();
    sprites.SetSpriteDraws(17, spriteDraws);
    tiles.SetLayerDraws(17, {});
    EXPECT_FALSE(sprites.HasSpriteDraws(17));
    EXPECT_FALSE(tiles.HasLayerDraws(17));
}
