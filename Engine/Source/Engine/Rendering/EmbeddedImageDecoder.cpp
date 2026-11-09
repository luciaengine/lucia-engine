#include "Engine/Rendering/EmbeddedImageDecoder.h"

#include "Image/Decode.h"

#include <stb_image.h>
#include <span>
#include <utility>

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
    if (!compressedData || compressedSize == 0)
        return {};
    Image::Image8 image = Image::Decode8(std::span(compressedData, compressedSize), "embedded image",
                                        Image::DecodeChannels::Rgba);
    return {std::move(image.Pixels), image.Width, image.Height, image.Valid};
}

} // namespace GameEngine
