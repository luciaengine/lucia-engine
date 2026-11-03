#pragma once
#include "Types/Types.h"

namespace GameEngine::Components {
// Fixed-height or screen-divisor resolution for 1..10 camera-distance bands.
// Near/Middle/Far fields retain the first three bands for scene compatibility.
// DistanceMode chooses forward depth or radial distance. Sky has its own grid.
struct DepthPixelationEffect {
    bool Enabled{true};
    // Off pins the effect off wherever its volume applies: a value the volume blend reads.
    static constexpr bool KeepsOwnEnabledField = true;
    float32 Intensity{1.0f}; // Legacy scene field; Enabled controls the effect.
    int32 AutoBandResolution{0};
    int32 AutoFarPixelSize{1}; // Farthest band: 1x or 2x screen pixels.
    int32 SimpleScreenSpace{0}; // Comparison: pixelate the parent color/depth instead of rendering bands.
    int32 StabilizationMode{1}; // 0: Off, 1: Auto.
    float32 NearEnd{10.0f};
    float32 MiddleEnd{40.0f};
    float32 NearResolution{360.0f};
    float32 MiddleResolution{180.0f};
    float32 FarResolution{90.0f};
    float32 SkyResolution{90.0f};
    int32 DistanceMode{0}; // 0: camera-forward depth, 1: radial camera distance
    int32 BandCount{3};
    float32 Band3End{100.0f};
    float32 Band4End{160.0f};
    float32 Band5End{240.0f};
    float32 Band6End{360.0f};
    float32 Band7End{520.0f};
    float32 Band8End{720.0f};
    float32 Band9End{1000.0f};
    float32 Band4Resolution{90.0f};
    float32 Band5Resolution{90.0f};
    float32 Band6Resolution{90.0f};
    float32 Band7Resolution{90.0f};
    float32 Band8Resolution{90.0f};
    float32 Band9Resolution{90.0f};
    float32 Band10Resolution{90.0f};
    float32 Band4Divisor{4.0f};
    float32 Band5Divisor{4.0f};
    float32 Band6Divisor{4.0f};
    float32 Band7Divisor{4.0f};
    float32 Band8Divisor{4.0f};
    float32 Band9Divisor{4.0f};
    float32 Band10Divisor{4.0f};
    int32 ResolutionMode{0};
    float32 NearDivisor{24.0f};
    float32 MiddleDivisor{10.0f};
    float32 FarDivisor{4.0f};
    float32 SkyDivisor{4.0f};
    int32 DebugRanges{0}; // 0: effect, 1: depth range colors
};
} // namespace GameEngine::Components
