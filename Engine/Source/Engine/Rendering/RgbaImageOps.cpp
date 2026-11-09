#include "Engine/Rendering/RgbaImageOps.h"

#include "Image/Encode.h"
#include "Image/Resample.h"

#include <utility>
#include <stb_image_write.h>

#include <algorithm>

namespace GameEngine
{
namespace
{
void AppendBytes(void* context, void* data, int size)
{
    auto* out = static_cast<std::vector<uint8_t>*>(context);
    const auto* bytes = static_cast<const uint8_t*>(data);
    out->insert(out->end(), bytes, bytes + size);
}
} // namespace

DecodedImage DownscaleRgbaToLongEdge(const DecodedImage& image, uint32_t maxLongEdge)
{
    if (!image.valid || image.width == 0 || image.height == 0 || maxLongEdge == 0)
        return {};
    const uint32_t longEdge = std::max(image.width, image.height);
    if (longEdge <= maxLongEdge)
        return image;

    Image::Image8 scaled = Image::ResampleToLongEdge(image.pixels, image.width, image.height, maxLongEdge);
    return {std::move(scaled.Pixels), scaled.Width, scaled.Height, scaled.Valid};
}

std::vector<uint8_t> EncodeRgbaPng(const DecodedImage& image)
{
    if (!image.valid)
        return {};
    return Image::EncodePng(image.pixels, image.width, image.height);
}

std::vector<uint8_t> EncodeRgbaJpeg(const DecodedImage& image, int quality)
{
    std::vector<uint8_t> jpeg;
    if (!image.valid || image.width == 0 || image.height == 0)
        return jpeg;
    if (!stbi_write_jpg_to_func(AppendBytes, &jpeg, static_cast<int>(image.width), static_cast<int>(image.height), 4,
                                image.pixels.data(), std::clamp(quality, 1, 100)))
        jpeg.clear();
    return jpeg;
}

} // namespace GameEngine
