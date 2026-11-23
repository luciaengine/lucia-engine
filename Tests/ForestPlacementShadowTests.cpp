// The forest pages that sun cascades want: the shadow cap inside the one slot pool, color demand
// first at a camera cut, the prefetch reserve, one slot for a page that the view and a cascade
// both want, the detail distance limit on shadow pages, read retries, and the reader fence of a
// cascade draw.

#include "PackedPlacementTestArchive.h"

#include "ForestPlacementRuntime.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace GameEngine::PageStreaming
{
namespace
{
using namespace Engine::Renderer;

// The pages of a row are this far apart along +X, in meters.
constexpr double kRowSpacing = 55.0;
const Mathematics::WorldPosition kRowOrigin{512'000'000.0, 8.0, 512'000'001.0};

class ForestPlacementShadowTest : public PackedPlacementArchiveTest
{
protected:
    // A row of pages along +X, one placement each, with the bytes of kFirstPage.
    std::shared_ptr<ForestPlacementRuntime> OpenRow(uint32 count, const ForestPlacementRuntimeDesc& desc)
    {
        std::vector<PlacementPage> pages(count, m_Pages[0]);
        std::vector<PackedPlacement> placements(count);
        for (uint32 page = 0; page < count; ++page)
        {
            pages[page].Origin = {kRowOrigin.X + kRowSpacing * page, kRowOrigin.Y, kRowOrigin.Z};
            pages[page].First = page;
            pages[page].Count = 1;
            std::memcpy(&placements[page], kFirstPage.data(), sizeof(PackedPlacement));
        }
        const auto file = m_Scratch.Path() / "row.geforest";
        EXPECT_EQ(WritePackedPlacementStore(file, kTestBoundRadius, pages, placements), "");
        std::string error;
        auto runtime = ForestPlacementRuntime::Open(file, desc, error);
        EXPECT_TRUE(runtime) << error;
        return runtime;
    }

    static bool Publish(ForestPlacementRuntime& runtime, ForestPageTicket ticket)
    {
        return runtime.CompleteRead(ticket, kFirstPage) &&
               runtime.CompleteUpload(ticket, ForestUploadResult::Published);
    }

    // A camera at page `page` that sees only x >= its eye - 24 m (forward) or x <= its eye + 30 m.
    static ForestPlacementView Camera(uint32 page, bool forward)
    {
        ForestPlacementView view{{kRowOrigin.X + kRowSpacing * page, kRowOrigin.Y, kRowOrigin.Z}, {}, 31.0f};
        view.EyeRelativePlanes[0] = forward ? Mathematics::Vector4{1, 0, 0, 24} : Mathematics::Vector4{-1, 0, 0, 30};
        return view;
    }

    // A camera 36 m above page `page` that sees every page and has only that page in its prefetch
    // band (31 m to 43.4 m).
    static ForestPlacementView Above(uint32 page)
    {
        return {{kRowOrigin.X + kRowSpacing * page + 8.0, kRowOrigin.Y + 25.0 + 36.0, kRowOrigin.Z + 8.0}, {}, 31.0f};
    }

    // A camera at the row origin that sees no page.
    static ForestPlacementView Blind()
    {
        ForestPlacementView view{kRowOrigin, {}, 31.0f};
        view.EyeRelativePlanes[0] = {0, 0, -1, -400};
        return view;
    }

    // The light moves from page 1 to page 2, then the camera cuts from page 0 to page 3.
    void ExpectCameraCutKeepsItsSlots(const ForestPlacementRuntimeDesc& desc)
    {
        auto runtime = OpenRow(4, desc);
        ASSERT_TRUE(runtime);
        ASSERT_EQ(runtime->SlotCount(), 3u);
        const ForestPlacementView camera = Camera(0, false);
        constexpr std::array<ForestShadowPage, 1> oldLight{{{1, 1}}};
        constexpr std::array<ForestShadowPage, 1> newLight{{{2, 4}}};
        ForestPlacementFrame plan;
        ASSERT_TRUE(runtime->Prepare(camera, 1, 0, oldLight, plan));
        ASSERT_EQ(plan.Reads.size(), 1u);
        const auto oldColor = plan.Reads.front();
        ASSERT_EQ(oldColor.Page, 0u);
        ASSERT_TRUE(Publish(*runtime, oldColor));

        ASSERT_TRUE(runtime->Prepare(camera, 2, 1, oldLight, plan));
        ASSERT_EQ(plan.Reads.size(), 1u);
        const auto oldShadow = plan.Reads.front();
        ASSERT_EQ(oldShadow.Page, 1u);
        ASSERT_TRUE(Publish(*runtime, oldShadow));

        ASSERT_TRUE(runtime->Prepare(camera, 3, 2, oldLight, plan));
        ASSERT_EQ(plan.ShadowDraws.size(), 1u);
        EXPECT_EQ(plan.ShadowDraws.front().Draw.Ticket, oldShadow);
        // The retired shadow page still holds its slot until its cascade draw completes.
        ASSERT_TRUE(runtime->Prepare(camera, 4, 2, newLight, plan));
        ASSERT_EQ(plan.DetailedDraws.size(), 1u);
        EXPECT_EQ(plan.DetailedDraws.front().Ticket, oldColor);
        EXPECT_TRUE(plan.ShadowDraws.empty());
        EXPECT_TRUE(plan.Reads.empty());

        // The cut reads its page at once, while both earlier slots still wait for their draws.
        ASSERT_TRUE(runtime->Prepare(Camera(3, true), 5, 2, newLight, plan));
        EXPECT_EQ(plan.VisiblePages, 1u);
        EXPECT_TRUE(plan.DetailedDraws.empty());
        ASSERT_EQ(plan.Reads.size(), 1u);
        const auto incomingColor = plan.Reads.front();
        EXPECT_EQ(incomingColor.Page, 3u);
        EXPECT_NE(incomingColor.Slot, oldColor.Slot);
        EXPECT_NE(incomingColor.Slot, oldShadow.Slot);
        ASSERT_TRUE(Publish(*runtime, incomingColor));

        // The new shadow page takes the slot of the shadow page that it replaces.
        ASSERT_TRUE(runtime->Prepare(Camera(3, true), 6, 3, newLight, plan));
        ASSERT_EQ(plan.DetailedDraws.size(), 1u);
        EXPECT_EQ(plan.DetailedDraws.front().Ticket, incomingColor);
        ASSERT_EQ(plan.Reads.size(), 1u);
        const auto incomingShadow = plan.Reads.front();
        EXPECT_EQ(incomingShadow.Page, 2u);
        EXPECT_EQ(incomingShadow.Slot, oldShadow.Slot);
        ASSERT_TRUE(Publish(*runtime, incomingShadow));

        ASSERT_TRUE(runtime->Prepare(Camera(3, true), 7, 4, newLight, plan));
        ASSERT_EQ(plan.DetailedDraws.size(), 1u);
        EXPECT_EQ(plan.DetailedDraws.front().Ticket, incomingColor);
        ASSERT_EQ(plan.ShadowDraws.size(), 1u);
        EXPECT_EQ(plan.ShadowDraws.front().Draw.Ticket, incomingShadow);
        EXPECT_EQ(plan.ShadowDraws.front().CascadeMask, 4u);
    }
};

TEST_F(ForestPlacementShadowTest, SinglePageCanDrawOnlyToSunCascades)
{
    auto runtime = OpenRow(1, {.DetailSlots = 128, .TransitionSlots = 4, .ReadsPerFrame = 4, .ShadowSlots = 16});
    ASSERT_TRUE(runtime);
    ASSERT_EQ(runtime->SlotCount(), 1u);

    constexpr std::array<ForestShadowPage, 1> shadowDemand{{{0, 14}}};
    ForestPlacementFrame plan;
    ASSERT_TRUE(runtime->Prepare(Blind(), 1, 0, shadowDemand, plan));
    EXPECT_EQ(plan.VisiblePages, 0u);
    EXPECT_FALSE(plan.WantsDetails);
    EXPECT_TRUE(plan.DetailedDraws.empty());
    ASSERT_EQ(plan.Reads.size(), 1u);
    const auto ticket = plan.Reads.front();
    EXPECT_EQ(ticket.Page, 0u);
    ASSERT_TRUE(Publish(*runtime, ticket));

    ASSERT_TRUE(runtime->Prepare(Blind(), 2, 1, shadowDemand, plan));
    EXPECT_TRUE(plan.DetailedDraws.empty());
    ASSERT_EQ(plan.ShadowDraws.size(), 1u);
    EXPECT_EQ(plan.ShadowDraws.front().Draw.Ticket, ticket);
    EXPECT_EQ(plan.ShadowDraws.front().Draw.FirstPlacement, ticket.Slot * PackedPlacementStore::kPlacementsPerPage);
    EXPECT_EQ(plan.ShadowDraws.front().Draw.PlacementCount, 1u);
    EXPECT_EQ(plan.ShadowDraws.front().CascadeMask, 14u);
}

TEST_F(ForestPlacementShadowTest, ShadowReplacementKeepsTheCameraCutReserve)
{
    // The compat tier shape, with a transition slot.
    ExpectCameraCutKeepsItsSlots({.DetailSlots = 1, .TransitionSlots = 1, .ReadsPerFrame = 1, .ShadowSlots = 1});
    // The default tier shape, without transition slots.
    ExpectCameraCutKeepsItsSlots({.DetailSlots = 2, .TransitionSlots = 0, .ReadsPerFrame = 1, .ShadowSlots = 1});
}

TEST_F(ForestPlacementShadowTest, ShadowOnlyPagesNeverExceedShadowSlots)
{
    auto runtime = OpenRow(8, {.DetailSlots = 2, .ReadsPerFrame = 2, .ShadowSlots = 2});
    ASSERT_TRUE(runtime);
    ASSERT_EQ(runtime->SlotCount(), 4u);
    constexpr std::array<ForestShadowPage, 4> firstLight{{{0, 1}, {1, 1}, {2, 1}, {3, 1}}};
    // Page 4 comes first in the next light, so page 1 falls past the cap.
    constexpr std::array<ForestShadowPage, 3> nextLight{{{4, 1}, {0, 1}, {1, 1}}};
    ForestPlacementFrame plan;
    ASSERT_TRUE(runtime->Prepare(Blind(), 1, 0, firstLight, plan));
    ASSERT_EQ(plan.Reads.size(), 2u);
    const auto first = plan.Reads;
    EXPECT_EQ(first[0].Page, 0u);
    EXPECT_EQ(first[1].Page, 1u);
    for (const auto ticket : first)
        ASSERT_TRUE(Publish(*runtime, ticket));

    // Pages past the cap wait, although two slots are free.
    ASSERT_TRUE(runtime->Prepare(Blind(), 2, 1, firstLight, plan));
    EXPECT_TRUE(plan.Reads.empty());
    EXPECT_EQ(plan.ShadowDraws.size(), 2u);

    // Page 1 retires but holds its slot until its draw completes; then page 4 takes that slot.
    ASSERT_TRUE(runtime->Prepare(Blind(), 3, 1, nextLight, plan));
    EXPECT_TRUE(plan.Reads.empty());
    ASSERT_EQ(plan.ShadowDraws.size(), 1u);
    EXPECT_EQ(plan.ShadowDraws.front().Draw.Ticket, first[0]);
    ASSERT_TRUE(runtime->Prepare(Blind(), 4, 2, nextLight, plan));
    ASSERT_EQ(plan.Reads.size(), 1u);
    EXPECT_EQ(plan.Reads.front().Page, 4u);
    EXPECT_EQ(plan.Reads.front().Slot, first[1].Slot);
}

TEST_F(ForestPlacementShadowTest, APageInTheFrustumAndACascadeTakesOneSlot)
{
    auto runtime = OpenRow(2, {.DetailSlots = 1, .ReadsPerFrame = 1, .ShadowSlots = 1});
    ASSERT_TRUE(runtime);
    ASSERT_EQ(runtime->SlotCount(), 2u);
    // The camera draws page 0. A page that the camera draws does not count against the shadow cap.
    constexpr std::array<ForestShadowPage, 2> light{{{0, 1}, {1, 2}}};
    ForestPlacementFrame plan;
    ASSERT_TRUE(runtime->Prepare(Camera(0, false), 1, 0, light, plan));
    ASSERT_EQ(plan.Reads.size(), 1u);
    const auto both = plan.Reads.front();
    ASSERT_EQ(both.Page, 0u);
    ASSERT_TRUE(Publish(*runtime, both));
    ASSERT_TRUE(runtime->Prepare(Camera(0, false), 2, 1, light, plan));
    ASSERT_EQ(plan.Reads.size(), 1u);
    const auto shadowOnly = plan.Reads.front();
    ASSERT_EQ(shadowOnly.Page, 1u);
    ASSERT_TRUE(Publish(*runtime, shadowOnly));

    ASSERT_TRUE(runtime->Prepare(Camera(0, false), 3, 2, light, plan));
    EXPECT_TRUE(plan.Reads.empty());
    ASSERT_EQ(plan.DetailedDraws.size(), 1u);
    EXPECT_EQ(plan.DetailedDraws.front().Ticket, both);
    ASSERT_EQ(plan.ShadowDraws.size(), 2u);
    const auto draw = [&plan](uint32 page)
    {
        return *std::ranges::find(plan.ShadowDraws, page,
                                  [](const ForestShadowDraw& shadow) { return shadow.Draw.Ticket.Page; });
    };
    EXPECT_EQ(draw(0).Draw.Ticket, both);
    EXPECT_EQ(draw(0).CascadeMask, 1u);
    EXPECT_EQ(draw(1).Draw.Ticket, shadowOnly);
    EXPECT_EQ(draw(1).CascadeMask, 2u);
}

TEST_F(ForestPlacementShadowTest, ShadowPagesBeyondDetailDistanceAreNotRead)
{
    auto runtime = OpenRow(4, {.DetailSlots = 4, .ReadsPerFrame = 4, .ShadowSlots = 4});
    ASSERT_TRUE(runtime);
    // Cascade 0 sees the pages from 50 m past the eye, cascade 1 sees every page.
    std::array<ForestPlacementView, 2> cascades{};
    cascades[0].Eye = kRowOrigin;
    cascades[0].EyeRelativePlanes[0] = {1, 0, 0, -50};
    cascades[1].Eye = kRowOrigin;
    const auto pages = runtime->GatherShadowPages(cascades, kRowOrigin, 120.0f);
    // Page 3 starts 163 m from the eye.
    const std::vector<std::pair<uint32, uint32>> expected{{1, 3}, {2, 3}, {0, 2}};
    std::vector<std::pair<uint32, uint32>> actual;
    for (const ForestShadowPage& page : pages)
        actual.emplace_back(page.Page, page.CascadeMask);
    EXPECT_EQ(actual, expected);

    const std::vector<ForestShadowPage> demand(pages.begin(), pages.end());
    ForestPlacementFrame plan;
    ASSERT_TRUE(runtime->Prepare(Blind(), 1, 0, demand, plan));
    std::vector<uint32> read;
    for (const auto ticket : plan.Reads)
        read.push_back(ticket.Page);
    EXPECT_EQ(read, (std::vector<uint32>{1, 2, 0}));
    EXPECT_TRUE(runtime->GatherShadowPages(cascades, kRowOrigin, 0.0f).empty());
}

TEST_F(ForestPlacementShadowTest, ACascadeDrawFencesItsSlot)
{
    auto runtime = OpenRow(2, {.DetailSlots = 1, .ReadsPerFrame = 1, .ShadowSlots = 1});
    ASSERT_TRUE(runtime);
    constexpr std::array<ForestShadowPage, 1> oldLight{{{0, 1}}};
    constexpr std::array<ForestShadowPage, 1> newLight{{{1, 1}}};
    ForestPlacementFrame plan;
    ASSERT_TRUE(runtime->Prepare(Blind(), 1, 0, oldLight, plan));
    ASSERT_EQ(plan.Reads.size(), 1u);
    const auto drawn = plan.Reads.front();
    ASSERT_TRUE(Publish(*runtime, drawn));
    ASSERT_TRUE(runtime->Prepare(Blind(), 2, 1, oldLight, plan));
    ASSERT_EQ(plan.ShadowDraws.size(), 1u);

    // Frame 2 drew the slot. Until frame 2 is complete, no page takes it.
    ASSERT_TRUE(runtime->Prepare(Blind(), 3, 1, newLight, plan));
    EXPECT_TRUE(plan.Reads.empty());
    ASSERT_TRUE(runtime->Prepare(Blind(), 4, 2, newLight, plan));
    ASSERT_EQ(plan.Reads.size(), 1u);
    EXPECT_EQ(plan.Reads.front().Page, 1u);
    EXPECT_EQ(plan.Reads.front().Slot, drawn.Slot);
}

TEST_F(ForestPlacementShadowTest, PrefetchKeepsOnlyTheUnusedShadowSlots)
{
    auto runtime = OpenRow(4, {.DetailSlots = 1, .ReadsPerFrame = 1, .ShadowSlots = 1});
    ASSERT_TRUE(runtime);
    ASSERT_EQ(runtime->SlotCount(), 2u);
    ForestPlacementFrame plan;
    ASSERT_TRUE(runtime->Prepare(Camera(0, false), 1, 0, {}, plan));
    ASSERT_EQ(plan.Reads.size(), 1u);
    ASSERT_TRUE(Publish(*runtime, plan.Reads.front()));
    ASSERT_TRUE(runtime->Prepare(Camera(0, false), 2, 1, {}, plan));
    ASSERT_EQ(plan.DetailedDraws.size(), 1u);

    // No shadow page holds a slot: the free slot stays for the cascades.
    ASSERT_TRUE(runtime->Prepare(Above(2), 3, 1, {}, plan));
    EXPECT_TRUE(plan.Reads.empty());
    ASSERT_TRUE(runtime->Prepare(Above(2), 4, 2, {}, plan));
    ASSERT_EQ(plan.Reads.size(), 1u);
    EXPECT_EQ(plan.Reads.front().Page, 2u);
    ASSERT_TRUE(Publish(*runtime, plan.Reads.front()));

    constexpr std::array<ForestShadowPage, 1> light{{{3, 1}}};
    ASSERT_TRUE(runtime->Prepare(Above(2), 5, 3, light, plan));
    ASSERT_EQ(plan.Reads.size(), 1u);
    EXPECT_EQ(plan.Reads.front().Page, 3u);
    ASSERT_TRUE(Publish(*runtime, plan.Reads.front()));

    // The shadow page holds its slot, so prefetch takes the free slot.
    ASSERT_TRUE(runtime->Prepare(Above(1), 6, 4, light, plan));
    ASSERT_EQ(plan.Reads.size(), 1u);
    EXPECT_EQ(plan.Reads.front().Page, 1u);
}

TEST_F(ForestPlacementShadowTest, AColorReadIntoARetiredShadowSlotLeavesRoomForAShadowRead)
{
    auto runtime = OpenRow(5, {.DetailSlots = 2, .ReadsPerFrame = 2, .ShadowSlots = 1});
    ASSERT_TRUE(runtime);
    ASSERT_EQ(runtime->SlotCount(), 3u);
    constexpr std::array<ForestShadowPage, 1> oldLight{{{1, 1}}};
    constexpr std::array<ForestShadowPage, 1> newLight{{{2, 1}}};
    ForestPlacementFrame plan;
    ASSERT_TRUE(runtime->Prepare(Blind(), 1, 0, oldLight, plan));
    ASSERT_EQ(plan.Reads.size(), 1u);
    const auto oldShadow = plan.Reads.front();
    ASSERT_TRUE(Publish(*runtime, oldShadow));
    ASSERT_TRUE(runtime->Prepare(Camera(0, false), 2, 1, oldLight, plan));
    ASSERT_EQ(plan.ShadowDraws.size(), 1u);
    ASSERT_EQ(plan.Reads.size(), 1u);
    const auto oldColor = plan.Reads.front();
    ASSERT_TRUE(Publish(*runtime, oldColor));
    ASSERT_TRUE(runtime->Prepare(Camera(0, false), 3, 2, {}, plan));
    ASSERT_EQ(plan.DetailedDraws.size(), 1u);
    ASSERT_TRUE(runtime->Prepare(Camera(4, true), 4, 2, {}, plan));
    ASSERT_EQ(plan.Reads.size(), 1u);
    ASSERT_TRUE(Publish(*runtime, plan.Reads.front()));
    ASSERT_TRUE(runtime->Prepare(Camera(4, true), 5, 3, {}, plan));
    ASSERT_EQ(plan.DetailedDraws.size(), 1u);

    // No slot is free. The color read takes the oldest retired slot, the shadow page's, and the
    // new shadow page then takes the retired color slot in the same frame.
    ASSERT_TRUE(runtime->Prepare(Camera(3, true), 6, 4, newLight, plan));
    ASSERT_EQ(plan.Reads.size(), 2u);
    EXPECT_EQ(plan.Reads[0].Page, 3u);
    EXPECT_EQ(plan.Reads[0].Slot, oldShadow.Slot);
    EXPECT_EQ(plan.Reads[1].Page, 2u);
    EXPECT_EQ(plan.Reads[1].Slot, oldColor.Slot);
}

TEST_F(ForestPlacementShadowTest, APageWaitingForARetryHoldsNoShadowPlace)
{
    auto runtime = OpenRow(3, {.DetailSlots = 1, .ReadsPerFrame = 1, .ShadowSlots = 1});
    ASSERT_TRUE(runtime);
    constexpr std::array<ForestShadowPage, 2> light{{{0, 1}, {1, 1}}};
    ForestPlacementFrame plan;
    // Page 0 fails twice, so it waits two frames for its next read.
    for (uint64 frame = 1; frame <= 2; ++frame)
    {
        ASSERT_TRUE(runtime->Prepare(Blind(), frame, frame - 1, light, plan));
        ASSERT_EQ(plan.Reads.size(), 1u);
        ASSERT_EQ(plan.Reads.front().Page, 0u);
        EXPECT_FALSE(runtime->CompleteRead(plan.Reads.front(), {}));
    }
    ASSERT_TRUE(runtime->Prepare(Blind(), 3, 2, light, plan));
    ASSERT_EQ(plan.Reads.size(), 1u);
    EXPECT_EQ(plan.Reads.front().Page, 1u);
}

} // namespace
} // namespace GameEngine::PageStreaming
