#pragma once

#include "Animation/AnimationClip.h"
#include "Mathematics/Vector4.h"

#include <span>

namespace GameEngine::Animation
{

/// Immutable playback tracks and a lossless archive of all authoring fields.
/// Keep a shared snapshot for the entire evaluation or upload. Build is serialized.
class AnimationClipData
{
public:
    struct Track
    {
        uint32 BoneIndex = 0u;
        StringId TargetNameId = 0u;
        String TargetName;
        AnimPath Path = AnimPath::Translation;
        AnimInterp Interpolation = AnimInterp::Linear;
        AnimExtrapolation PreInfinity = AnimExtrapolation::Constant;
        AnimExtrapolation PostInfinity = AnimExtrapolation::Constant;
        uint32 KeyCount = 0u;
        uint32 WordOffset = 0u;
        uint32 WordsPerKey = 0u;
        bool HasCubic = false;
    };

    static SharedPtr<const AnimationClipData> Build(std::span<const AnimChannel> channels);
    std::span<const Track> Tracks() const { return m_Tracks; }
    std::span<const uint32> Words() const { return m_Words; }
    uint64 Revision() const { return m_Revision; }
    size_t GetMemoryUsage() const;
    bool DecodeChannels(std::vector<AnimChannel>& channels) const;
    /// Samples original key times with clamped endpoints and effective segment interpolation.
    /// The track must belong to this retained snapshot.
    bool Sample(const Track& track, float32 time, Mathematics::Vector4& value) const;

private:
    std::vector<Track> m_Tracks;
    std::vector<uint32> m_Words;
    std::vector<uint8> m_AuthoringArchive;
    uint64 m_Revision = 0u;
};

} // namespace GameEngine::Animation
