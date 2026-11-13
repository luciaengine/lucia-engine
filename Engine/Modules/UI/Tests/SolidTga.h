#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <vector>

namespace GameEngine::UITesting
{

// Writes an uncompressed 32-bit top-left-origin TGA of `width` x `height` opaque
// texels in one color, given as sRGB bytes.
inline void WriteSolidTga(const std::filesystem::path& path, uint16_t width, uint16_t height, uint8_t red,
                          uint8_t green, uint8_t blue)
{
    std::vector<uint8_t> bytes(18, 0);
    bytes[2] = 2;
    bytes[12] = uint8_t(width & 0xff);
    bytes[13] = uint8_t(width >> 8);
    bytes[14] = uint8_t(height & 0xff);
    bytes[15] = uint8_t(height >> 8);
    bytes[16] = 32;
    bytes[17] = 0x28;
    bytes.reserve(bytes.size() + size_t(width) * height * 4);
    for (size_t texel = 0; texel < size_t(width) * height; ++texel)
        for (const uint8_t channel : {blue, green, red, uint8_t(255)})
            bytes.push_back(channel);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
}

} // namespace GameEngine::UITesting
