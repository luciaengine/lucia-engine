#include "Engine/Rendering/EmbeddedImageDecoder.h"

#include <stb_image.h>

#include <limits>

namespace GameEngine
{

bool ReadImageDimensions(const uint8_t* compressedData, size_t compressedSize, uint32_t& width, uint32_t& height)
{
    width = height = 0;
    if (!compressedData || compressedSize == 0 || compressedSize > static_cast<size_t>(std::numeric_limits<int>::max()))
        return false;
    int w = 0, h = 0, channels = 0;
    if (!stbi_info_from_memory(compressedData, static_cast<int>(compressedSize), &w, &h, &channels) || w <= 0 || h <= 0)
        return false;
    width = static_cast<uint32_t>(w);
    height = static_cast<uint32_t>(h);
    return true;
}

DecodedImage DecodeImageToRGBA(const uint8_t* compressedData, size_t compressedSize)
{
    DecodedImage result{};
    if (!compressedData || compressedSize == 0)
        return result;

    int w = 0, h = 0, channels = 0;
    stbi_uc* pixels = stbi_load_from_memory(
        compressedData, static_cast<int>(compressedSize),
        &w, &h, &channels, 4); // force RGBA

    if (!pixels || w <= 0 || h <= 0)
    {
        if (pixels)
            stbi_image_free(pixels);
        return result;
    }

    const size_t byteCount = static_cast<size_t>(w) * static_cast<size_t>(h) * 4;
    result.pixels.assign(pixels, pixels + byteCount);
    result.width = static_cast<uint32_t>(w);
    result.height = static_cast<uint32_t>(h);
    result.valid = true;

    stbi_image_free(pixels);
    return result;
}

} // namespace GameEngine
