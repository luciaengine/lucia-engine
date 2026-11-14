#include "Thumbnails/ThumbnailImageOps.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace GameEngine
{
namespace Editor
{

ThumbnailImage MakeTransparent(int32 width, int32 height)
{
    ThumbnailImage image;
    if (width <= 0 || height <= 0)
        return image;
    image.Width = width;
    image.Height = height;
    image.Pixels.assign(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u, 0u);
    return image;
}

ThumbnailCropRect TrimToOpaqueBounds(const ThumbnailImage& source, const ThumbnailCropRect& uv,
                                     uint8 alphaThreshold)
{
    if (source.IsEmpty())
        return uv;

    const float32 sourceW = static_cast<float32>(source.Width);
    const float32 sourceH = static_cast<float32>(source.Height);
    const int32 x0 = std::clamp(static_cast<int32>(std::lround(uv.U0 * sourceW)), 0, source.Width);
    const int32 x1 = std::clamp(static_cast<int32>(std::lround(uv.U1 * sourceW)), 0, source.Width);
    const int32 y0 = std::clamp(static_cast<int32>(std::lround(uv.V0 * sourceH)), 0, source.Height);
    const int32 y1 = std::clamp(static_cast<int32>(std::lround(uv.V1 * sourceH)), 0, source.Height);
    if (x1 - x0 <= 0 || y1 - y0 <= 0)
        return uv;

    int32 minX = x1;
    int32 minY = y1;
    int32 maxX = x0;
    int32 maxY = y0;
    for (int32 y = y0; y < y1; ++y)
    {
        for (int32 x = x0; x < x1; ++x)
        {
            const size_t index =
                (static_cast<size_t>(y) * static_cast<size_t>(source.Width) +
                 static_cast<size_t>(x)) * 4u + 3u;
            if (index >= source.Pixels.size() || source.Pixels[index] <= alphaThreshold)
                continue;
            minX = std::min(minX, x);
            minY = std::min(minY, y);
            maxX = std::max(maxX, x);
            maxY = std::max(maxY, y);
        }
    }
    if (minX > maxX || minY > maxY)
        return uv;

    // maxX/maxY name the last opaque texel, so the exclusive edge is one past it.
    return ThumbnailCropRect{static_cast<float32>(minX) / sourceW,
                             static_cast<float32>(minY) / sourceH,
                             static_cast<float32>(maxX + 1) / sourceW,
                             static_cast<float32>(maxY + 1) / sourceH};
}

ThumbnailImage ScaleUpInteger(const ThumbnailImage& image, int32 minEdgePx, int32 maxEdgePx)
{
    if (image.IsEmpty() || minEdgePx <= 0 || maxEdgePx <= 0)
        return image;

    const int32 longEdge = std::max(image.Width, image.Height);
    if (longEdge >= minEdgePx)
        return image;

    const int32 wanted = minEdgePx / longEdge;
    const int32 allowed = maxEdgePx / longEdge;
    const int32 factor = std::min(wanted, allowed);
    if (factor <= 1)
        return image;

    ThumbnailImage target = MakeTransparent(image.Width * factor, image.Height * factor);
    if (target.IsEmpty())
        return image;

    for (int32 y = 0; y < target.Height; ++y)
    {
        const int32 srcY = y / factor;
        for (int32 x = 0; x < target.Width; ++x)
        {
            const int32 srcX = x / factor;
            const size_t srcIndex =
                (static_cast<size_t>(srcY) * static_cast<size_t>(image.Width) +
                 static_cast<size_t>(srcX)) * 4u;
            const size_t dstIndex =
                (static_cast<size_t>(y) * static_cast<size_t>(target.Width) +
                 static_cast<size_t>(x)) * 4u;
            std::copy_n(image.Pixels.begin() + static_cast<ptrdiff_t>(srcIndex), 4,
                        target.Pixels.begin() + static_cast<ptrdiff_t>(dstIndex));
        }
    }
    return target;
}

ThumbnailImage AddTransparentMargin(const ThumbnailImage& image, int32 marginPx)
{
    if (image.IsEmpty() || marginPx <= 0)
        return image;

    ThumbnailImage target =
        MakeTransparent(image.Width + marginPx * 2, image.Height + marginPx * 2);
    if (target.IsEmpty())
        return image;

    const size_t sourceRowBytes = static_cast<size_t>(image.Width) * 4u;
    for (int32 y = 0; y < image.Height; ++y)
    {
        const size_t srcIndex = static_cast<size_t>(y) * sourceRowBytes;
        const size_t dstIndex =
            (static_cast<size_t>(y + marginPx) * static_cast<size_t>(target.Width) +
             static_cast<size_t>(marginPx)) * 4u;
        std::copy_n(image.Pixels.begin() + static_cast<ptrdiff_t>(srcIndex), sourceRowBytes,
                    target.Pixels.begin() + static_cast<ptrdiff_t>(dstIndex));
    }
    return target;
}

void MirrorHorizontal(ThumbnailImage& image)
{
    if (image.IsEmpty())
        return;
    const size_t rowBytes = static_cast<size_t>(image.Width) * 4u;
    for (int32 y = 0; y < image.Height; ++y)
    {
        uint8* row = image.Pixels.data() + static_cast<size_t>(y) * rowBytes;
        for (int32 x = 0; x < image.Width / 2; ++x)
        {
            uint8* left = row + static_cast<size_t>(x) * 4u;
            uint8* right = row + static_cast<size_t>(image.Width - 1 - x) * 4u;
            std::swap_ranges(left, left + 4, right);
        }
    }
}

ThumbnailImage CropNearest(const ThumbnailImage& source, const ThumbnailCropRect& uv,
                           int32 maxEdgePx)
{
    if (source.IsEmpty() || maxEdgePx <= 0)
        return {};

    const float32 sourceW = static_cast<float32>(source.Width);
    const float32 sourceH = static_cast<float32>(source.Height);
    const int32 srcX0 =
        std::clamp(static_cast<int32>(std::lround(uv.U0 * sourceW)), 0, source.Width);
    const int32 srcX1 =
        std::clamp(static_cast<int32>(std::lround(uv.U1 * sourceW)), 0, source.Width);
    const int32 srcY0 =
        std::clamp(static_cast<int32>(std::lround(uv.V0 * sourceH)), 0, source.Height);
    const int32 srcY1 =
        std::clamp(static_cast<int32>(std::lround(uv.V1 * sourceH)), 0, source.Height);

    const int32 cropWidth = srcX1 - srcX0;
    const int32 cropHeight = srcY1 - srcY0;
    if (cropWidth <= 0 || cropHeight <= 0)
        return {};

    const int32 longEdge = std::max(cropWidth, cropHeight);
    const float32 scale = longEdge > maxEdgePx
                              ? static_cast<float32>(maxEdgePx) / static_cast<float32>(longEdge)
                              : 1.0f;
    const int32 outWidth = std::clamp(
        static_cast<int32>(std::lround(static_cast<float32>(cropWidth) * scale)), 1, cropWidth);
    const int32 outHeight = std::clamp(
        static_cast<int32>(std::lround(static_cast<float32>(cropHeight) * scale)), 1, cropHeight);

    ThumbnailImage target = MakeTransparent(outWidth, outHeight);
    if (target.IsEmpty())
        return target;

    for (int32 y = 0; y < outHeight; ++y)
    {
        // Sample the texel centre of the destination pixel, so a 1:1 crop is
        // exact and a downscale picks a representative texel per pixel.
        const float32 v = (static_cast<float32>(y) + 0.5f) / static_cast<float32>(outHeight);
        const int32 srcY = std::clamp(srcY0 + static_cast<int32>(v * static_cast<float32>(cropHeight)),
                                      srcY0, srcY1 - 1);
        for (int32 x = 0; x < outWidth; ++x)
        {
            const float32 u = (static_cast<float32>(x) + 0.5f) / static_cast<float32>(outWidth);
            const int32 srcX =
                std::clamp(srcX0 + static_cast<int32>(u * static_cast<float32>(cropWidth)), srcX0,
                           srcX1 - 1);

            const size_t srcIndex =
                (static_cast<size_t>(srcY) * static_cast<size_t>(source.Width) +
                 static_cast<size_t>(srcX)) * 4u;
            const size_t dstIndex =
                (static_cast<size_t>(y) * static_cast<size_t>(outWidth) +
                 static_cast<size_t>(x)) * 4u;
            if (srcIndex + 3u >= source.Pixels.size())
                continue;
            std::copy_n(source.Pixels.begin() + static_cast<ptrdiff_t>(srcIndex), 4,
                        target.Pixels.begin() + static_cast<ptrdiff_t>(dstIndex));
        }
    }
    return target;
}

} // namespace Editor
} // namespace GameEngine
