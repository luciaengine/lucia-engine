#pragma once

#include "PageCodec.h"

namespace GameEngine::PageStreaming
{

/// Encodes two channels as BC5 blocks over a 136-square page.
/// Each block uses its extreme endpoints and the nearest palette value, with lowest-index ties.
/// Identical sample blocks produce identical bytes, including neighboring apron blocks.
class Bc5PageCodec final : public PageCodec
{
public:
    void Encode(uint32 level, std::span<const uint8> samples, const PageFootprint& footprint,
                EncodedPage& out) const override;
    void Decode(uint32 level, const PageIndexEntry& entry, std::span<const uint8> bytes,
                std::span<uint8> outSamples) const override;
};

} // namespace GameEngine::PageStreaming
