#pragma once

#include "Rendering/Core/Device.h"

#include <cstdint>

namespace GameEngine
{

struct TextureCookSettings;

/// Creates RGBA8 mips with cookSettings; srgb selects sRGB alpha-weighted filtering.
/// gpuFormat must be RGBA8; outLevels reports uploaded levels (one without a resampler).
/// Failure destroys any new texture, returns an invalid handle, and sets outLevels to zero.
Rendering::TextureHandle UploadRgba8WithMips(Rendering::IDevice* device,
                                             const uint8_t* rgba, uint32_t width, uint32_t height,
                                             Rendering::TextureFormat gpuFormat,
                                             const TextureCookSettings& cookSettings,
                                             bool srgb, const char* debugName,
                                             const char* stagingName, uint32_t& outLevels);

} // namespace GameEngine
