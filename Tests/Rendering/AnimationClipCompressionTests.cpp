#include "Animation/AnimationClip.h"
#include "Animation/AnimationPose.h"
#include "Animation/Compression/AnimationClipArchive.h"
#include "Animation/Compression/AnimationClipData.h"
#include "Animation/EvaluationContext.h"
#include "Animation/Nodes/ClipPlayerNode.h"
#include "Assets/Parsers/AnimationAssetParser.h"
#include "JobSystem/JobCounter.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "TestTempDir.h"

#include <atomic>
#include <fstream>
#include <iterator>
#include <limits>

#include <gtest/gtest.h>

namespace GameEngine::Animation
{
namespace
{
SharedPtr<AnimationClip> MakeClip()
{
    auto clip = MakeShared<AnimationClip>(GUID::Generate(), "compression.anim");
    AnimChannel track{};
    track.path = AnimPath::Translation;
    track.keys.resize(2u);
    track.keys[1].time = 1.0f;
    track.keys[1].translation[0] = 2.0f;
    for (auto& key : track.keys)
    {
        key.rotation[3] = 1.0f;
        key.scale[0] = key.scale[1] = key.scale[2] = 1.0f;
        for (uint32 component = 0u; component < 4u; ++component)
            key.outWeight[component] = key.inWeight[component] = 1.0f / 3.0f;
    }
    track.keys[0].hasSegmentInterpOverride = true;
    track.keys[0].segmentInterp = AnimInterp::CubicSpline;
    track.keys[1].inTangent[0] = 4.0f;
    clip->SetChannelsAndDurationForTest({track}, 1.0f);
    return clip;
}

void CheckAuthoringView(const AnimationClip& clip, std::atomic<const AnimChannel*>& first,
    std::atomic<bool>& consistent)
{
    const auto& channels = clip.GetChannels();
    if (channels.size() != 1u || channels[0].keys.size() != 2u)
    {
        consistent = false;
        return;
    }
    const AnimChannel* expected = nullptr;
    if (!first.compare_exchange_strong(expected, channels.data()) && expected != channels.data())
        consistent = false;
}
}

TEST(AnimationClipCompression, GraphUsesSegmentOverridesWithoutExpandingAuthoringKeys)
{
    const auto clip = MakeClip();
    const size_t residentBytes = clip->GetMemoryUsage();
    ClipPlayerNode player;
    player.SetClip(clip);
    player.SetTime(0.5f);
    EvaluationContext context;
    AnimationPose pose;
    player.Evaluate(context, pose);
    ASSERT_EQ(pose.BoneCount, 1u);
    EXPECT_NEAR(pose.Positions[0].x, 0.5f, 1e-6f);
    EXPECT_EQ(clip->GetMemoryUsage(), residentBytes);
}

TEST(AnimationClipCompression, EditsPublishNewSnapshotsAndUndoRestoresOriginalAuthoring)
{
    const auto clip = MakeClip();
    const auto original = clip->GetRuntimeData();
    ASSERT_TRUE(original);
    Vector<uint8> saved;
    ASSERT_TRUE(clip->SaveToData(saved));
    ASSERT_TRUE(clip->SetKeyframeComponentValue(0u, 1.0f, 0u, 8.0f));
    const auto edited = clip->GetRuntimeData();
    ASSERT_TRUE(edited);
    EXPECT_NE(edited->Revision(), original->Revision());
    Mathematics::Vector4 value;
    ASSERT_TRUE(original->Sample(original->Tracks()[0], 0.5f, value));
    EXPECT_NEAR(value.x, 0.5f, 1e-6f);
    ASSERT_TRUE(edited->Sample(edited->Tracks()[0], 0.5f, value));
    EXPECT_NEAR(value.x, 3.5f, 1e-6f);
    ASSERT_TRUE(clip->RestoreFromData(saved));
    Vector<uint8> restored;
    ASSERT_TRUE(clip->SaveToData(restored));
    EXPECT_EQ(saved, restored);
    auto copy = MakeClip();
    copy->CopyFrom(*clip);
    EXPECT_EQ(copy->GetRuntimeData(), clip->GetRuntimeData());
    clip->Unload();
    EXPECT_FALSE(clip->GetRuntimeData());
    ASSERT_TRUE(copy->GetRuntimeData()->Sample(copy->GetRuntimeData()->Tracks()[0], 0.5f, value));
    EXPECT_NEAR(value.x, 0.5f, 1e-6f);
}

TEST(AnimationClipCompression, FailedLoadsAndRebuildsRetainThePublishedClip)
{
    const auto clip = MakeClip();
    const auto original = clip->GetRuntimeData();
    const Vector<uint8> invalid{'{', '?'};
    EXPECT_FALSE(clip->LoadFromData(invalid));
    EXPECT_FALSE(clip->RestoreFromData(invalid));
    EXPECT_EQ(clip->GetRuntimeData(), original);
    EXPECT_FALSE(clip->SetChannelInterpolation(0u, static_cast<AnimInterp>(99u)));
    EXPECT_EQ(clip->GetRuntimeData(), original);
    EXPECT_EQ(clip->GetChannels()[0].interp, AnimInterp::Linear);
    clip->SetDuration(std::numeric_limits<float>::infinity());
    EXPECT_FLOAT_EQ(clip->GetDuration(), 1.0f);
    EXPECT_EQ(clip->GetRuntimeData(), original);
}

TEST(AnimationClipCompression, ConcurrentFirstAuthoringReadsShareOneMaterializedView)
{
    const auto clip = MakeClip();
    JobSystem::WorkStealingThreadPool pool(4u);
    JobSystem::JobCounter counter;
    std::atomic<const AnimChannel*> first{nullptr};
    std::atomic<bool> consistent{true};
    for (uint32 job = 0u; job < 64u; ++job)
    {
        pool.Run([&clip, &first, &consistent] { CheckAuthoringView(*clip, first, consistent); }, counter);
    }
    pool.Wait(counter);
    EXPECT_TRUE(consistent.load());
}

TEST(AnimationClipCompression, FailedInitialRuntimeCompilationSetsFailedState)
{
    const String source = R"({"channels":[{"path":"Translation","keys":[{"time":1e40}]}]})";
    const Vector<uint8> bytes(source.begin(), source.end());
    AnimationClip clip(GUID::Generate(), "invalid.anim");
    EXPECT_FALSE(clip.LoadFromData(bytes));
    EXPECT_EQ(clip.GetState(), AssetState::Failed);
    EXPECT_FALSE(clip.GetRuntimeData());
}

TEST(AnimationClipCompression, PackagedSourceLoadsAndCookIsIdempotent)
{
    const TestUtils::ScopedTempDir directory(TestUtils::MakeUniqueTempDirectory("animation-compression"));
    const auto path = directory.Path() / "clip.anim";
    const auto clip = MakeClip();
    ASSERT_TRUE(clip->SaveToPath(path));
    AnimationAssetParser parser;
    String error;
    ASSERT_TRUE(parser.CookForPackage(path, error)) << error;
    std::ifstream input(path, std::ios::binary);
    const Vector<uint8> bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    input.close();
    ASSERT_TRUE(AnimationClipArchive::IsArchive(bytes));
    AnimationClip cooked(GUID::Generate(), path);
    ASSERT_TRUE(cooked.LoadFromData(bytes));
    const auto data = cooked.GetRuntimeData();
    ASSERT_TRUE(data);
    Mathematics::Vector4 value;
    ASSERT_TRUE(data->Sample(data->Tracks()[0], 0.5f, value));
    EXPECT_NEAR(value.x, 0.5f, 1e-6f);
    ASSERT_TRUE(parser.CookForPackage(path, error)) << error;
    std::ifstream second(path, std::ios::binary);
    const Vector<uint8> recooked((std::istreambuf_iterator<char>(second)), std::istreambuf_iterator<char>());
    EXPECT_EQ(bytes, recooked);
}

TEST(AnimationClipCompression, OversizedPackageRefusesBeforeChangingTheStagedFile)
{
    const TestUtils::ScopedTempDir directory(TestUtils::MakeUniqueTempDirectory("animation-compression-bounds"));
    const auto path = directory.Path() / "clip.anim";
    std::ofstream(path, std::ios::binary).put('{');
    const size_t oversizedBytes = AnimationClipArchive::MaximumEncodedBytes() + 1u;
    std::filesystem::resize_file(path, oversizedBytes);
    AnimationAssetParser parser;
    String error;
    EXPECT_FALSE(parser.CookForPackage(path, error));
    EXPECT_NE(error.find("256 MiB source limit"), String::npos);
    EXPECT_EQ(std::filesystem::file_size(path), oversizedBytes);
    EXPECT_GT(AnimationClipArchive::MaximumEncodedBytes(), AnimationClipArchive::kMaximumSourceBytes);
}

} // namespace GameEngine::Animation
