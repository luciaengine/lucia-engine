#include "DebugServer/Capture/ScreenshotUtils.h"

#include "EditorPanelIds.h"
#include "Mathematics/Interpolation.h"
#include "Mathematics/Vector3.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Passes/SRGBEncodePass.h"
#include "SceneView/SceneViewController.h"
#include "SceneView/SceneViewPanel.h"
#include "UI/Controls/Mount.h"
#include "UI/UIManager.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace GameEngine::Editor
{

// In-place gamma encode (linear → sRGB) for pixels already quantized to RGBA8.
// Only for sources that are 8-bit to begin with (no precision left to recover).
// Float sources must encode in float inside ConvertToRGBA8 BEFORE the quantize:
// encoding after quantizing spreads one 1/255 linear quantum across ~4 output
// LSB near black, manufacturing banding the screen never shows.
static void ApplySRGBEncodeRGBA8(std::vector<uint8_t>& rgba)
{
    auto encode = [](uint8_t v) -> uint8_t
    {
        float linear = static_cast<float>(v) / 255.0f;
        float srgb = (linear <= 0.0031308f)
                         ? 12.92f * linear
                         : 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
        srgb = std::max(0.0f, std::min(1.0f, srgb));
        return static_cast<uint8_t>(srgb * 255.0f + 0.5f);
    };
    for (size_t i = 0; i + 3 < rgba.size(); i += 4)
    {
        rgba[i + 0] = encode(rgba[i + 0]);
        rgba[i + 1] = encode(rgba[i + 1]);
        rgba[i + 2] = encode(rgba[i + 2]);
        // alpha is left untouched
    }
}

// Interleaved gradient noise, ported verbatim from encode_srgb.frag.
// Coordinates are pixel centers (x+0.5, y+0.5) to mirror gl_FragCoord.
static float InterleavedGradientNoiseAt(uint32_t px, uint32_t py, float offset)
{
    auto fract = [](float v) -> float { return v - std::floor(v); };
    const float x = static_cast<float>(px) + 0.5f + offset;
    const float y = static_cast<float>(py) + 0.5f + offset * 0.7f;
    return fract(52.9829189f * fract(0.06711056f * x + 0.00583715f * y));
}

// Per-pixel triangular-PDF dither, ported verbatim from encode_srgb.frag
// (InterleavedGradientNoise + inverse-CDF remap). The screen's sRGB encode pass
// dithers its 8-bit quantize with exactly this pattern; captures quantizing the
// same linear source must match or they show staircase banding the display
// never had. Returns a value in (-1, 1).
static float TriangularDitherAt(uint32_t px, uint32_t py)
{
    const float u = InterleavedGradientNoiseAt(px, py, 0.0f);
    const float o = 2.0f * u - 1.0f;
    const float s = (o > 0.0f) ? 1.0f : ((o < 0.0f) ? -1.0f : 0.0f);
    return s * (1.0f - std::sqrt(std::max(0.0f, 1.0f - std::abs(o))));
}

// Clamp-to-edge bilinear sample of a plane of 3 floats per pixel, at pixel-center
// coordinates (sx, sy).
static Mathematics::Vector3 SampleEncodedPlaneBilinear(
    const std::vector<float>& plane, uint32_t width, uint32_t height, float sx, float sy)
{
    const float fx = std::clamp(sx - 0.5f, 0.0f, static_cast<float>(width - 1));
    const float fy = std::clamp(sy - 0.5f, 0.0f, static_cast<float>(height - 1));
    const uint32_t x0 = static_cast<uint32_t>(fx);
    const uint32_t y0 = static_cast<uint32_t>(fy);
    const uint32_t x1 = std::min(x0 + 1, width - 1);
    const uint32_t y1 = std::min(y0 + 1, height - 1);
    const float tx = fx - static_cast<float>(x0);
    const float ty = fy - static_cast<float>(y0);
    const float* p00 = &plane[(static_cast<size_t>(y0) * width + x0) * 3];
    const float* p10 = &plane[(static_cast<size_t>(y0) * width + x1) * 3];
    const float* p01 = &plane[(static_cast<size_t>(y1) * width + x0) * 3];
    const float* p11 = &plane[(static_cast<size_t>(y1) * width + x1) * 3];
    Mathematics::Vector3 rgb;
    for (size_t c = 0; c < 3; ++c)
    {
        const float top = p00[c] + (p10[c] - p00[c]) * tx;
        const float bot = p01[c] + (p11[c] - p01[c]) * tx;
        rgb[c] = top + (bot - top) * ty;
    }
    return rgb;
}

// Gradient-aware deband on the encoded float plane — the CPU mirror of
// encode_srgb.frag's pre-dither DebandEncoded, kept so captures continue to
// match the screen (the same parity contract as TriangularDitherAt above):
// identical noise fields, tap geometry, iteration schedule, and all-channel
// threshold gate, with clamp-to-edge bilinear taps mirroring the pass's
// sampler. One deliberate divergence: the shader encodes each bilinear tap of
// the LINEAR source, this taps the pre-ENCODED plane — the two differ only by
// transfer-curve curvature across a tap's bilinear footprint, sub-LSB on the
// shallow gradients the gate opens for, and the gate is shut at real edges
// either way. `thresholdLsb` is the pass-published effective gate — the SAME
// volume/env resolution the screen's backbuffer encode used (see
// GetBackbufferOutputDebandThresholdLsb), so a volume toggle or a GE_DEBAND
// relaunch changes screen and captures together. Radius stays process-wide.
//
// Reached only on frames whose composite is still LINEAR — an HDR-display
// capture, where the terminal pass remains the single encode and debands
// full-frame. On SDR frames each world view debanded inside its own image, so the
// composite arrives finalized and the conversion below requantizes it without
// running either filter.
static std::vector<float> DebandEncodedPlane(
    const std::vector<float>& enc, uint32_t width, uint32_t height, float thresholdLsb)
{
    const float threshold = thresholdLsb / 255.0f;
    const float baseRadius = Rendering::Passes::GetOutputDebandSettings().RadiusPx;
    constexpr int kIterations = 3; // encode_srgb.frag kDebandIterations
    constexpr float kGolden = 0.61803399f;
    constexpr float kTwoPi = 6.2831853f;

    std::vector<float> out(enc.size());
    for (uint32_t py = 0; py < height; ++py)
    {
        for (uint32_t px = 0; px < width; ++px)
        {
            const float u1 = InterleavedGradientNoiseAt(px, py, 5.0f);
            const float u2 = InterleavedGradientNoiseAt(px, py, 11.0f);
            const float cx = static_cast<float>(px) + 0.5f;
            const float cy = static_cast<float>(py) + 0.5f;
            const float* center = &enc[(static_cast<size_t>(py) * width + px) * 3];
            Mathematics::Vector3 debanded(center[0], center[1], center[2]);
            for (int i = 1; i <= kIterations; ++i)
            {
                const float r1 = Math::Fract(u1 + static_cast<float>(i) * kGolden);
                const float r2 = Math::Fract(u2 + static_cast<float>(i) * kGolden);
                const float dist = baseRadius * static_cast<float>(i) * (0.25f + 0.75f * r1);
                const float angle = kTwoPi * r2;
                const float ox = dist * std::cos(angle);
                const float oy = dist * std::sin(angle);
                const Mathematics::Vector3 t0 = SampleEncodedPlaneBilinear(enc, width, height, cx + ox, cy + oy);
                const Mathematics::Vector3 t1 = SampleEncodedPlaneBilinear(enc, width, height, cx - oy, cy + ox);
                const Mathematics::Vector3 t2 = SampleEncodedPlaneBilinear(enc, width, height, cx - ox, cy - oy);
                const Mathematics::Vector3 t3 = SampleEncodedPlaneBilinear(enc, width, height, cx + oy, cy - ox);
                // Gate against the ORIGINAL center (mirrors DebandEncoded):
                // bounds total displacement to one threshold by construction.
                Mathematics::Vector3 avg;
                bool pass = true;
                for (size_t c = 0; c < 3; ++c)
                {
                    avg[c] = 0.25f * (t0[c] + t1[c] + t2[c] + t3[c]);
                    pass = pass && std::abs(avg[c] - center[c]) < threshold;
                }
                if (pass)
                    debanded = avg;
            }
            float* dst = &out[(static_cast<size_t>(py) * width + px) * 3];
            dst[0] = debanded[0];
            dst[1] = debanded[1];
            dst[2] = debanded[2];
        }
    }
    return out;
}

// Encoded-domain finish for float color planes (3 floats per pixel, linear,
// already range-scaled): transfer curve, optional deband, TPDF dither,
// quantize to RGBA8. Splitting the encode from the quantize (vs the inline
// toU8 in ConvertToRGBA8) lets the deband tap a fully-encoded plane the way
// the screen's shader taps its source; with the deband inactive the op order
// (curve, +dither, clamp-quantize) is identical to the inline path.
static void EncodeDitherQuantizePlane(
    std::vector<float>& plane, uint32_t width, uint32_t height,
    bool srgbEncode, bool dither, bool deband, const CaptureEncodeFilterState* filterState,
    std::vector<uint8_t>& rgba)
{
    // Captures always quantize to 8 bits whatever depth the swapchain runs at,
    // so the mirror's dither is sized to ITS OWN quantizer — not to the screen's.
    constexpr float kCaptureDitherLsb = 1.0f / 255.0f;
    // BOTH filters are conditioned on `srgbEncode`, which asks "does this
    // conversion own the transfer curve?" — and therefore also "has anything
    // quantized these values for a destination yet?". A linear source (an
    // HDR-display frame's composite, a float data plane) has not been, so the
    // mirror encodes it and runs the screen's filters. An already-encoded source
    // arrives FINALIZED: every world view applied the OETF, debanded and dithered
    // at the presented step inside its own image, and the chrome around it is CSS
    // bytes that quantize exactly. Filtering that a second time would displace
    // world pixels the screen kept and grain chrome the screen left byte-stable,
    // so the conversion is a pure requantize — which makes screen-vs-capture
    // parity exact on chrome by construction. The residual it accepts: an 8-bit
    // capture of a world dithered for a 10-bit screen keeps only a quarter of
    // that noise, so a shallow gradient can contour in the PNG where the screen
    // shows none. Fixing that belongs upstream (finalize at the capture's step),
    // not in a filter here that cannot tell world from chrome.
    if (srgbEncode)
    {
        for (float& v : plane)
            v = (v <= 0.0031308f) ? 12.92f * v
                                  : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
    }
    // The gate is the pass-published effective threshold (volume AND env
    // resolved), 0 whenever the screen's filter is off — captures and screen
    // stay in lockstep through every combination of lever and override.
    const float effectiveDebandLsb =
        filterState ? filterState->DebandThresholdLsb
                    : Rendering::Passes::GetBackbufferOutputDebandThresholdLsb();
    if (deband && srgbEncode && effectiveDebandLsb > 0.0f)
        plane = DebandEncodedPlane(plane, width, height, effectiveDebandLsb);

    const bool ditherThisPlane = dither && srgbEncode;
    for (uint32_t py = 0; py < height; ++py)
    {
        for (uint32_t px = 0; px < width; ++px)
        {
            const size_t i = static_cast<size_t>(py) * width + px;
            const float d =
                ditherThisPlane ? TriangularDitherAt(px, py) * kCaptureDitherLsb : 0.0f;
            for (int c = 0; c < 3; ++c)
            {
                const float v = plane[i * 3 + c] + d;
                rgba[i * 4 + c] =
                    static_cast<uint8_t>(std::max(0.0f, std::min(255.0f, v * 255.0f + 0.5f)));
            }
        }
    }
}
std::vector<uint8_t> ConvertToRGBA8(
    const void* raw, uint32_t width, uint32_t height,
    Rendering::TextureFormat fmt, float rangeMin, float rangeMax, bool srgbEncode,
    bool dither, bool deband, const CaptureEncodeFilterState* filterState)
{
    using TF = Rendering::TextureFormat;
    const size_t pixelCount = static_cast<size_t>(width) * height;
    std::vector<uint8_t> rgba(pixelCount * 4, 255);

    constexpr float kDitherLsb = 1.0f / 255.0f;

    // d is the per-pixel TPDF offset (already scaled to the 1/255 output LSB;
    // 0 when dithering is off), applied after the transfer curve — the same
    // order as encode_srgb.frag (encode, dither in sRGB space, quantize).
    auto toU8 = [srgbEncode](float v, float d) -> uint8_t
    {
        if (srgbEncode)
            v = (v <= 0.0031308f) ? 12.92f * v
                                  : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
        v += d;
        return static_cast<uint8_t>(std::max(0.0f, std::min(255.0f, v * 255.0f + 0.5f)));
    };

    auto ditherAt = [dither, width](size_t i) -> float
    {
        if (!dither)
            return 0.0f;
        const uint32_t px = static_cast<uint32_t>(i % width);
        const uint32_t py = static_cast<uint32_t>(i / width);
        return TriangularDitherAt(px, py) * kDitherLsb;
    };

    float scale = (rangeMax > rangeMin) ? 1.0f / (rangeMax - rangeMin) : 1.0f;

    if (fmt == TF::RGBA8_UNORM || fmt == TF::RGBA8_SRGB)
    {
        std::memcpy(rgba.data(), raw, pixelCount * 4);
        if (srgbEncode)
            ApplySRGBEncodeRGBA8(rgba);
        return rgba;
    }

    if (fmt == TF::BGRA8_UNORM || fmt == TF::BGRA8_SRGB)
    {
        auto* src = static_cast<const uint8_t*>(raw);
        for (size_t i = 0; i < pixelCount; ++i)
        {
            rgba[i * 4 + 0] = src[i * 4 + 2]; // B→R
            rgba[i * 4 + 1] = src[i * 4 + 1]; // G→G
            rgba[i * 4 + 2] = src[i * 4 + 0]; // R→B
            rgba[i * 4 + 3] = src[i * 4 + 3]; // A→A
        }
        if (srgbEncode)
            ApplySRGBEncodeRGBA8(rgba);
        return rgba;
    }

    if (fmt == TF::D32_FLOAT)
    {
        auto* src = static_cast<const float*>(raw);
        for (size_t i = 0; i < pixelCount; ++i)
        {
            float v = (src[i] - rangeMin) * scale;
            uint8_t g = toU8(v, ditherAt(i));
            rgba[i * 4 + 0] = g;
            rgba[i * 4 + 1] = g;
            rgba[i * 4 + 2] = g;
        }
        return rgba;
    }

    if (fmt == TF::D24_UNORM_S8_UINT || fmt == TF::X8_D24_UNORM_PACK32)
    {
        auto* src = static_cast<const uint32_t*>(raw);
        for (size_t i = 0; i < pixelCount; ++i)
        {
            float depth = static_cast<float>(src[i] & 0x00FFFFFFu) / 16777215.0f;
            float v = (depth - rangeMin) * scale;
            uint8_t g = toU8(v, ditherAt(i));
            rgba[i * 4 + 0] = g;
            rgba[i * 4 + 1] = g;
            rgba[i * 4 + 2] = g;
        }
        return rgba;
    }

    if (fmt == TF::D16_UNORM)
    {
        auto* src = static_cast<const uint16_t*>(raw);
        for (size_t i = 0; i < pixelCount; ++i)
        {
            float depth = static_cast<float>(src[i]) / 65535.0f;
            float v = (depth - rangeMin) * scale;
            uint8_t g = toU8(v, ditherAt(i));
            rgba[i * 4 + 0] = g;
            rgba[i * 4 + 1] = g;
            rgba[i * 4 + 2] = g;
        }
        return rgba;
    }

    if (fmt == TF::R16G16B16A16_FLOAT)
    {
        // half-float: use bit manipulation to convert to float
        auto halfToFloat = [](uint16_t h) -> float
        {
            uint32_t sign = (h >> 15) & 1;
            uint32_t exp = (h >> 10) & 0x1F;
            uint32_t mantissa = h & 0x3FF;
            if (exp == 0)
                return sign ? -0.0f : 0.0f;
            if (exp == 31)
                return sign ? -std::numeric_limits<float>::infinity() : std::numeric_limits<float>::infinity();
            float f = std::ldexp(static_cast<float>(mantissa) / 1024.0f + 1.0f, static_cast<int>(exp) - 15);
            return sign ? -f : f;
        };

        auto* src = static_cast<const uint16_t*>(raw);
        std::vector<float> plane(pixelCount * 3);
        for (size_t i = 0; i < pixelCount; ++i)
        {
            plane[i * 3 + 0] = (halfToFloat(src[i * 4 + 0]) - rangeMin) * scale;
            plane[i * 3 + 1] = (halfToFloat(src[i * 4 + 1]) - rangeMin) * scale;
            plane[i * 3 + 2] = (halfToFloat(src[i * 4 + 2]) - rangeMin) * scale;
        }
        EncodeDitherQuantizePlane(plane, width, height, srgbEncode, dither, deband, filterState, rgba);
        return rgba;
    }

    if (fmt == TF::R16_FLOAT)
    {
        auto halfToFloat = [](uint16_t h) -> float
        {
            uint32_t sign = (h >> 15) & 1;
            uint32_t exp = (h >> 10) & 0x1F;
            uint32_t mantissa = h & 0x3FF;
            if (exp == 0)
                return sign ? -0.0f : 0.0f;
            if (exp == 31)
                return sign ? -std::numeric_limits<float>::infinity() : std::numeric_limits<float>::infinity();
            float f = std::ldexp(static_cast<float>(mantissa) / 1024.0f + 1.0f, static_cast<int>(exp) - 15);
            return sign ? -f : f;
        };

        auto* src = static_cast<const uint16_t*>(raw);
        for (size_t i = 0; i < pixelCount; ++i)
        {
            float v = (halfToFloat(src[i]) - rangeMin) * scale;
            uint8_t g = toU8(v, ditherAt(i));
            rgba[i * 4 + 0] = g;
            rgba[i * 4 + 1] = g;
            rgba[i * 4 + 2] = g;
        }
        return rgba;
    }

    if (fmt == TF::R32_FLOAT)
    {
        auto* src = static_cast<const float*>(raw);
        for (size_t i = 0; i < pixelCount; ++i)
        {
            const uint8_t g = toU8((src[i] - rangeMin) * scale, ditherAt(i));
            rgba[i * 4 + 0] = g;
            rgba[i * 4 + 1] = g;
            rgba[i * 4 + 2] = g;
        }
        return rgba;
    }

    if (fmt == TF::R32G32B32A32_FLOAT)
    {
        auto* src = static_cast<const float*>(raw);
        std::vector<float> plane(pixelCount * 3);
        for (size_t i = 0; i < pixelCount; ++i)
        {
            plane[i * 3 + 0] = (src[i * 4 + 0] - rangeMin) * scale;
            plane[i * 3 + 1] = (src[i * 4 + 1] - rangeMin) * scale;
            plane[i * 3 + 2] = (src[i * 4 + 2] - rangeMin) * scale;
        }
        EncodeDitherQuantizePlane(plane, width, height, srgbEncode, dither, deband, filterState, rgba);
        return rgba;
    }

    if (fmt == TF::R11G11B10_FLOAT)
    {
        auto* src = static_cast<const uint32_t*>(raw);
        for (size_t i = 0; i < pixelCount; ++i)
        {
            // Unpack R11G11B10 — 11-bit R, 11-bit G, 10-bit B (all unsigned float)
            uint32_t bits = src[i];
            auto unpack11 = [](uint32_t v) -> float
            {
                uint32_t exp = (v >> 6) & 0x1F;
                uint32_t mantissa = v & 0x3F;
                if (exp == 0)
                    return 0.0f;
                if (exp == 31)
                    return 65536.0f;
                return std::ldexp(static_cast<float>(mantissa) / 64.0f + 1.0f, static_cast<int>(exp) - 15);
            };
            auto unpack10 = [](uint32_t v) -> float
            {
                uint32_t exp = (v >> 5) & 0x1F;
                uint32_t mantissa = v & 0x1F;
                if (exp == 0)
                    return 0.0f;
                if (exp == 31)
                    return 65536.0f;
                return std::ldexp(static_cast<float>(mantissa) / 32.0f + 1.0f, static_cast<int>(exp) - 15);
            };
            float r = (unpack11(bits & 0x7FF) - rangeMin) * scale;
            float g = (unpack11((bits >> 11) & 0x7FF) - rangeMin) * scale;
            float b = (unpack10((bits >> 22) & 0x3FF) - rangeMin) * scale;
            const float d = ditherAt(i);
            rgba[i * 4 + 0] = toU8(r, d);
            rgba[i * 4 + 1] = toU8(g, d);
            rgba[i * 4 + 2] = toU8(b, d);
        }
        return rgba;
    }

    if (fmt == TF::R8_UNORM)
    {
        auto* src = static_cast<const uint8_t*>(raw);
        for (size_t i = 0; i < pixelCount; ++i)
        {
            rgba[i * 4 + 0] = src[i];
            rgba[i * 4 + 1] = src[i];
            rgba[i * 4 + 2] = src[i];
        }
        if (srgbEncode)
            ApplySRGBEncodeRGBA8(rgba);
        return rgba;
    }

    if (fmt == TF::RGB10A2_UNORM)
    {
        auto* src = static_cast<const uint32_t*>(raw);
        for (size_t i = 0; i < pixelCount; ++i)
        {
            uint32_t bits = src[i];
            float r = static_cast<float>(bits & 0x3FF) / 1023.0f;
            float g = static_cast<float>((bits >> 10) & 0x3FF) / 1023.0f;
            float b = static_cast<float>((bits >> 20) & 0x3FF) / 1023.0f;
            const float d = ditherAt(i);
            rgba[i * 4 + 0] = toU8(r, d);
            rgba[i * 4 + 1] = toU8(g, d);
            rgba[i * 4 + 2] = toU8(b, d);
        }
        return rgba;
    }

    // Fallback: copy raw bytes as RGBA8 (best effort).
    size_t bpp = Rendering::BytesPerPixel(fmt);
    if (bpp > 0)
    {
        auto* src = static_cast<const uint8_t*>(raw);
        size_t copyBytes = std::min(bpp, size_t(4));
        for (size_t i = 0; i < pixelCount; ++i)
        {
            std::memcpy(&rgba[i * 4], &src[i * bpp], copyBytes);
        }
        if (srgbEncode)
            ApplySRGBEncodeRGBA8(rgba);
    }
    return rgba;
}

SceneViewController* ResolveActiveSceneController(EditorApplication::EditorWindowContext& window)
{
    SceneViewController* fallback = window.scene.get();
    if (!window.ui)
        return fallback;
    auto* root = window.ui->GetRootElement();
    if (!root)
        return fallback;
    auto* mount = root->FindById<Mount>(HashStringId(EditorPanelIds::MountSceneView)).Get();
    auto* panel = mount ? dynamic_cast<SceneViewPanel*>(mount->GetTarget()) : nullptr;
    if (!panel)
        return fallback;
    SceneViewController* active = panel->GetSceneControllerForSlot(panel->GetActiveViewportSlot());
    return active ? active : fallback;
}

} // namespace GameEngine::Editor
