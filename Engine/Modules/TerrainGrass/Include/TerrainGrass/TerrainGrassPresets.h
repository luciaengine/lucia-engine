#pragma once

// A preset is a whole field look that the Inspector writes once; the dials stay editable, and an edit
// away from the picked preset turns Preset to Custom. A scene stores and loads Preset as written, and
// writing a dial by any other path leaves it alone.

#include "Components/Terrain/TerrainGrass.h"

#include <algorithm>
#include <cmath>

namespace GameEngine::TerrainGrass
{

// Each preset is a whole component: its look dials are the preset, and a dial it does not name keeps
// the component's default. Colors are sRGB bytes, as the picker writes them; the linear albedo each
// one decodes to is in its comment.
inline const Components::TerrainGrass kTerrainGrassPresets[] = {
    // Meadow, the component's defaults: a leaf green (0.19, 0.30, 0.075) on 0.72 m blades, 21 per
    // square meter at 36 mm so the field covers the ground from eye height and from a strategy
    // camera within the instance budget's 369 m.
    {.Preset = Components::TerrainGrassPreset::Meadow},
    // Lawn: short, upright and dense, a deeper green (0.15, 0.27, 0.07). 64 per square meter fit
    // the budget to about 211 m.
    {.Preset = Components::TerrainGrassPreset::Lawn,
     .BladesPerSquareMeter = 64.0f,
     .BladeHeight = 0.4f,
     .BladeWidth = 0.024f,
     .BladeTilt = 0.1f,
     .TipColor = 0xFF6C8E4Bu,
     .BacklightColor = 0xFFAAC859u},
    // Steppe: sparse, tall, wiry and dry, a straw green (0.36, 0.33, 0.12) that leans loosely.
    {.Preset = Components::TerrainGrassPreset::Steppe,
     .BladesPerSquareMeter = 12.0f,
     .BladeHeight = 0.9f,
     .BladeWidth = 0.02f,
     .BladeTilt = 0.35f,
     .TipColor = 0xFFA29B61u,
     .BacklightColor = 0xFFCEC581u},
    // Stylized: short, dense and even. A moss green tip (0.08, 0.17, 0.04) over a set root
    // (0.07, 0.16, 0.03), little color variation, large soft clumps, a small flutter and less light
    // through the blades.
    {.Preset = Components::TerrainGrassPreset::Stylized,
     .BladesPerSquareMeter = 128.0f,
     .ClumpSize = 1.8f,
     .ClumpHeightVariance = 0.45f,
     .ClumpAlignment = 0.3f,
     .BladeHeight = 0.5f,
     .BladeWidth = 0.032f,
     .BladeTilt = 0.2f,
     .BladeBend = 0.25f,
     .BladeTwist = 0.1f,
     .BladeTaperStart = 0.55f,
     .TuftFan = 0.15f,
     .RandomScale = 0.3f,
     .WindRestingLean = 0.19f,
     .WindFlutterAmount = 0.1f,
     .RandomBrightness = 0.1f,
     .HueVariation = 0.035f,
     .RootFadeEnd = 0.3f,
     .CanopyShade = 0.85f,
     .Translucency = 0.35f,
     .RootColor = 0xFF4C702Eu,
     .TipColor = 0xFF507338u,
     .BacklightColor = 0xFF708E54u},
};

namespace Detail
{
// The look dials a preset sets. A preset leaves the scene's dials alone: Range, Density Falloff, the
// layer, Mask Threshold, the seeds, the wind's strength, gusts and heading, the ground bake, Splat
// Root Color, Blade Segments, the render mode and the textures.
inline constexpr float32 Components::TerrainGrass::*kTerrainGrassPresetFloatDials[] = {
    &Components::TerrainGrass::BladesPerSquareMeter, &Components::TerrainGrass::ClumpSize,
    &Components::TerrainGrass::ClumpHeightVariance,  &Components::TerrainGrass::ClumpAlignment,
    &Components::TerrainGrass::ClumpTaper,           &Components::TerrainGrass::BladeHeight,
    &Components::TerrainGrass::BladeWidth,           &Components::TerrainGrass::BladeTilt,
    &Components::TerrainGrass::BladeBend,            &Components::TerrainGrass::BladeTwist,
    &Components::TerrainGrass::BladeTaperStart,      &Components::TerrainGrass::TuftFan,
    &Components::TerrainGrass::RandomScale,          &Components::TerrainGrass::WindRestingLean,
    &Components::TerrainGrass::WindFlutterAmount,    &Components::TerrainGrass::WindFlutterSpeed,
    &Components::TerrainGrass::Brightness,           &Components::TerrainGrass::RandomBrightness,
    &Components::TerrainGrass::HueVariation,         &Components::TerrainGrass::RootFadeStart,
    &Components::TerrainGrass::RootFadeEnd,          &Components::TerrainGrass::GroundingStrength,
    &Components::TerrainGrass::GroundTintHeight,     &Components::TerrainGrass::CanopyShade,
    &Components::TerrainGrass::Translucency,
};
inline constexpr uint32 Components::TerrainGrass::*kTerrainGrassPresetColorDials[] = {
    &Components::TerrainGrass::RootColor,
    &Components::TerrainGrass::TipColor,
    &Components::TerrainGrass::BacklightColor,
};

inline const Components::TerrainGrass* FindTerrainGrassPreset(Components::TerrainGrassPreset preset)
{
    for (const Components::TerrainGrass& look : kTerrainGrassPresets)
    {
        if (look.Preset == preset)
            return &look;
    }
    return nullptr;
}

inline bool TerrainGrassPresetValueMatches(float32 value, float32 preset)
{
    // Tolerates the scene text's %g round trip.
    return std::abs(value - preset) <= 1e-5f * std::max(1.0f, std::abs(preset));
}
} // namespace Detail

// Writes the preset's look dials and names it. Custom writes nothing but the name.
inline void ApplyTerrainGrassPreset(Components::TerrainGrass& grass, Components::TerrainGrassPreset preset)
{
    grass.Preset = preset;
    const Components::TerrainGrass* look = Detail::FindTerrainGrassPreset(preset);
    if (!look)
        return;
    for (const auto dial : Detail::kTerrainGrassPresetFloatDials)
        grass.*dial = look->*dial;
    for (const auto dial : Detail::kTerrainGrassPresetColorDials)
        grass.*dial = look->*dial;
}

// Custom as soon as any look dial differs from the named preset; otherwise unchanged.
inline void ReconcileTerrainGrassPreset(Components::TerrainGrass& grass)
{
    const Components::TerrainGrass* look = Detail::FindTerrainGrassPreset(grass.Preset);
    if (!look)
        return;
    const bool floatsMatch = std::ranges::all_of(Detail::kTerrainGrassPresetFloatDials, [&](const auto dial)
        { return Detail::TerrainGrassPresetValueMatches(grass.*dial, look->*dial); });
    const bool colorsMatch = std::ranges::all_of(Detail::kTerrainGrassPresetColorDials,
                                                 [&](const auto dial) { return grass.*dial == look->*dial; });
    if (!floatsMatch || !colorsMatch)
        grass.Preset = Components::TerrainGrassPreset::Custom;
}

} // namespace GameEngine::TerrainGrass
