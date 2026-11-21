#include "Engine/Rendering/Forest/ForestPlacementRuntime.h"
#include "PageStreaming/Placement/PackedPlacementStore.h"
#include "TestTempDir.h"

#include <gtest/gtest.h>

#include <array>

using namespace GameEngine;
using namespace GameEngine::Engine::Renderer;
using namespace GameEngine::PageStreaming;

namespace
{
class ForestPlacementShadowTest : public testing::Test
{
protected:
    template <size_t Count>
    void WriteArchive()
    {
        std::array<PlacementPage, Count> pages{};
        for (uint32 page = 0; page < Count; ++page)
        {
            pages[page].Origin = {55.0 * page, 0.0, 0.0};
            pages[page].LocalMin = {-2.0f, 0.0f, -2.0f};
            pages[page].LocalMax = {18.0f, 25.0f, 18.0f};
            pages[page].First = page;
            pages[page].Count = 1;
        }
        std::array<PackedPlacement, Count> trees{};
        trees.fill(Placement);
        ASSERT_TRUE(WritePackedPlacementStore(Archive, pages, trees).empty());
    }

    bool Publish(ForestPlacementRuntime& runtime, ForestPageTicket ticket) const
    {
        const std::span bytes(reinterpret_cast<const uint8*>(&Placement), sizeof(Placement));
        return runtime.CompleteRead(ticket, bytes) && runtime.CompleteUpload(ticket, true);
    }

    const TestUtils::ScopedTempDir Scratch{TestUtils::MakeUniqueTempDirectory("forest_shadow_placement")};
    const std::filesystem::path Archive = Scratch.Path() / "Trees.geforest";
    const PackedPlacement Placement{12, 99, 0, 32768};
};
} // namespace

TEST_F(ForestPlacementShadowTest, SinglePageCanDrawOnlyToSunCascades)
{
    ASSERT_NO_FATAL_FAILURE(WriteArchive<1>());
    std::string error;
    auto runtime = ForestPlacementRuntime::OpenQuadStand(Archive, error, 128, 4, true, 4, 16);
    ASSERT_TRUE(runtime) << error;
    ASSERT_EQ(runtime->SlotCount(), 1u);

    ForestPlacementView view{{0, 8, 0}, {}, 275.0f};
    view.EyeRelativePlanes[0] = {0, 0, -1, -400};
    constexpr std::array<ForestShadowPage, 1> shadowDemand{{{0, 14}}};
    ForestPlacementFrame plan;
    ASSERT_TRUE(runtime->Prepare(view, 1, 0, plan, shadowDemand));
    EXPECT_EQ(plan.VisibleCells, 0u);
    EXPECT_FALSE(plan.WantsDetails);
    EXPECT_TRUE(plan.DetailedDraws.empty());
    ASSERT_EQ(plan.Reads.size(), 1u);
    const auto ticket = plan.Reads.front();
    EXPECT_EQ(ticket.Page, 0u);
    ASSERT_TRUE(Publish(*runtime, ticket));

    ASSERT_TRUE(runtime->Prepare(view, 2, 1, plan, shadowDemand));
    EXPECT_TRUE(plan.DetailedDraws.empty());
    ASSERT_EQ(plan.ShadowDraws.size(), 1u);
    EXPECT_EQ(plan.ShadowDraws.front().Draw.Ticket, ticket);
    EXPECT_EQ(plan.ShadowDraws.front().CascadeMask, 14u);
}

