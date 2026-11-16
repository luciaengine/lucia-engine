// Pixel-level pin for the Slug rasteriser's edge model. Coverage is the exact
// pixel area of the glyph outline grown outward by
// min(kTextDilationPerPpem * ppem, kTextMaxDilationPx) device px per side
// (ui_sdf_common.glsl): a straight stem edge is a 1 px exact-area ramp, moved
// outward. That is the edge Chrome 155 draws on macOS 15.7 (headless, DPR 1
// and 2, system font smoothing default), measured on straight Roboto stems from
// 8 to 140 px: no reconstruction blur, and a stem 2 x 0.30 px wider than its
// outline at 96 px. This renders real glyphs through the real ui_sdf pipeline
// (staged font, primitive generation, compiled shaders, executed render graph)
// and measures the edges off the readback, because a CPU transcription of the
// ramp would only ever agree with itself.
//
// Two discriminators. The count of intermediate-coverage samples per stem
// edge: an interval of length L sampled at integer positions with uniform
// subpixel phase contains E[count] = L sample points, so the bare exact-area
// ramp (0.94 px between the 3%/97% pads) yields ~0.94 and never more than one
// sample per edge, while any blur widens it. And the coverage mass across one
// stem: exact-area coverage integrates to the stem's width, so the mass is the
// outline width plus twice the dilation. Glyph advances alone land the stems
// on a narrow set of phases, so the render is repeated with the label shifted
// by 0.2 px steps, which the quarter-pixel pen rounding (GlyphRunEmitter)
// lands on four distinct pen phases. Text contrast is off and the blend target linear, so the
// red channel of white text on black IS the raw rasteriser coverage.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "IsolatedUIFixture.h"
#include "UIRgTestHarness.h"

#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/PassPhase.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"

using GameEngine::UITesting::IsolatedUIFixture;
using namespace GameEngine;
using namespace GameEngine::Rendering;

namespace
{

// Ten lowercase l's: identical straight stems. Per-render fractional
// margin-left offsets stratify the stem subpixel phases (see file comment).
constexpr char kStemXml[] = R"(<uielement id="root">
  <label id="stem">llllllllll</label>
</uielement>)";

// 96px: the stem is ~8 px wide, so its core reaches full coverage (a plateau)
// with room to spare on both sides of the transition bands.
constexpr char kStemCssFmt[] = R"(
#root { display: flex; flex-direction: column; width: 800px; height: 600px; background-color: #000000; }
#stem { font-family: Roboto; font-size: 96px; line-height: 120px; color: #ffffff; margin: 20px 20px 20px %spx; }
)";

constexpr const char* kPhaseMargins[] = {"20.0", "20.2", "20.4", "20.6", "20.8"};

constexpr uint32_t kTargetW = 800;
constexpr uint32_t kTargetH = 600;

float HalfToFloat(uint16_t value)
{
    const uint32_t sign = static_cast<uint32_t>(value & 0x8000u) << 16u;
    uint32_t exponent = (value >> 10u) & 0x1Fu;
    uint32_t mantissa = value & 0x03FFu;

    uint32_t bits;
    if (exponent == 0u)
    {
        if (mantissa == 0u)
        {
            bits = sign;
        }
        else
        {
            exponent = 1u;
            while ((mantissa & 0x0400u) == 0u)
            {
                mantissa <<= 1u;
                --exponent;
            }
            mantissa &= 0x03FFu;
            bits = sign | ((exponent + 112u) << 23u) | (mantissa << 13u);
        }
    }
    else if (exponent == 0x1Fu)
    {
        bits = sign | 0x7F800000u | (mantissa << 13u);
    }
    else
    {
        bits = sign | ((exponent + 112u) << 23u) | (mantissa << 13u);
    }

    float out;
    static_assert(sizeof(out) == sizeof(bits));
    std::memcpy(&out, &bits, sizeof(out));
    return out;
}

