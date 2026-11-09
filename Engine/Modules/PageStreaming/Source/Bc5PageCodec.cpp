#include "Bc5PageCodec.h"

#include "Types/Fnv1a.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdlib>
#include <limits>

namespace GameEngine::PageStreaming
{
namespace
{

constexpr uint32 kChannels = 2u;
constexpr uint32 kBlockValues = kPageBlockTexels * kPageBlockTexels;
constexpr uint32 kBc4BlockBytes = kBc5BlockBytes / kChannels;
constexpr uint32 kIndexBits = 3u;
constexpr uint32 kPaletteSize = 8u;

using Palette = std::array<uint8, kPaletteSize>;

// The palette of a BC4 block's two endpoint bytes, each interpolated entry rounded to nearest:
// six interpolated entries when the first endpoint is the larger, else four and then 0 and 255.
Palette MakePalette(uint8 first, uint8 second)
{
    Palette palette{first, second};
    if (first > second)
    {
        for (uint32 paletteIndex = 2; paletteIndex < kPaletteSize; ++paletteIndex)
            palette[paletteIndex] = static_cast<uint8>(((8u - paletteIndex) * first + (paletteIndex - 1u) * second + 3u) / 7u);
        return palette;
    }
    for (uint32 paletteIndex = 2; paletteIndex < 6u; ++paletteIndex)
        palette[paletteIndex] = static_cast<uint8>(((6u - paletteIndex) * first + (paletteIndex - 1u) * second + 2u) / 5u);
    palette[6] = 0u;
    palette[7] = 255u;
    return palette;
}

// One channel's 16 values as a BC4 block.
void EncodeBc4(const std::array<uint8, kBlockValues>& values, uint8* out)
{
    const auto [lowest, highest] = std::minmax_element(values.begin(), values.end());
    // Highest first selects the eight-entry palette; equal endpoints give a palette whose first
    // entry is the one value, which every index 0 names.
    out[0] = *highest;
    out[1] = *lowest;
    const Palette palette = MakePalette(out[0], out[1]);
    uint64 indices = 0;
    for (uint32 sampleIndex = 0; sampleIndex < kBlockValues; ++sampleIndex)
    {
        uint32 best = 0;
        int32 bestDistance = std::numeric_limits<int32>::max();
        for (uint32 paletteIndex = 0; paletteIndex < kPaletteSize; ++paletteIndex)
        {
            const int32 distance = std::abs(static_cast<int32>(values[sampleIndex]) - static_cast<int32>(palette[paletteIndex]));
            if (distance < bestDistance)
            {
                best = paletteIndex;
                bestDistance = distance;
            }
        }
        indices |= static_cast<uint64>(best) << (kIndexBits * sampleIndex);
    }
    for (uint32 byteIndex = 0; byteIndex < kBc4BlockBytes - 2u; ++byteIndex)
        out[2u + byteIndex] = static_cast<uint8>(indices >> (8u * byteIndex));
}

void DecodeBc4(const uint8* block, std::array<uint8, kBlockValues>& values)
{
    const Palette palette = MakePalette(block[0], block[1]);
    uint64 indices = 0;
    for (uint32 byteIndex = 0; byteIndex < kBc4BlockBytes - 2u; ++byteIndex)
        indices |= static_cast<uint64>(block[2u + byteIndex]) << (8u * byteIndex);
    for (uint32 sampleIndex = 0; sampleIndex < kBlockValues; ++sampleIndex)
        values[sampleIndex] = palette[(indices >> (kIndexBits * sampleIndex)) & (kPaletteSize - 1u)];
}

} // namespace

void Bc5PageCodec::Encode(uint32 /*level*/, std::span<const uint8> samples, const PageFootprint& /*footprint*/,
                          EncodedPage& out) const
{
    const uint32 stride = PageStrideTexels(PageFieldFormat::Bc5);
    const uint32 blocks = stride / kPageBlockTexels;
    assert(samples.size() == static_cast<std::size_t>(stride) * stride * kChannels);
    out.Bytes.resize(PageStoredBytes(PageFieldFormat::Bc5, false));
    std::array<uint8, kBlockValues> values{};
    for (uint32 blockRow = 0; blockRow < blocks; ++blockRow)
    {
        for (uint32 blockColumn = 0; blockColumn < blocks; ++blockColumn)
        {
            uint8* block = out.Bytes.data() + (static_cast<std::size_t>(blockRow) * blocks + blockColumn) * kBc5BlockBytes;
            for (uint32 channel = 0; channel < kChannels; ++channel)
            {
                for (uint32 sampleIndex = 0; sampleIndex < kBlockValues; ++sampleIndex)
                {
                    const uint32 row = blockRow * kPageBlockTexels + sampleIndex / kPageBlockTexels;
                    const uint32 column = blockColumn * kPageBlockTexels + sampleIndex % kPageBlockTexels;
                    values[sampleIndex] = samples[(static_cast<std::size_t>(row) * stride + column) * kChannels + channel];
                }
                EncodeBc4(values, block + channel * kBc4BlockBytes);
            }
        }
    }
    out.Hash = Hashing::Fnv1a64(out.Bytes.data(), out.Bytes.size());
    out.MinWord = 0;
    out.MaxWord = 0;
    out.Flags = 0;
}

void Bc5PageCodec::Decode(uint32 /*level*/, const PageIndexEntry& /*entry*/, std::span<const uint8> bytes,
                          std::span<uint8> outSamples) const
{
    const uint32 stride = PageStrideTexels(PageFieldFormat::Bc5);
    const uint32 blocks = stride / kPageBlockTexels;
    assert(bytes.size() == PageStoredBytes(PageFieldFormat::Bc5, false) &&
           outSamples.size() == static_cast<std::size_t>(stride) * stride * kChannels);
    std::array<uint8, kBlockValues> values{};
    for (uint32 blockRow = 0; blockRow < blocks; ++blockRow)
    {
        for (uint32 blockColumn = 0; blockColumn < blocks; ++blockColumn)
        {
            const uint8* block = bytes.data() + (static_cast<std::size_t>(blockRow) * blocks + blockColumn) * kBc5BlockBytes;
            for (uint32 channel = 0; channel < kChannels; ++channel)
            {
                DecodeBc4(block + channel * kBc4BlockBytes, values);
                for (uint32 sampleIndex = 0; sampleIndex < kBlockValues; ++sampleIndex)
                {
                    const uint32 row = blockRow * kPageBlockTexels + sampleIndex / kPageBlockTexels;
                    const uint32 column = blockColumn * kPageBlockTexels + sampleIndex % kPageBlockTexels;
                    outSamples[(static_cast<std::size_t>(row) * stride + column) * kChannels + channel] = values[sampleIndex];
                }
            }
        }
    }
}

} // namespace GameEngine::PageStreaming
