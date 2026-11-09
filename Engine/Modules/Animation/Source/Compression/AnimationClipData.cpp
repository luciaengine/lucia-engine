#include "Animation/Compression/AnimationClipData.h"

#include "Animation/Compression/AnimationClipArchive.h"
#include "Mathematics/Quaternion.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>

#include <glm/gtc/quaternion.hpp>

namespace GameEngine::Animation
{
namespace
{
constexpr size_t kAuthoringWordsPerKey = 29u;
constexpr uint32 kOverrideBit = 4u;
std::atomic<uint64> s_NextRevision{1u};

uint32 ComponentCount(AnimPath path)
{
    return path == AnimPath::Rotation ? 4u : path == AnimPath::MorphWeight ? 1u : 3u;
}

const float* Values(const AnimKeyframe& key, AnimPath path)
{
    return path == AnimPath::Rotation ? key.rotation : path == AnimPath::Scale ? key.scale : key.translation;
}

void AppendFloats(std::vector<uint32>& words, std::span<const float> values)
{
    for (float value : values)
        words.push_back(std::bit_cast<uint32>(value));
}

void ReadFloats(std::span<const uint32> words, size_t& cursor, std::span<float> values)
{
    for (float& value : values)
        value = std::bit_cast<float>(words[cursor++]);
}

uint32 AuthoringFlags(const AnimKeyframe& key)
{
    uint32 flags = static_cast<uint32>(key.tangentBroken) << 24u;
    for (uint32 component = 0u; component < 4u; ++component)
    {
        flags |= static_cast<uint32>(key.inTangentType[component]) << (component * 3u);
        flags |= static_cast<uint32>(key.outTangentType[component]) << (12u + component * 3u);
    }
    return flags;
}

void AppendAuthoring(std::vector<uint32>& words, const AnimKeyframe& key)
{
    words.push_back(std::bit_cast<uint32>(key.time));
    AppendFloats(words, key.translation);
    AppendFloats(words, key.rotation);
    AppendFloats(words, key.scale);
    AppendFloats(words, key.inTangent);
    AppendFloats(words, key.outTangent);
    AppendFloats(words, key.inWeight);
    AppendFloats(words, key.outWeight);
    words.push_back(AuthoringFlags(key));
    words.push_back(static_cast<uint32>(key.segmentInterp) | (key.hasSegmentInterpOverride ? kOverrideBit : 0u));
}

bool ValidKey(const AnimKeyframe& key, float previousTime)
{
    if (!std::isfinite(key.time) || key.time < previousTime ||
        static_cast<uint32>(key.segmentInterp) > static_cast<uint32>(AnimInterp::CubicSpline))
        return false;
    for (uint32 component = 0u; component < 4u; ++component)
    {
        if (static_cast<uint32>(key.inTangentType[component]) > static_cast<uint32>(AnimTangentType::Custom) ||
            static_cast<uint32>(key.outTangentType[component]) > static_cast<uint32>(AnimTangentType::Custom))
            return false;
    }
    return true;
}

float ReadFloat(std::span<const uint32> words, size_t offset)
{
    return std::bit_cast<float>(words[offset]);
}

template<uint32 kComponents>
Mathematics::Vector4 ReadVector(std::span<const uint32> words, size_t offset)
{
    Mathematics::Vector4 value;
    for (uint32 component = 0u; component < kComponents; ++component)
        value[component] = ReadFloat(words, offset + component);
    return value;
}

float SampleCubic(float first, float outgoing, float outgoingWeight, float second,
    float incoming, float incomingWeight, float duration, float alpha)
{
    const float firstControl = first + outgoing * (duration * std::clamp(outgoingWeight, 0.05f, 0.49f));
    const float secondControl = second - incoming * (duration * std::clamp(incomingWeight, 0.05f, 0.49f));
    const float inverse = 1.0f - alpha;
    return inverse * inverse * inverse * first + 3.0f * inverse * inverse * alpha * firstControl +
        3.0f * inverse * alpha * alpha * secondControl + alpha * alpha * alpha * second;
}

template<uint32 kComponents>
bool SampleTrack(std::span<const uint32> words, const AnimationClipData::Track& track,
    float32 time, Mathematics::Vector4& value)
{
    if (track.KeyCount == 0u)
        return false;
    uint32 lower = 0u;
    while (lower < track.KeyCount && ReadFloat(words, track.WordOffset + lower * track.WordsPerKey) < time)
        ++lower;
    const uint32 first = lower == 0u ? 0u : lower - 1u;
    const uint32 second = std::min(lower, track.KeyCount - 1u);
    const size_t firstOffset = track.WordOffset + first * track.WordsPerKey;
    const size_t secondOffset = track.WordOffset + second * track.WordsPerKey;
    const float start = ReadFloat(words, firstOffset);
    const float duration = ReadFloat(words, secondOffset) - start;
    const float alpha = duration > 0.0f ? std::clamp((time - start) / duration, 0.0f, 1.0f) : 0.0f;
    const uint32 flags = words[firstOffset + 1u];
    const AnimInterp interpolation = (flags & kOverrideBit) != 0u
        ? static_cast<AnimInterp>(flags & 3u) : track.Interpolation;
    const auto firstValue = ReadVector<kComponents>(words, firstOffset + 2u);
    const auto secondValue = ReadVector<kComponents>(words, secondOffset + 2u);
    if (interpolation == AnimInterp::Step || duration <= 0.0f)
        value = firstValue;
    else if (interpolation == AnimInterp::CubicSpline)
    {
        for (uint32 component = 0u; component < kComponents; ++component)
            value[component] = SampleCubic(firstValue[component],
                ReadFloat(words, firstOffset + 2u + kComponents * 2u + component),
                ReadFloat(words, firstOffset + 2u + kComponents * 4u + component), secondValue[component],
                ReadFloat(words, secondOffset + 2u + kComponents + component),
                ReadFloat(words, secondOffset + 2u + kComponents * 3u + component), duration, alpha);
        if constexpr (kComponents == 4u)
        {
            const auto rotation = Mathematics::Quaternion(value.w, value.x, value.y, value.z).Normalized().GetGLM();
            value = {rotation.x, rotation.y, rotation.z, rotation.w};
        }
    }
    else if constexpr (kComponents == 4u)
    {
        const auto rotation = glm::slerp(
            Mathematics::Quaternion(firstValue.w, firstValue.x, firstValue.y, firstValue.z).GetGLM(),
            Mathematics::Quaternion(secondValue.w, secondValue.x, secondValue.y, secondValue.z).GetGLM(), alpha);
        value = {rotation.x, rotation.y, rotation.z, rotation.w};
    }
    else
        value = firstValue * (1.0f - alpha) + secondValue * alpha;
    return true;
}
}

SharedPtr<const AnimationClipData> AnimationClipData::Build(std::span<const AnimChannel> channels)
{
    try
    {
        auto data = MakeShared<AnimationClipData>();
        std::vector<uint32> authoring;
        size_t totalKeys = 0u;
        for (const auto& channel : channels)
        {
            if (channel.keys.size() > AnimationClipArchive::kMaximumSourceBytes /
                    (kAuthoringWordsPerKey * sizeof(uint32)) - totalKeys ||
                static_cast<uint32>(channel.path) > static_cast<uint32>(AnimPath::MorphWeight) ||
                static_cast<uint32>(channel.interp) > static_cast<uint32>(AnimInterp::CubicSpline) ||
                static_cast<uint32>(channel.preInfinity) > static_cast<uint32>(AnimExtrapolation::Oscillate) ||
                static_cast<uint32>(channel.postInfinity) > static_cast<uint32>(AnimExtrapolation::Oscillate))
                return {};
            totalKeys += channel.keys.size();
        }
        authoring.reserve(totalKeys * kAuthoringWordsPerKey);
        data->m_Tracks.reserve(channels.size());
        for (const auto& channel : channels)
        {
            Track track;
            track.BoneIndex = channel.boneIndex;
            track.TargetNameId = channel.targetNameId;
            track.TargetName = channel.targetName;
            track.Path = channel.path;
            track.Interpolation = channel.interp;
            track.PreInfinity = channel.preInfinity;
            track.PostInfinity = channel.postInfinity;
            track.KeyCount = static_cast<uint32>(channel.keys.size());
            track.WordOffset = static_cast<uint32>(data->m_Words.size());
            track.HasCubic = std::any_of(channel.keys.begin(), channel.keys.end(),
                [&channel](const AnimKeyframe& key)
                { return (key.hasSegmentInterpOverride ? key.segmentInterp : channel.interp) == AnimInterp::CubicSpline; });
            const uint32 components = ComponentCount(track.Path);
            track.WordsPerKey = 2u + components * (track.HasCubic ? 5u : 1u);
            float previousTime = -std::numeric_limits<float>::infinity();
            for (const auto& key : channel.keys)
            {
                if (!ValidKey(key, previousTime))
                    return {};
                previousTime = key.time;
                data->m_Words.push_back(std::bit_cast<uint32>(key.time));
                data->m_Words.push_back(static_cast<uint32>(key.segmentInterp) |
                    (key.hasSegmentInterpOverride ? kOverrideBit : 0u));
                AppendFloats(data->m_Words, {Values(key, track.Path), components});
                if (track.HasCubic)
                {
                    AppendFloats(data->m_Words, {key.inTangent, components});
                    AppendFloats(data->m_Words, {key.outTangent, components});
                    AppendFloats(data->m_Words, {key.inWeight, components});
                    AppendFloats(data->m_Words, {key.outWeight, components});
                }
                AppendAuthoring(authoring, key);
            }
            data->m_Tracks.push_back(std::move(track));
        }
        const auto* bytes = reinterpret_cast<const uint8*>(authoring.data());
        if (!AnimationClipArchive::Encode({bytes, authoring.size() * sizeof(uint32)}, data->m_AuthoringArchive))
            return {};
        data->m_Revision = s_NextRevision.fetch_add(1u, std::memory_order_relaxed);
        return data;
    }
    catch (const std::bad_alloc&)
    {
        return {};
    }
}

size_t AnimationClipData::GetMemoryUsage() const
{
    size_t bytes = sizeof(*this) + m_Tracks.capacity() * sizeof(Track) +
        m_Words.capacity() * sizeof(uint32) + m_AuthoringArchive.capacity();
    for (const auto& track : m_Tracks)
        bytes += track.TargetName.capacity();
    return bytes;
}

bool AnimationClipData::DecodeChannels(std::vector<AnimChannel>& channels) const
{
    std::vector<uint8> bytes;
    if (!AnimationClipArchive::Decode(m_AuthoringArchive, bytes) || bytes.size() % sizeof(uint32) != 0u)
        return false;
    std::vector<uint32> storage(bytes.size() / sizeof(uint32));
    if (!bytes.empty())
        std::memcpy(storage.data(), bytes.data(), bytes.size());
    const std::span<const uint32> words(storage);
    size_t expectedWords = 0u;
    for (const auto& track : m_Tracks)
        expectedWords += static_cast<size_t>(track.KeyCount) * kAuthoringWordsPerKey;
    if (words.size() != expectedWords)
        return false;
    std::vector<AnimChannel> decoded;
    decoded.reserve(m_Tracks.size());
    size_t cursor = 0u;
    for (const auto& track : m_Tracks)
    {
        AnimChannel channel{};
        channel.boneIndex = track.BoneIndex;
        channel.targetNameId = track.TargetNameId;
        channel.targetName = track.TargetName;
        channel.path = track.Path;
        channel.interp = track.Interpolation;
        channel.preInfinity = track.PreInfinity;
        channel.postInfinity = track.PostInfinity;
        channel.keys.resize(track.KeyCount);
        for (auto& key : channel.keys)
        {
            key.time = ReadFloat(words, cursor++);
            ReadFloats(words, cursor, key.translation);
            ReadFloats(words, cursor, key.rotation);
            ReadFloats(words, cursor, key.scale);
            ReadFloats(words, cursor, key.inTangent);
            ReadFloats(words, cursor, key.outTangent);
            ReadFloats(words, cursor, key.inWeight);
            ReadFloats(words, cursor, key.outWeight);
            const uint32 flags = words[cursor++];
            key.tangentBroken = static_cast<uint8>(flags >> 24u);
            for (uint32 component = 0u; component < 4u; ++component)
            {
                key.inTangentType[component] = static_cast<AnimTangentType>((flags >> (component * 3u)) & 7u);
                key.outTangentType[component] = static_cast<AnimTangentType>((flags >> (12u + component * 3u)) & 7u);
            }
            const uint32 segment = words[cursor++];
            key.segmentInterp = static_cast<AnimInterp>(segment & 3u);
            key.hasSegmentInterpOverride = (segment & kOverrideBit) != 0u;
        }
        decoded.push_back(std::move(channel));
    }
    channels = std::move(decoded);
    return true;
}

bool AnimationClipData::Sample(const Track& track, float32 time, Mathematics::Vector4& value) const
{
    if (track.Path == AnimPath::Rotation)
        return SampleTrack<4u>(m_Words, track, time, value);
    if (track.Path == AnimPath::MorphWeight)
        return SampleTrack<1u>(m_Words, track, time, value);
    return SampleTrack<3u>(m_Words, track, time, value);
}

} // namespace GameEngine::Animation
