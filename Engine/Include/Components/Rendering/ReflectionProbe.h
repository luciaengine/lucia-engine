#pragma once

#include "Types/Types.h"

namespace GameEngine {
namespace Components {

enum class ReflectionProbeUpdateMode : uint32 {
    Once = 0,
    Realtime = 1,
};

enum class ReflectionProbeInfluenceShape : uint32 { Box = 1, Ellipsoid = 2 };

struct ReflectionProbe {
    // Local influence volume. Like PostProcessVolume, the entity transform owns
    // the probe range: a unit box/sphere is scaled and oriented by WorldTransform.
    float32 BlendDistance = 2.0f;
    uint32 Priority = 0;

    ReflectionProbeInfluenceShape InfluenceShape = ReflectionProbeInfluenceShape::Box;
    // Projection box in the influence volume's local axes. Extent multipliers
    // and world-unit offsets default to the original transform-owned bounds.
    float32 ProjectionScaleX = 1.0f; // @ge-range 0.001
    float32 ProjectionScaleY = 1.0f; // @ge-range 0.001
    float32 ProjectionScaleZ = 1.0f; // @ge-range 0.001
    float32 ProjectionOffsetX = 0.0f;
    float32 ProjectionOffsetY = 0.0f;
    float32 ProjectionOffsetZ = 0.0f;
    float32 OriginOffsetX = 0.0f;
    float32 OriginOffsetY = 0.0f;
    float32 OriginOffsetZ = 0.0f;

    // Dynamic multiplier applied through EnvData, so changing it does not force a
    // cubemap rebake.
    float32 Intensity = 1.0f;
    float32 ExposureEV = 0.0f;
    float32 RotationDegrees = 0.0f;
    float32 IblLowerHemisphereDarkness = 0.3f;

    // Include the global sky/HDRI in captured geometry's environment lighting.
    // Local probes are excluded from every probe capture, even when enabled,
    // so captures cannot feed themselves or form cross-probe feedback loops.
    bool CaptureEnvironment = false;

    // Scene capture behavior. Probes use the active sky/HDRI as background
    // when present, then capture nearby scene content.
    uint32 CaptureResolution = 256;
    ReflectionProbeUpdateMode UpdateMode = ReflectionProbeUpdateMode::Realtime;
    // Seconds between realtime capture starts. 0 requests continuous refresh;
    // all probes still share the global face/filter work budget.
    float32 RealtimeUpdateInterval = 0.5f; // @ge-range 0
    float32 MaxDistance = 0.0f;
    uint32 CullMask = 0xFFFFFFFFu;
};

} // namespace Components
} // namespace GameEngine
