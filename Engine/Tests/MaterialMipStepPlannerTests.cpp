// The step order of material mip streaming: which texture gains or loses a level under a budget.

#include "Engine/Rendering/MaterialMipStreaming/MaterialMipStepPlanner.h"

#include <gtest/gtest.h>

#include <array>
#include <string>
#include <vector>

using namespace GameEngine;
using GameEngine::Engine::Renderer::MaterialMipStepPlanner;

namespace
{

constexpr uint64 kBytesPerTexel = 4;

// A square texture's sizes. Its tail is the level with a 256 texel edge.
struct Texture
{
    std::vector<uint64> SuffixBytes;
    uint32 TopExtent = 0;
    uint32 Tail = 0;

    explicit Texture(uint32 topExtent)
        : TopExtent(topExtent)
    {
        std::vector<uint64> levelBytes;
        for (uint32 extent = topExtent; extent >= 1; extent /= 2)
        {
            if (extent == 256)
                Tail = static_cast<uint32>(levelBytes.size());
            levelBytes.push_back(static_cast<uint64>(extent) * extent * kBytesPerTexel);
        }
        SuffixBytes.resize(levelBytes.size());
        uint64 bytes = 0;
        for (size_t level = levelBytes.size(); level-- > 0;)
        {
            bytes += levelBytes[level];
            SuffixBytes[level] = bytes;
        }
    }

    MaterialMipStepPlanner::Stream At(uint32 firstMip, float32 coverage, const char* id) const
    {
        MaterialMipStepPlanner::Stream stream;
        stream.Id = GUID(std::string(id));
        stream.SuffixBytes = SuffixBytes;
        stream.FirstMip = firstMip;
        stream.Tail = Tail;
        stream.TopExtent = TopExtent;
        stream.ScreenCoverage = coverage;
        return stream;
    }
};

constexpr const char* kIdA = "00000000-0000-0000-0000-00000000000a";
constexpr const char* kIdB = "00000000-0000-0000-0000-00000000000b";
constexpr const char* kIdC = "00000000-0000-0000-0000-00000000000c";

uint64 Used(std::span<const MaterialMipStepPlanner::Stream> streams)
{
    uint64 bytes = 0;
    for (const auto& stream : streams)
        bytes += stream.SuffixBytes[stream.FirstMip];
    return bytes;
}

} // namespace

TEST(MaterialMipStepPlanner, TheStreamWithTheMostNeedGainsFirst)
{
    const Texture texture(1024);
    std::array streams{texture.At(texture.Tail, 0.1f, kIdA), texture.At(texture.Tail, 0.8f, kIdB)};
    const uint64 oneGain = texture.SuffixBytes[texture.Tail - 1] - texture.SuffixBytes[texture.Tail];

    MaterialMipStepPlanner planner;
    const auto step = planner.Plan(streams, Used(streams) + oneGain);

    ASSERT_TRUE(step.has_value());
    EXPECT_EQ(step->Stream, 1u);
    EXPECT_EQ(step->TargetMip, texture.Tail - 1);
}

TEST(MaterialMipStepPlanner, NeedCountsResidentTexelsNotOnlyCoverage)
{
    // Equal coverage: the stream with fewer resident texels is the less sharp one on screen.
    const Texture texture(1024);
    std::array streams{texture.At(texture.Tail - 1, 0.5f, kIdA), texture.At(texture.Tail, 0.5f, kIdB)};

    MaterialMipStepPlanner planner;
    const auto step = planner.Plan(streams, Used(streams) * 4);

    ASSERT_TRUE(step.has_value());
    EXPECT_EQ(step->Stream, 1u);
}

TEST(MaterialMipStepPlanner, OverTheBudgetTheStreamWithTheLeastNeedLosesFirst)
{
    const Texture texture(1024);
    std::array streams{texture.At(0, 0.9f, kIdA), texture.At(0, 0.05f, kIdB), texture.At(0, 0.4f, kIdC)};

    MaterialMipStepPlanner planner;
    const auto step = planner.Plan(streams, Used(streams) - 1);

    ASSERT_TRUE(step.has_value());
    EXPECT_EQ(step->Stream, 1u);
    EXPECT_EQ(step->TargetMip, 1u);
}