TEST_F(ForestPlacementShadowTest, ShadowReplacementKeepsTheCameraCutReserve)
{
    ASSERT_NO_FATAL_FAILURE(WriteArchive<4>());
    std::string error;
    auto runtime = ForestPlacementRuntime::OpenQuadStand(Archive, error, 1, 1, true, 1, 1);
    ASSERT_TRUE(runtime) << error;
    ASSERT_EQ(runtime->SlotCount(), 3u);

    ForestPlacementView camera{{0, 8, 0}, {}, 31.0f};
    camera.EyeRelativePlanes[0] = {-1, 0, 0, 30};
    constexpr std::array<ForestShadowPage, 1> oldLight{{{1, 1}}};
    constexpr std::array<ForestShadowPage, 1> newLight{{{2, 4}}};
    ForestPlacementFrame plan;
    ASSERT_TRUE(runtime->Prepare(camera, 1, 0, plan, oldLight));
    ASSERT_EQ(plan.Reads.size(), 1u);
    const auto oldColor = plan.Reads.front();
    ASSERT_EQ(oldColor.Page, 0u);
    ASSERT_TRUE(Publish(*runtime, oldColor));

    ASSERT_TRUE(runtime->Prepare(camera, 2, 1, plan, oldLight));
    ASSERT_EQ(plan.Reads.size(), 1u);
    const auto oldShadow = plan.Reads.front();
    ASSERT_EQ(oldShadow.Page, 1u);
    ASSERT_TRUE(Publish(*runtime, oldShadow));

    ASSERT_TRUE(runtime->Prepare(camera, 3, 2, plan, oldLight));
    ASSERT_EQ(plan.ShadowDraws.size(), 1u);
    EXPECT_EQ(plan.ShadowDraws.front().Draw.Ticket, oldShadow);
    ASSERT_TRUE(runtime->Prepare(camera, 4, 2, plan, newLight));
    ASSERT_EQ(plan.DetailedDraws.size(), 1u);
    EXPECT_EQ(plan.DetailedDraws.front().Ticket, oldColor);
    EXPECT_TRUE(plan.ShadowDraws.empty());
    EXPECT_TRUE(plan.Reads.empty());

    ForestPlacementView cut{{165, 8, 0}, {}, 31.0f};
    cut.EyeRelativePlanes[0] = {1, 0, 0, 24};
    ASSERT_TRUE(runtime->Prepare(cut, 5, 2, plan, newLight));
    EXPECT_EQ(plan.VisibleCells, 1u);
    EXPECT_EQ(plan.StandDraws.size(), 1u);
    EXPECT_TRUE(plan.DetailedDraws.empty());
    ASSERT_EQ(plan.Reads.size(), 1u);
    const auto incomingColor = plan.Reads.front();
    EXPECT_EQ(incomingColor.Page, 3u);
    EXPECT_NE(incomingColor.Slot, oldColor.Slot);
    EXPECT_NE(incomingColor.Slot, oldShadow.Slot);
    ASSERT_TRUE(Publish(*runtime, incomingColor));

    ASSERT_TRUE(runtime->Prepare(cut, 6, 3, plan, newLight));
    EXPECT_TRUE(plan.Reads.empty());
    ASSERT_EQ(plan.DetailedDraws.size(), 1u);
    EXPECT_EQ(plan.DetailedDraws.front().Ticket, incomingColor);

    ASSERT_TRUE(runtime->Prepare(cut, 7, 4, plan, newLight));
    ASSERT_EQ(plan.Reads.size(), 1u);
    const auto incomingShadow = plan.Reads.front();
    EXPECT_EQ(incomingShadow.Page, 2u);
    EXPECT_NE(incomingShadow.Slot, incomingColor.Slot);
    ASSERT_TRUE(Publish(*runtime, incomingShadow));

    ASSERT_TRUE(runtime->Prepare(cut, 8, 5, plan, newLight));
    ASSERT_EQ(plan.DetailedDraws.size(), 1u);
    EXPECT_EQ(plan.DetailedDraws.front().Ticket, incomingColor);
    ASSERT_EQ(plan.ShadowDraws.size(), 1u);
    EXPECT_EQ(plan.ShadowDraws.front().Draw.Ticket, incomingShadow);
    EXPECT_EQ(plan.ShadowDraws.front().CascadeMask, 4u);
}