// Executes one UI render into a transient RGBA16F target and returns the red
// channel (white text over the pass's black clear: red == coverage), row-major.
std::vector<float> RenderCoverage(UIManager& ui)
{
    IDevice* dev = ui.GetDevice();
    if (!dev)
        return {};

    const size_t pixelCount = static_cast<size_t>(kTargetW) * kTargetH;
    const size_t outBytes = pixelCount * 8; // RGBA16F
    const BufferHandle readback = dev->CreateReadbackBuffer(outBytes, "SlugEdge.Readback");
    if (!readback.IsValid())
        return {};

    std::vector<float> coverage;
    {
        RenderGraph::RGResourcePool persistent(dev);
        RenderGraph::RGTransientPool transient(dev);
        RenderGraph::RGUploadRing ring(dev, 2, 262144);
        RenderGraph::RGFrame frame(dev, &persistent, &transient, &ring);
        frame.BeginFrame(1);

        TextureDesc td{};
        td.width = kTargetW;
        td.height = kTargetH;
        td.mipLevels = 1;
        td.arrayLayers = 1;
        td.sampleCount = 1;
        td.format = static_cast<uint32_t>(TextureFormat::R16G16B16A16_FLOAT);
        td.usage = static_cast<uint32_t>(TextureUsage::RenderTarget) |
                   static_cast<uint32_t>(TextureUsage::ShaderResource) |
                   static_cast<uint32_t>(TextureUsage::TransferSrc);
        td.debugName = "SlugEdge.Target";
        const RenderGraph::RGTexture target = frame.CreateTexture("SlugEdge.Target", td);
        // Deliberately LinearSdr while the harness itself is flipped (#767):
        // this test measures the RASTERISER's edge model, and the
        // white-on-black linear render is the one configuration where the red
        // channel read back IS raw coverage. An encoded target would put the
        // OETF between the rasteriser and the measurement and turn the 3%/97%
        // pads into different coverage cut points — a distorted instrument,
        // not a stronger test.
        if (!target.IsValid() || !ui.RenderRG(frame, target, UI::UITargetSpace::LinearSdr()))
            return {};

        frame.AddPass(
            "SlugEdge.Readback", PassPhase::kFinalize,
            [&](RenderGraph::RGPassBuilder& p)
            {
                p.Read(target, RenderGraph::RGTextureRead::CopySrc);
                p.PreventCulling(); // consumed by the CPU, not the graph
            },
            [target, readback](RenderGraph::RGContext& ctx)
            {
                ctx.Cmd->CopyTextureSubresourceToBuffer(ctx.GetTexture(target), 0, 0, readback,
                                                        kTargetW, kTargetH);
            });

        frame.Execute();
        dev->WaitForIdle();

        const void* mapped = dev->MapBuffer(readback);
        if (mapped)
        {
            const uint16_t* halves = static_cast<const uint16_t*>(mapped);
            coverage.resize(pixelCount);
            for (size_t i = 0; i < pixelCount; ++i)
                coverage[i] = HalfToFloat(halves[i * 4]); // red channel
            dev->UnmapBuffer(readback);
        }
    }
    dev->DestroyBuffer(readback);
    return coverage;
}

// Intermediate coverage: clearly off the background and clearly short of the
// plateau. The 0.03/0.97 pads keep plateau noise and background bleed out of
// the count; the expected footprint below accounts for the ramp length they
// exclude.
bool Intermediate(float v)
{
    return v > 0.03f && v < 0.97f;
}

struct EdgeFootprints
{
    int Edges = 0;
    int IntermediateSamples = 0;
    int WidestEdge = 0;
    int Stems = 0;
    double StemMass = 0.0;
};

// For every full-coverage stem core in the row (a run of >= 2 samples at
// plateau), counts the contiguous intermediate samples flanking it on each
// side (the sampled footprint of one edge transition) and sums the coverage
// across the stem and one sample past each flank (its width in px).
void ScanRow(const std::vector<float>& cov, uint32_t rowStart, uint32_t width, EdgeFootprints& out)
{
    uint32_t x = 0;
    while (x < width)
    {
        if (cov[rowStart + x] < 0.97f)
        {
            ++x;
            continue;
        }
        uint32_t runEnd = x;
        while (runEnd + 1 < width && cov[rowStart + runEnd + 1] >= 0.97f)
            ++runEnd;
        if (runEnd > x) // plateau of >= 2 samples: a resolved stem core
        {
            int left = 0;
            for (uint32_t c = x; c > 0 && Intermediate(cov[rowStart + c - 1]); --c)
                ++left;
            int right = 0;
            for (uint32_t c = runEnd; c + 1 < width && Intermediate(cov[rowStart + c + 1]); ++c)
                ++right;
            out.Edges += 2;
            out.IntermediateSamples += left + right;
            out.WidestEdge = std::max({out.WidestEdge, left, right});
            const uint32_t first = x - static_cast<uint32_t>(left);
            const uint32_t last = runEnd + static_cast<uint32_t>(right);
            if (first > 0 && last + 1 < width)
            {
                double mass = 0.0;
                for (uint32_t c = first - 1; c <= last + 1; ++c)
                    mass += cov[rowStart + c];
                ++out.Stems;
                out.StemMass += mass;
            }
        }
        x = runEnd + 1;
    }
}

