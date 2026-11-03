#pragma once

#include "Engine/Rendering/EmbeddedImageDecoder.h"

#include <cstdint>
#include <vector>

namespace GameEngine
{

/// `image` scaled down, with sRGB-aware filtering and its aspect kept, so that its longer edge is
/// `maxLongEdge` pixels. An image whose longer edge is already at most `maxLongEdge` is returned
/// unchanged; an invalid image, or a failed resize, returns an invalid one.
DecodedImage DownscaleRgbaToLongEdge(const DecodedImage& image, uint32_t maxLongEdge);

/// `image` (RGBA8) encoded as a PNG file's bytes; empty when it is invalid or encoding fails.
std::vector<uint8_t> EncodeRgbaPng(const DecodedImage& image);

/// `image` (RGBA8) encoded as a JPEG file's bytes at `quality` (1 to 100), its alpha dropped;
/// empty when it is invalid or encoding fails.
std::vector<uint8_t> EncodeRgbaJpeg(const DecodedImage& image, int quality);

} // namespace GameEngine
