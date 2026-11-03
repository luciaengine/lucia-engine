#include "Engine/Rendering/RgbaImageOps.h"

#include <stb_image_resize2.h>
#include <stb_image_write.h>

#include <algorithm>
#include <cmath>

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

    const double scale = static_cast<double>(maxLongEdge) / static_cast<double>(longEdge);
    DecodedImage scaled;
    scaled.width = std::max(1u, static_cast<uint32_t>(std::lround(image.width * scale)));
    scaled.height = std::max(1u, static_cast<uint32_t>(std::lround(image.height * scale)));
    scaled.pixels.resize(static_cast<size_t>(scaled.width) * scaled.height * 4);
    // Alpha rides STBIR_RGBA, which keeps coverage off the sRGB transfer curve.
    if (!stbir_resize_uint8_srgb(image.pixels.data(), static_cast<int>(image.width), static_cast<int>(image.height), 0,
                                 scaled.pixels.data(), static_cast<int>(scaled.width),
                                 static_cast<int>(scaled.height), 0, STBIR_RGBA))
        return {};
    scaled.valid = true;
    return scaled;
}

std::vector<uint8_t> EncodeRgbaPng(const DecodedImage& image)
{
    std::vector<uint8_t> png;
    if (!image.valid || image.width == 0 || image.height == 0)
        return png;
    const int width = static_cast<int>(image.width);
    if (!stbi_write_png_to_func(AppendBytes, &png, width, static_cast<int>(image.height), 4, image.pixels.data(),
                                width * 4))
        png.clear();
    return png;
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
