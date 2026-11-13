// The grass palette and presets: the automatic root and the default palette in linear light (the
// surface's decode is ColorSRGB's, GrassColorDecodeTests.cpp), and the presets
// (TerrainGrass/TerrainGrassPresets.h): Meadow is the component's defaults, a preset writes its whole
// look and no scene dial, and an edit away from the picked preset reads Custom.

#include <gtest/gtest.h>

#include "TerrainGrass/TerrainGrassPresets.h"

#include "Types/Color.h"

#include <algorithm>

namespace GameEngine::TerrainGrass
{
namespace
{
ColorLinear DecodeArgb(uint32 argb)
{
    return ColorSRGB::FromSRGB255(static_cast<uint8>((argb >> 16) & 0xFFu), static_cast<uint8>((argb >> 8) & 0xFFu),
                                  static_cast<uint8>(argb & 0xFFu))
        .ToLinear();
}

float32 Luminance(const ColorLinear& c)
{
    return 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b;
}

// Every look dial a preset writes, field for field.
void ExpectSameLook(const Components::TerrainGrass& actual, const Components::TerrainGrass& expected)
{
    for (const auto dial : Detail::kTerrainGrassPresetFloatDials)
        EXPECT_FLOAT_EQ(actual.*dial, expected.*dial);
    for (const auto dial : Detail::kTerrainGrassPresetColorDials)
        EXPECT_EQ(actual.*dial, expected.*dial);
}

} // namespace

// Auto Root Color is the tip in shade: kTerrainGrassAutoRootLuminance of the tip's luminance in
// linear light, at the tip's chromaticity, within the 8-bit encode's rounding.
TEST(TerrainGrassPalette, AnAutomaticRootKeepsTheTipsChromaticityAtTheRootLuminanceInLinearLight)
{
    for (const uint32 tip : {0xFF79954Du, 0xFFB3DB4Du, 0xFF9CD334u, 0xFF6C8E4Bu, 0xFFA29B61u, 0xFFFFFFFFu})
    {
        const ColorLinear tipLinear = DecodeArgb(tip);
        const ColorLinear rootLinear = DecodeArgb(Components::TerrainGrassAutoRootColor(tip));
        EXPECT_NEAR(Luminance(rootLinear) / Luminance(tipLinear), Components::kTerrainGrassAutoRootLuminance, 0.02f)
            << std::hex << "tip 0x" << tip;
        const float32 tipSum = tipLinear.r + tipLinear.g + tipLinear.b;
        const float32 rootSum = rootLinear.r + rootLinear.g + rootLinear.b;
        EXPECT_NEAR(rootLinear.g / rootSum, tipLinear.g / tipSum, 0.02f) << std::hex << "tip 0x" << tip;
        EXPECT_NEAR(rootLinear.b / rootSum, tipLinear.b / tipSum, 0.02f) << std::hex << "tip 0x" << tip;
    }
}

// The default palette: a leaf tip and a brighter yellow-green back-light, in linear light.
TEST(TerrainGrassPalette, TheDefaultPaletteIsALeafAndAYellowGreenBacklight)
{
    const Components::TerrainGrass defaults{};
    const ColorLinear tip = DecodeArgb(defaults.TipColor);
    const ColorLinear backlight = DecodeArgb(defaults.BacklightColor);
    constexpr float32 kByte = 1.0f / 255.0f;
    EXPECT_NEAR(tip.r, 0.19f, kByte);
    EXPECT_NEAR(tip.g, 0.30f, kByte);
    EXPECT_NEAR(tip.b, 0.075f, kByte);
    EXPECT_NEAR(backlight.r, 0.50f, 2.0f * kByte);
    EXPECT_NEAR(backlight.g, 0.62f, 2.0f * kByte);
    EXPECT_NEAR(backlight.b, 0.12f, 2.0f * kByte);
}

TEST(TerrainGrassPresets, MeadowIsTheComponentDefaults)
{
    const Components::TerrainGrass defaults{};
    EXPECT_EQ(defaults.Preset, Components::TerrainGrassPreset::Meadow);
    Components::TerrainGrass picked{};
    ApplyTerrainGrassPreset(picked, Components::TerrainGrassPreset::Meadow);
    ExpectSameLook(picked, defaults);
}

// Lawn and Steppe set their palette, density, height, width and tilt, and every other look dial is
// the component's default.
TEST(TerrainGrassPresets, LawnAndSteppeSetTheirPaletteAndBladesOverTheDefaults)
{
    Components::TerrainGrass lawn{};
    lawn.BladesPerSquareMeter = 64.0f;
    lawn.BladeHeight = 0.4f;
    lawn.BladeWidth = 0.024f;
    lawn.BladeTilt = 0.1f;
    lawn.TipColor = 0xFF6C8E4Bu;
    lawn.BacklightColor = 0xFFAAC859u;
    Components::TerrainGrass picked{};
    ApplyTerrainGrassPreset(picked, Components::TerrainGrassPreset::Lawn);
    ExpectSameLook(picked, lawn);

    Components::TerrainGrass steppe{};
    steppe.BladesPerSquareMeter = 12.0f;
    steppe.BladeHeight = 0.9f;
    steppe.BladeWidth = 0.02f;
    steppe.BladeTilt = 0.35f;
    steppe.TipColor = 0xFFA29B61u;
    steppe.BacklightColor = 0xFFCEC581u;
    picked = {};
    ApplyTerrainGrassPreset(picked, Components::TerrainGrassPreset::Steppe);
    ExpectSameLook(picked, steppe);
}

// Stylized is short, dense and moss green, with a set root color in place of the automatic one, little
// color variation, large soft clumps, a small flutter and less light through the blades.
TEST(TerrainGrassPresets, StylizedIsAnEvenMossGreenFieldInSoftClumps)
{
    Components::TerrainGrass picked{};
    ApplyTerrainGrassPreset(picked, Components::TerrainGrassPreset::Stylized);
    EXPECT_EQ(picked.Preset, Components::TerrainGrassPreset::Stylized);
    EXPECT_EQ(picked.TipColor, 0xFF507338u);
    EXPECT_EQ(picked.RootColor, 0xFF4C702Eu);
    EXPECT_EQ(picked.BacklightColor, 0xFF708E54u);
    EXPECT_FLOAT_EQ(picked.BladesPerSquareMeter, 128.0f);
    EXPECT_FLOAT_EQ(picked.BladeHeight, 0.5f);
    EXPECT_FLOAT_EQ(picked.BladeWidth, 0.032f);
    EXPECT_FLOAT_EQ(picked.BladeTilt, 0.2f);
    EXPECT_FLOAT_EQ(picked.ClumpSize, 1.8f);
    EXPECT_FLOAT_EQ(picked.ClumpHeightVariance, 0.45f);
    EXPECT_FLOAT_EQ(picked.ClumpAlignment, 0.3f);
    EXPECT_FLOAT_EQ(picked.BladeBend, 0.25f);
    EXPECT_FLOAT_EQ(picked.BladeTwist, 0.1f);
    EXPECT_FLOAT_EQ(picked.BladeTaperStart, 0.55f);
    EXPECT_FLOAT_EQ(picked.TuftFan, 0.15f);
    EXPECT_FLOAT_EQ(picked.RandomScale, 0.3f);
    EXPECT_FLOAT_EQ(picked.WindRestingLean, 0.19f);
    EXPECT_FLOAT_EQ(picked.WindFlutterAmount, 0.1f);
    EXPECT_FLOAT_EQ(picked.RandomBrightness, 0.1f);
    EXPECT_FLOAT_EQ(picked.HueVariation, 0.035f);
    EXPECT_FLOAT_EQ(picked.Translucency, 0.35f);
    EXPECT_FLOAT_EQ(picked.RootFadeEnd, 0.3f);
    EXPECT_FLOAT_EQ(picked.CanopyShade, 0.85f);
}

TEST(TerrainGrassPresets, APresetWritesItsLookAndNoSceneDialAndAnEditAwayFromItReadsCustom)
{
    for (const Components::TerrainGrass& look : kTerrainGrassPresets)
    {
        Components::TerrainGrass grass{};
        grass.HueVariation = 0.9f;
        grass.Range = 80.0f;
        grass.MaskThreshold = 0.1f;
        grass.WindDirection = 1.0f;
        grass.WindStrength = 0.4f;
        grass.DensityFalloff = 3.0f;
        ApplyTerrainGrassPreset(grass, look.Preset);
        EXPECT_EQ(grass.Preset, look.Preset);
        ExpectSameLook(grass, look);
        EXPECT_FLOAT_EQ(grass.Range, 80.0f) << "a preset wrote a scene dial";
        EXPECT_FLOAT_EQ(grass.MaskThreshold, 0.1f) << "a preset wrote a scene dial";
        EXPECT_FLOAT_EQ(grass.WindDirection, 1.0f) << "a preset wrote a scene dial";
        EXPECT_FLOAT_EQ(grass.WindStrength, 0.4f) << "a preset wrote a scene dial";
        EXPECT_FLOAT_EQ(grass.DensityFalloff, 3.0f) << "a preset wrote a scene dial";

        ReconcileTerrainGrassPreset(grass);
        EXPECT_EQ(grass.Preset, look.Preset) << "an unedited preset reads Custom";
        grass.HueVariation += 0.05f;
        ReconcileTerrainGrassPreset(grass);
        EXPECT_EQ(grass.Preset, Components::TerrainGrassPreset::Custom);
    }
    // Custom names the dials as authored and writes none of them.
    Components::TerrainGrass grass{};
    grass.TipColor = 0xFF123456u;
    ApplyTerrainGrassPreset(grass, Components::TerrainGrassPreset::Custom);
    EXPECT_EQ(grass.Preset, Components::TerrainGrassPreset::Custom);
    EXPECT_EQ(grass.TipColor, 0xFF123456u);
}

} // namespace GameEngine::TerrainGrass
