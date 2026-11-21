#pragma once

#include "Types/Types.h"

#include <array>
#include <string>
#include <vector>

namespace GameEngine::Tests
{
// A square Radiance image with one RGBE texel per pixel.
inline std::vector<uint8> MakeGradientHdr(uint32 size)
{
    const std::string header = "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y " + std::to_string(size) + " +X " +
                               std::to_string(size) + "\n";
    std::vector<uint8> data(header.begin(), header.end());
    for (uint32 row = 0; row < size; ++row)
    {
        for (uint32 column = 0; column < size; ++column)
        {
            const std::array<uint8, 4> texel{
                static_cast<uint8>(64 + (column * 191) / (size - 1)),
                static_cast<uint8>(64 + (row * 191) / (size - 1)),
                128,
                static_cast<uint8>(128 + ((column + row) * 4) / (2 * size)),
            };
            data.insert(data.end(), texel.begin(), texel.end());
        }
    }
    return data;
}
} // namespace GameEngine::Tests
