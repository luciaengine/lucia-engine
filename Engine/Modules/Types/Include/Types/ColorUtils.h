#pragma once

#include "Types/Color.h"

#include <algorithm>
#include <cstdint>

namespace GameEngine
{
// Shared color conversion helpers that belong in the Types module so they can be
// used without pulling in UI (or any UI-specific dependencies).
namespace ColorUtils
{
/// Pack normalized RGBA channels as 0xAARRGGBB, rounding to the nearest byte.
/// Channels are clamped to [0, 1]; no color-space conversion is performed.
inline std::uint32_t PackArgbRounded(float red, float green, float blue, float alpha)
{
    constexpr float kByteScale = 255.0f;
    constexpr float kRoundToNearest = 0.5f;
    const auto alphaByte = static_cast<std::uint32_t>(std::clamp(alpha, 0.0f, 1.0f) * kByteScale + kRoundToNearest);
    const auto redByte = static_cast<std::uint32_t>(std::clamp(red, 0.0f, 1.0f) * kByteScale + kRoundToNearest);
    const auto greenByte = static_cast<std::uint32_t>(std::clamp(green, 0.0f, 1.0f) * kByteScale + kRoundToNearest);
    const auto blueByte = static_cast<std::uint32_t>(std::clamp(blue, 0.0f, 1.0f) * kByteScale + kRoundToNearest);
    return (alphaByte << 24) | (redByte << 16) | (greenByte << 8) | blueByte;
}

/// Unpack 0xAARRGGBB into normalized RGBA channels in [0, 1]. Inverse of
/// PackArgbRounded; no color-space conversion is performed.
inline ColorLinear UnpackArgb(std::uint32_t argb)
{
    constexpr float kInv255 = 1.0f / 255.0f;
    return {static_cast<float>((argb >> 16) & 0xFF) * kInv255, static_cast<float>((argb >> 8) & 0xFF) * kInv255,
            static_cast<float>(argb & 0xFF) * kInv255, static_cast<float>((argb >> 24) & 0xFF) * kInv255};
}

// Luminance of a LINEAR Rec.709 / sRGB-primaries colour (the engine's scene-linear space), with the
// BT.709 weights. Relative: 1 for linear white.
constexpr float LinearRec709Luminance(const float linearRgb[3])
{
    return 0.2126f * linearRgb[0] + 0.7152f * linearRgb[1] + 0.0722f * linearRgb[2];
}

// Interprets the input as sRGB-encoded ARGB (0xAARRGGBB) and converts to *linear* floats in [0,1].
inline void ARGBToLinearFloats(std::uint32_t argb, float out[4])
{
    // Colors from CSS/UI are stored as ARGB 0xAARRGGBB in sRGB space.
    std::uint8_t a8 = static_cast<std::uint8_t>((argb >> 24) & 0xFF);
    std::uint8_t r8 = static_cast<std::uint8_t>((argb >> 16) & 0xFF);
    std::uint8_t g8 = static_cast<std::uint8_t>((argb >> 8) & 0xFF);
    std::uint8_t b8 = static_cast<std::uint8_t>(argb & 0xFF);

    ColorSRGB srgb = ColorSRGB::FromSRGB255(r8, g8, b8, a8);
    ColorLinear linear = srgb.ToLinear();
    out[0] = linear.r;
    out[1] = linear.g;
    out[2] = linear.b;
    out[3] = linear.a;
}

// Packs float RGB in [0,1] into opaque ARGB (0xFFRRGGBB), clamping and rounding each channel.
// No transfer function: the bytes carry whatever encoding the floats are in.
inline std::uint32_t RgbFloatsToArgb(const float rgb[3])
{
    const auto toByte = [](float value)
    { return static_cast<std::uint32_t>(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return 0xFF000000u | (toByte(rgb[0]) << 16) | (toByte(rgb[1]) << 8) | toByte(rgb[2]);
}

// Unpacks the RGB bytes of an ARGB value (0xAARRGGBB) into floats in [0,1]; alpha is ignored.
// No transfer function, the inverse of RgbFloatsToArgb.
inline void ArgbToRgbFloats(std::uint32_t argb, float out[3])
{
    out[0] = static_cast<float>((argb >> 16) & 0xFFu) / 255.0f;
    out[1] = static_cast<float>((argb >> 8) & 0xFFu) / 255.0f;
    out[2] = static_cast<float>(argb & 0xFFu) / 255.0f;
}

// Convert HSV to RGB (sRGB space).
// h: hue in degrees (0-360), s: saturation (0-1), v: value (0-1)
// Returns r, g, b in range 0-1
inline void HsvToRgb(float h, float s, float v, float& r, float& g, float& b)
{
    ColorHSV hsv(h, s, v, 1.0f);
    ColorSRGB srgb = hsv.ToSRGB();
    r = srgb.r;
    g = srgb.g;
    b = srgb.b;
}

// Convert RGB to HSV (sRGB space).
// r, g, b: in range 0-1
// Returns h: hue in degrees (0-360), s: saturation (0-1), v: value (0-1)
inline void RgbToHsv(float r, float g, float b, float& h, float& s, float& v)
{
    ColorSRGB srgb(r, g, b, 1.0f);
    ColorHSV hsv = srgb.ToHSV();
    h = hsv.h;
    s = hsv.s;
    v = hsv.v;
}

// Convert HSV to ARGB packed integer (0xAARRGGBB)
inline std::uint32_t HsvToArgb(float h, float s, float v, float a = 1.0f)
{
    ColorHSV hsv(h, s, v, a);
    return hsv.ToARGB();
}

// Convert ARGB packed integer (0xAARRGGBB) to HSV
inline void ArgbToHsv(std::uint32_t argb, float& h, float& s, float& v, float& a)
{
    ColorHSV hsv = ColorHSV::FromARGB(argb);
    h = hsv.h;
    s = hsv.s;
    v = hsv.v;
    a = hsv.a;
}
} // namespace ColorUtils
} // namespace GameEngine