TEST(MaterialMipStepPlanner, ALevelThatDoesNotFitTakesBytesFromAStreamWithLessThanHalfItsNeed)
{
    const Texture texture(1024);
    // B is at level 1 with 4 times the need of A at level 0: A's level 0 pays for B's.
    std::array streams{texture.At(0, 0.1f, kIdA), texture.At(1, 0.2f, kIdB)};

    MaterialMipStepPlanner planner;
    const auto step = planner.Plan(streams, Used(streams));

    ASSERT_TRUE(step.has_value());
    EXPECT_EQ(step->Stream, 0u);
    EXPECT_EQ(step->TargetMip, 1u);
}

TEST(MaterialMipStepPlanner, AStreamWithHalfTheNeedOrMoreKeepsItsLevel)
{
    const Texture texture(1024);
    // B at level 1 has exactly twice the need of A at level 0: after a trade A would need as much.
    std::array streams{texture.At(0, 0.1f, kIdA), texture.At(1, 0.1f, kIdB)};

    MaterialMipStepPlanner planner;
    EXPECT_FALSE(planner.Plan(streams, Used(streams)).has_value());
}

TEST(MaterialMipStepPlanner, DonorsThatFreeTooLittleKeepTheirLevels)
{
    const Texture large(2048);
    const Texture small(512);
    // The small stream's level 0 frees less than the large stream's level 0 needs.
    std::array streams{small.At(0, 0.001f, kIdA), large.At(1, 0.9f, kIdB)};

    MaterialMipStepPlanner planner;
    EXPECT_FALSE(planner.Plan(streams, Used(streams)).has_value());
}

TEST(MaterialMipStepPlanner, ATradeSettlesAndIsNotTradedBack)
{
    const Texture texture(1024);
    std::array streams{texture.At(0, 0.02f, kIdA), texture.At(texture.Tail, 0.9f, kIdB)};
    const uint64 budget = Used(streams);

    MaterialMipStepPlanner planner;
    uint32 steps = 0;
    constexpr uint32 kStepLimit = 16;
    for (; steps < kStepLimit; ++steps)
    {
        const auto step = planner.Plan(streams, budget);
        if (!step)
            break;
        streams[step->Stream].FirstMip = step->TargetMip;
    }

    ASSERT_LT(steps, kStepLimit);
    EXPECT_EQ(streams[1].FirstMip, 0u);
    EXPECT_EQ(streams[0].FirstMip, texture.Tail);
    EXPECT_LE(Used(streams), budget);
}

TEST(MaterialMipStepPlanner, WithoutDemandTheSmallestGainGoesFirstThenTheLowestId)
{
    const Texture large(2048);
    const Texture small(1024);
    std::array streams{large.At(1, 0.0f, kIdA), small.At(1, 0.0f, kIdC), small.At(1, 0.0f, kIdB)};

    MaterialMipStepPlanner planner;
    const auto step = planner.Plan(streams, Used(streams) * 4);

    ASSERT_TRUE(step.has_value());
    EXPECT_EQ(step->Stream, 2u);
    EXPECT_EQ(step->TargetMip, 0u);
}

TEST(MaterialMipStepPlanner, WithoutDemandNoStreamGivesUpALevelForAnother)
{
    const Texture texture(1024);
    std::array streams{texture.At(0, 0.0f, kIdA), texture.At(1, 0.0f, kIdB)};

    MaterialMipStepPlanner planner;
    EXPECT_FALSE(planner.Plan(streams, Used(streams)).has_value());
}

TEST(MaterialMipStepPlanner, TailsStayWhenTheyAloneExceedTheBudget)
{
    const Texture texture(1024);
    std::array streams{texture.At(texture.Tail, 0.5f, kIdA), texture.At(texture.Tail, 0.1f, kIdB)};

    MaterialMipStepPlanner planner;
    EXPECT_FALSE(planner.Plan(streams, 1).has_value());
}

TEST(MaterialMipStepPlanner, ASettledSetHasNoStep)
{
    const Texture texture(1024);
    std::array streams{texture.At(0, 0.5f, kIdA), texture.At(0, 0.1f, kIdB)};

    MaterialMipStepPlanner planner;
    EXPECT_FALSE(planner.Plan(streams, Used(streams)).has_value());
    EXPECT_FALSE(planner.Plan({}, 0).has_value());
}