// Renders one phase stratum and accumulates its stem-edge footprints.
// Returns false only for the device-missing / font-missing skip cases,
// with `why` naming which.
bool MeasureStratum(const char* margin, EdgeFootprints& fp, int& strataSamples, std::string& why)
{
    char css[512];
    snprintf(css, sizeof(css), kStemCssFmt, margin);

    IsolatedUIFixture fx;
    const bool built = fx.Build(1.0f, kStemXml, css);
    if (!fx.DeviceAvailable())
    {
        why = "No Vulkan device available";
        return false;
    }
    EXPECT_TRUE(built) << fx.Diagnostic();
    if (!built)
        return true;
    if (fx.ResolvedFontFamily("stem").find("Roboto") == std::string::npos)
    {
        why = "Staged Roboto-Regular.ttf not found next to the test executable";
        return false;
    }

    // Raw coverage: no contrast term, and a linear target on the linear
    // attachment, which makes the blend-space retarget the identity.
    fx.Manager().SetTextContrast(0.0f);
    fx.Manager().SetTextBlendGamma(0.0f);
    const std::vector<float> cov = RenderCoverage(fx.Manager());
    EXPECT_FALSE(cov.empty()) << "UI render/readback produced no pixels";
    if (cov.empty())
        return true;

    // Rows carrying stem ink; measure only the vertically interior half of
    // that band so every scanned row crosses full-height stem cores, not the
    // curved stem ends or the baseline overshoot.
    int inkTop = -1;
    int inkBottom = -1;
    for (uint32_t y = 0; y < kTargetH; ++y)
    {
        for (uint32_t x = 0; x < kTargetW; ++x)
        {
            if (cov[static_cast<size_t>(y) * kTargetW + x] >= 0.97f)
            {
                if (inkTop < 0)
                    inkTop = static_cast<int>(y);
                inkBottom = static_cast<int>(y);
                break;
            }
        }
    }
    EXPECT_GE(inkTop, 0) << "no full-coverage ink found in the render";
    if (inkTop < 0)
        return true;
    const int bandH = inkBottom - inkTop + 1;
    EXPECT_GE(bandH, 20) << "ink band unexpectedly short for 96px stems";

    const int before = fp.IntermediateSamples;
    const int rowFirst = inkTop + bandH / 4;
    const int rowLast = inkBottom - bandH / 4;
    for (int y = rowFirst; y <= rowLast; ++y)
        ScanRow(cov, static_cast<uint32_t>(y) * kTargetW, kTargetW, fp);
    strataSamples = fp.IntermediateSamples - before;
    return true;
}

} // namespace

TEST(SlugEdgeProfile, StemEdgesAreDilatedExactAreaRamps)
{
    EdgeFootprints fp;
    std::vector<int> perStratum;
    for (const char* margin : kPhaseMargins)
    {
        int stratumSamples = 0;
        std::string why;
        if (!MeasureStratum(margin, fp, stratumSamples, why))
            GTEST_SKIP() << why;
        perStratum.push_back(stratumSamples);
    }

    ASSERT_GE(fp.Edges, 500) << "too few resolved stem edges — layout or font changed under the test";
    ASSERT_GE(fp.Stems, 200) << "too few whole stems to weigh";

    // The strata only stratify if the fractional margins actually shift the
    // stems: identical per-stratum totals across all five means x got snapped
    // to whole pixels somewhere and the phase coverage silently collapsed.
    const bool allIdentical =
        std::all_of(perStratum.begin(), perStratum.end(),
                    [&](int v) { return v == perStratum.front(); });
    EXPECT_FALSE(allIdentical)
        << "all phase strata measured identically — fractional x offsets are being snapped";

    const double meanFootprint =
        static_cast<double>(fp.IntermediateSamples) / static_cast<double>(fp.Edges);

    // The bare exact-area ramp spans 0.94 px between the 3%/97% pads, so its
    // phase-averaged footprint is ~0.94 samples per edge. 0.80 / 1.10 bound
    // that with margin for the quarter-pixel stratification granularity and pad
    // jitter; a reconstruction blur of 0.3 px or more lands above.
    EXPECT_GE(meanFootprint, 0.80) << "stem edges are sharper than an exact-area ramp";
    EXPECT_LE(meanFootprint, 1.10) << "stem edges are blurred past the exact-area ramp";

    // A 0.94 px span covers at most one integer sample position.
    EXPECT_LE(fp.WidestEdge, 1) << "an edge transition spans more samples than an exact-area ramp";

    // Roboto Regular's 'l' is a rectangle 185 units wide on a 2048-unit em:
    // 8.672 px at 96 px. The dilation at 96 px is the 0.30 px cap on each side,
    // so each stem carries 8.672 + 0.60 = 9.272 px of coverage. 0.06 px is
    // the half-float readback and stratification tolerance; the undilated
    // outline (8.67) and a half-strength dilation (8.97) both fall outside.
    constexpr double kOutlineWidthPx = 185.0 / 2048.0 * 96.0;
    constexpr double kChromeDilationPx = 0.30;
    const double meanStemMass = fp.StemMass / static_cast<double>(fp.Stems);
    EXPECT_NEAR(meanStemMass, kOutlineWidthPx + 2.0 * kChromeDilationPx, 0.06)
        << "stem width does not carry the outline dilation measured out of Chrome on macOS";
}

