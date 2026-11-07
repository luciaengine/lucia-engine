#pragma once

#include "Components/Terrain/TerrainGrass.h"

#include <algorithm>
#include <cmath>
#include <iterator>

namespace GameEngine::TerrainGrass
{

enum class GrassPreset : uint8
{
    Meadow,
    Lawn,
    Steppe,
    Stylized,
    Custom
};

// Each preset is a whole component: its look dials are the preset, and a dial it does not name keeps
// the component's default. Colors are sRGB bytes, as the picker writes them; the linear albedo each
// one decodes to is in its comment.
inline const Components::TerrainGrass kTerrainGrassPresets[] = {
    // Meadow, the component's defaults: a leaf green (0.19, 0.30, 0.075) on 0.72 m blades, 21 per
    // square meter at the default blade width so the field covers the ground from eye height and from a strategy
    // camera within the instance budget's 369 m.
    {},
    // Lawn: short, upright and dense, a deeper green (0.15, 0.27, 0.07). 64 per square meter fit
    // the budget to about 211 m.
    {.BladesPerSquareMeter = 64.0f,
     .BladeHeight = 0.4f,
     .BladeWidth = 0.024f,
     .TipColor = 0xFF6C8E4Bu,
     .BacklightColor = 0xFFAAC859u},
    // Steppe: sparse, tall, wiry and dry, a straw green (0.36, 0.33, 0.12) that leans loosely.
    {.BladesPerSquareMeter = 12.0f,
     .BladeHeight = 0.9f,
     .BladeWidth = 0.02f,
     .TipColor = 0xFFA29B61u,
     .BacklightColor = 0xFFCEC581u},
    // Stylized: short, dense and even. A moss green tip (0.08, 0.17, 0.04) over a set root
    // (0.07, 0.16, 0.03), little color variation, large soft clumps, a small flutter and less light
    // through the blades.
    {.BladesPerSquareMeter = 128.0f,
     .ClumpSize = 1.8f,
     .ClumpHeightVariance = 0.45f,
     .ClumpAlignment = 0.3f,
     .BladeHeight = 0.5f,
     .BladeWidth = 0.032f,
     .RandomScale = 0.3f,
     .WindRestingLean = 0.19f,
     .WindFlutterAmount = 0.1f,
     .RandomBrightness = 0.1f,
     .HueVariation = 0.035f,
     .RootFadeEnd = 0.3f,
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
    &Components::TerrainGrass::BladeHeight,          &Components::TerrainGrass::BladeWidth,
    &Components::TerrainGrass::RandomScale,          &Components::TerrainGrass::WindRestingLean,
    &Components::TerrainGrass::WindFlutterAmount,    &Components::TerrainGrass::WindFlutterSpeed,
    &Components::TerrainGrass::Brightness,           &Components::TerrainGrass::RandomBrightness,
    &Components::TerrainGrass::HueVariation,         &Components::TerrainGrass::RootFadeStart,
    &Components::TerrainGrass::RootFadeEnd,           &Components::TerrainGrass::GroundingStrength,
    &Components::TerrainGrass::Translucency,
};
inline constexpr uint32 Components::TerrainGrass::*kTerrainGrassPresetColorDials[] = {
    &Components::TerrainGrass::RootColor,
    &Components::TerrainGrass::TipColor,
    &Components::TerrainGrass::BacklightColor,
};

inline bool TerrainGrassPresetValueMatches(float32 value, float32 preset)
{
    constexpr float32 kRelativeTolerance = 1e-5f;
    return std::abs(value - preset) <= kRelativeTolerance * std::max(1.0f, std::abs(preset));
}
} // namespace Detail

/// Applies the named look while keeping scene-specific settings such as range and textures.
inline void ApplyTerrainGrassPreset(Components::TerrainGrass& grass, GrassPreset preset)
{
    const auto index = static_cast<size_t>(preset);
    if (index >= std::size(kTerrainGrassPresets))
        return;
    const auto& look = kTerrainGrassPresets[index];
    for (const auto dial : Detail::kTerrainGrassPresetFloatDials)
        grass.*dial = look.*dial;
    for (const auto dial : Detail::kTerrainGrassPresetColorDials)
        grass.*dial = look.*dial;
}

/// Finds the preset that matches the authored look dials.
inline GrassPreset MatchTerrainGrassPreset(const Components::TerrainGrass& grass)
{
    for (size_t index = 0; index < std::size(kTerrainGrassPresets); ++index)
    {
        const auto& look = kTerrainGrassPresets[index];
        const bool floatsMatch = std::ranges::all_of(Detail::kTerrainGrassPresetFloatDials, [&grass, &look](const auto dial)
            { return Detail::TerrainGrassPresetValueMatches(grass.*dial, look.*dial); });
        const bool colorsMatch = std::ranges::all_of(Detail::kTerrainGrassPresetColorDials,
            [&grass, &look](const auto dial) { return grass.*dial == look.*dial; });
        if (floatsMatch && colorsMatch)
            return static_cast<GrassPreset>(index);
    }
    return GrassPreset::Custom;
}

} // namespace GameEngine::TerrainGrass
