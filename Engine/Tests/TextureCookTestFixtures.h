#pragma once

#include "Types/Types.h"

#include <array>
#include <string>
#include <vector>

namespace GameEngine::Tests
{
// A Radiance image with one RGBE texel per pixel and both edges above one.
inline std::vector<uint8> MakeGradientHdr(uint32 width, uint32 height)
{
    const std::string header = "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y " + std::to_string(height) + " +X " +
                               std::to_string(width) + "\n";
    std::vector<uint8> data(header.begin(), header.end());
    for (uint32 row = 0; row < height; ++row)
    {
        for (uint32 column = 0; column < width; ++column)
        {
            const uint64 exponentNumerator = 2 * (uint64(column) * height + uint64(row) * width);
            const std::array<uint8, 4> texel{
                static_cast<uint8>(64 + (column * 191) / (width - 1)),
                static_cast<uint8>(64 + (row * 191) / (height - 1)),
                128,
                static_cast<uint8>(128 + exponentNumerator / (uint64(width) * height)),
            };
            data.insert(data.end(), texel.begin(), texel.end());
        }
    }
    return data;
}
} // namespace GameEngine::Tests