namespace
{
// Editor-size stems: one 'l' per ink run, separated by background.
constexpr char kSmallStemCssFmt[] = R"(
#root { display: flex; flex-direction: column; width: 800px; height: 600px; background-color: #000000; }
#stem { font-family: Roboto; font-size: %dpx; line-height: 60px; letter-spacing: 4px; color: #ffffff; margin: 20px 20px 20px %spx; }
)";

// Mean coverage mass of the ink runs on the interior rows of the stems, over
// the phase strata: the stem width in px.
bool MeasureSmallStemMass(int fontPx, double& meanMass, int& runs, std::string& why)
{
    double total = 0.0;
    runs = 0;
    for (const char* margin : kPhaseMargins)
    {
        char css[512];
        snprintf(css, sizeof(css), kSmallStemCssFmt, fontPx, margin);
        IsolatedUIFixture fx;
        const bool built = fx.Build(1.0f, kStemXml, css);
        if (!fx.DeviceAvailable())
        {
            why = "No Vulkan device available";
            return false;
        }
        EXPECT_TRUE(built) << fx.Diagnostic();
        if (!built)
            return true;
        fx.Manager().SetTextContrast(0.0f);
        fx.Manager().SetTextBlendGamma(0.0f);
        const std::vector<float> cov = RenderCoverage(fx.Manager());
        if (cov.empty())
            return true;
        int inkTop = -1;
        int inkBottom = -1;
        for (uint32_t y = 0; y < kTargetH; ++y)
            for (uint32_t x = 0; x < kTargetW; ++x)
                if (cov[static_cast<size_t>(y) * kTargetW + x] >= 0.5f)
                {
                    inkTop = inkTop < 0 ? static_cast<int>(y) : inkTop;
                    inkBottom = static_cast<int>(y);
                    break;
                }
        if (inkTop < 0)
            return true;
        const int bandH = inkBottom - inkTop + 1;
        for (int y = inkTop + bandH / 4; y <= inkBottom - bandH / 4; ++y)
        {
            const size_t row = static_cast<size_t>(y) * kTargetW;
            double run = 0.0;
            for (uint32_t x = 0; x < kTargetW; ++x)
            {
                const float v = cov[row + x];
                if (v > 0.003f)
                    run += v;
                else if (run > 0.0)
                {
                    total += run;
                    ++runs;
                    run = 0.0;
                }
            }
        }
    }
    meanMass = runs > 0 ? total / runs : 0.0;
    return true;
}
} // namespace

// At editor sizes the dilation slope is the whole effect: 0.0058 px per ppem,
// 0.070 px per side at 12 px and 0.139 px at 24 px (Chrome 155, macOS 15.7).
// Each Roboto Regular 'l' (185 / 2048 em wide) must carry its outline width
// plus twice that. 0.035 px holds the readback and stratification noise; half
// the slope (0.07 and 0.14 px less ink) falls outside.
TEST(SlugEdgeProfile, EditorSizeStemsCarryTheChromeDilation)
{
    for (const int fontPx : {12, 24})
    {
        double meanMass = 0.0;
        int runs = 0;
        std::string why;
        if (!MeasureSmallStemMass(fontPx, meanMass, runs, why))
            GTEST_SKIP() << why;
        ASSERT_GE(runs, 100) << fontPx << " px: too few stem runs";
        const double outline = 185.0 / 2048.0 * fontPx;
        const double dilation = 0.0058 * fontPx;
        EXPECT_NEAR(meanMass, outline + 2.0 * dilation, 0.035) << fontPx << " px";
    }
}
