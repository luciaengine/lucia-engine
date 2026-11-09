#pragma once

#include "Types/Types.h"

#include <span>
#include <vector>

namespace GameEngine::Animation
{

/// Versioned, bounded lossless storage for clip source and authoring bytes.
class AnimationClipArchive
{
public:
    static constexpr uint32 kMaximumSourceBytes = 256u * 1024u * 1024u;

    /// Largest envelope for a source within the decoded-size limit.
    static size_t MaximumEncodedBytes();

    static bool IsArchive(std::span<const uint8> bytes);
    static bool Encode(std::span<const uint8> source, std::vector<uint8>& output);
    /// Rejects truncated streams, trailing bytes, and expansion past the format limit.
    static bool Decode(std::span<const uint8> bytes, std::vector<uint8>& output);
};

} // namespace GameEngine::Animation
