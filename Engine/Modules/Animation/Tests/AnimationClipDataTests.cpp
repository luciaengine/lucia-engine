#include "Animation/Compression/AnimationClipArchive.h"
#include "Animation/Compression/AnimationClipData.h"

#include <bit>
#include <cmath>
#include <limits>

#include <gtest/gtest.h>

namespace GameEngine::Animation
{
namespace
{
AnimKeyframe Key(float time, float value)
{
    AnimKeyframe key{};
    key.time = time;
    key.translation[0] = value;
    key.rotation[3] = 1.0f;
    key.scale[0] = key.scale[1] = key.scale[2] = 1.0f;
    return key;
}

AnimChannel Track(AnimPath path, AnimInterp interpolation = AnimInterp::Linear)
{
    AnimChannel track{};
    track.path = path;
    track.interp = interpolation;
    track.targetName = "Root/Spine";
    track.targetNameId = HashStringId(track.targetName);
    track.keys = {Key(0.0f, 0.0f), Key(1.0f, 2.0f)};
    return track;
}

void ExpectSameFloats(std::span<const float> first, std::span<const float> second)
{
    ASSERT_EQ(first.size(), second.size());
    for (size_t index = 0u; index < first.size(); ++index)
        EXPECT_EQ(std::bit_cast<uint32>(first[index]), std::bit_cast<uint32>(second[index]));
}
}

TEST(AnimationClipData, AuthoringArchivePreservesEveryFieldAndSignedZero)
{
    auto track = Track(AnimPath::Translation);
    track.preInfinity = AnimExtrapolation::CycleWithOffset;
    track.postInfinity = AnimExtrapolation::Oscillate;
    for (auto& key : track.keys)
    {
        key.translation[2] = -0.0f;
        key.rotation[2] = 0.36f;
        key.scale[1] = 3.25f;
        key.tangentBroken = 0xa5u;
        key.segmentInterp = AnimInterp::Step;
        key.hasSegmentInterpOverride = true;
        for (uint32 component = 0u; component < 4u; ++component)
        {
            key.inTangent[component] = static_cast<float>(component) + 0.25f;
            key.outTangent[component] = -static_cast<float>(component) - 0.75f;
            key.inWeight[component] = 0.1f + static_cast<float>(component) * 0.05f;
            key.outWeight[component] = 0.4f - static_cast<float>(component) * 0.05f;
            key.inTangentType[component] = static_cast<AnimTangentType>(component);
            key.outTangentType[component] = static_cast<AnimTangentType>(6u - component);
        }
    }
    const auto data = AnimationClipData::Build(std::span(&track, 1u));
    ASSERT_TRUE(data);
    std::vector<AnimChannel> restored;
    ASSERT_TRUE(data->DecodeChannels(restored));
    ASSERT_EQ(restored.size(), 1u);
    EXPECT_EQ(restored[0].targetName, track.targetName);
    EXPECT_EQ(restored[0].targetNameId, track.targetNameId);
    EXPECT_EQ(restored[0].preInfinity, track.preInfinity);
    EXPECT_EQ(restored[0].postInfinity, track.postInfinity);
    for (size_t index = 0u; index < track.keys.size(); ++index)
    {
        const auto& first = track.keys[index];
        const auto& second = restored[0].keys[index];
        ExpectSameFloats(first.translation, second.translation);
        ExpectSameFloats(first.rotation, second.rotation);
        ExpectSameFloats(first.scale, second.scale);
        ExpectSameFloats(first.inTangent, second.inTangent);
        ExpectSameFloats(first.outTangent, second.outTangent);
        ExpectSameFloats(first.inWeight, second.inWeight);
        ExpectSameFloats(first.outWeight, second.outWeight);
        EXPECT_EQ(first.tangentBroken, second.tangentBroken);
        EXPECT_EQ(first.segmentInterp, second.segmentInterp);
        EXPECT_EQ(first.hasSegmentInterpOverride, second.hasSegmentInterpOverride);
        for (uint32 component = 0u; component < 4u; ++component)
        {
            EXPECT_EQ(first.inTangentType[component], second.inTangentType[component]);
            EXPECT_EQ(first.outTangentType[component], second.outTangentType[component]);
        }
    }
}

TEST(AnimationClipData, LinearTracksStoreOnlyTimeFlagsAndTheActiveValues)
{
    const std::vector<AnimChannel> tracks{Track(AnimPath::Translation), Track(AnimPath::Rotation),
        Track(AnimPath::Scale), Track(AnimPath::MorphWeight)};
    const auto data = AnimationClipData::Build(tracks);
    ASSERT_TRUE(data);
    ASSERT_EQ(data->Tracks().size(), 4u);
    EXPECT_EQ(data->Tracks()[0].WordsPerKey, 5u);
    EXPECT_EQ(data->Tracks()[1].WordsPerKey, 6u);
    EXPECT_EQ(data->Tracks()[2].WordsPerKey, 5u);
    EXPECT_EQ(data->Tracks()[3].WordsPerKey, 3u);
    EXPECT_EQ(data->Words().size(), 38u);
}

TEST(AnimationClipData, MixedOverridesKeepCubicDataAndClampStepEndpoints)
{
    auto track = Track(AnimPath::Translation);
    track.keys[0].segmentInterp = AnimInterp::CubicSpline;
    track.keys[0].hasSegmentInterpOverride = true;
    track.keys[0].outTangent[0] = 0.0f;
    track.keys[0].outWeight[0] = 1.0f / 3.0f;
    track.keys[1].inTangent[0] = 4.0f;
    track.keys[1].inWeight[0] = 1.0f / 3.0f;
    auto data = AnimationClipData::Build(std::span(&track, 1u));
    ASSERT_TRUE(data);
    EXPECT_TRUE(data->Tracks()[0].HasCubic);
    Mathematics::Vector4 value;
    ASSERT_TRUE(data->Sample(data->Tracks()[0], 0.5f, value));
    EXPECT_NEAR(value.x, 0.5f, 1e-6f);
    track.interp = AnimInterp::Step;
    track.keys[0].hasSegmentInterpOverride = false;
    data = AnimationClipData::Build(std::span(&track, 1u));
    ASSERT_TRUE(data->Sample(data->Tracks()[0], -1.0f, value));
    EXPECT_FLOAT_EQ(value.x, 0.0f);
    ASSERT_TRUE(data->Sample(data->Tracks()[0], 1.5f, value));
    EXPECT_FLOAT_EQ(value.x, 2.0f);
}

TEST(AnimationClipData, DuplicateTimesKeepTheirOriginalLowerBoundOrder)
{
    auto track = Track(AnimPath::Translation);
    track.keys = {Key(0.0f, 1.0f), Key(0.5f, 2.0f), Key(0.5f, 3.0f), Key(1.0f, 4.0f)};
    const auto data = AnimationClipData::Build(std::span(&track, 1u));
    ASSERT_TRUE(data);
    Mathematics::Vector4 value;
    ASSERT_TRUE(data->Sample(data->Tracks()[0], 0.5f, value));
    EXPECT_FLOAT_EQ(value.x, 2.0f);
    ASSERT_TRUE(data->Sample(data->Tracks()[0], 0.5001f, value));
    EXPECT_GT(value.x, 3.0f);
}

TEST(AnimationClipData, ScalarScaleAndQuaternionCubicPayloadsUseTheirOwnComponents)
{
    std::vector<AnimChannel> tracks{Track(AnimPath::MorphWeight, AnimInterp::CubicSpline),
        Track(AnimPath::Scale, AnimInterp::CubicSpline), Track(AnimPath::Rotation, AnimInterp::CubicSpline)};
    tracks[1].keys[0].scale[0] = 0.0f;
    tracks[1].keys[1].scale[0] = 2.0f;
    for (auto& track : tracks)
    {
        track.keys[0].outWeight[0] = track.keys[1].inWeight[0] = 1.0f / 3.0f;
        track.keys[1].inTangent[0] = 4.0f;
    }
    // Constant identity rotation with a non-zero X tangent at the second key.
    // Cubic X is -0.5 before quaternion normalization; W stays 1.
    const auto data = AnimationClipData::Build(tracks);
    ASSERT_TRUE(data);
    Mathematics::Vector4 value;
    for (uint32 track = 0u; track < 2u; ++track)
    {
        ASSERT_TRUE(data->Sample(data->Tracks()[track], 0.5f, value));
        EXPECT_NEAR(value.x, 0.5f, 1e-6f);
    }
    ASSERT_TRUE(data->Sample(data->Tracks()[2u], 0.5f, value));
    EXPECT_NEAR(value.x, -0.5f / std::sqrt(1.25f), 1e-6f);
    EXPECT_NEAR(value.w, 1.0f / std::sqrt(1.25f), 1e-6f);
}

TEST(AnimationClipData, InvalidTimesAndEnumsDoNotPublishData)
{
    auto track = Track(AnimPath::Translation);
    track.keys[0].time = std::numeric_limits<float>::infinity();
    EXPECT_FALSE(AnimationClipData::Build(std::span(&track, 1u)));
    track.keys[0].time = 2.0f;
    EXPECT_FALSE(AnimationClipData::Build(std::span(&track, 1u)));
    track.keys[0].time = 0.0f;
    track.path = static_cast<AnimPath>(55u);
    EXPECT_FALSE(AnimationClipData::Build(std::span(&track, 1u)));
}

TEST(AnimationClipData, RuntimeAndArchivedKeysUseLessMemoryThanExpandedDenseAuthoring)
{
    auto track = Track(AnimPath::Translation);
    track.keys.clear();
    for (uint32 index = 0u; index < 1024u; ++index)
        track.keys.push_back(Key(static_cast<float>(index) / 60.0f, static_cast<float>(index) * 0.01f));
    const auto data = AnimationClipData::Build(std::span(&track, 1u));
    ASSERT_TRUE(data);
    EXPECT_LT(data->GetMemoryUsage(), track.keys.size() * sizeof(AnimKeyframe));
}

TEST(AnimationClipArchive, EmptyAndBinarySourcesRoundTripWithoutPadding)
{
    for (const std::vector<uint8>& source : {std::vector<uint8>{}, std::vector<uint8>{0u, 255u, 13u, 0u}})
    {
        std::vector<uint8> encoded;
        std::vector<uint8> decoded;
        ASSERT_TRUE(AnimationClipArchive::Encode(source, encoded));
        ASSERT_TRUE(AnimationClipArchive::Decode(encoded, decoded));
        EXPECT_EQ(decoded, source);
    }
}

TEST(AnimationClipArchive, MalformedStreamsCannotReplaceAnExistingOutput)
{
    std::vector<uint8> encoded;
    ASSERT_TRUE(AnimationClipArchive::Encode(std::vector<uint8>{1u, 2u, 3u}, encoded));
    const std::vector<uint8> previous{99u};
    for (uint32 mutation = 0u; mutation < 4u; ++mutation)
    {
        auto damaged = encoded;
        if (mutation == 0u) damaged.pop_back();
        if (mutation == 1u) damaged.push_back(0u);
        if (mutation == 2u) damaged[4] = 77u;
        if (mutation == 3u) damaged[11] = 0xffu;
        auto output = previous;
        EXPECT_FALSE(AnimationClipArchive::Decode(damaged, output));
        EXPECT_EQ(output, previous);
    }
}
} // namespace GameEngine::Animation
