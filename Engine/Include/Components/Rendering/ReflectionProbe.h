#pragma once

#include "Rendering/RenderLayerMask.h"
#include "Types/Types.h"

namespace GameEngine {
namespace Components {

enum class ReflectionProbeUpdateMode : uint32 {
    Once = 0,
    Realtime = 1, // @ge-label Realtime (on change)
};

enum class ReflectionProbeInfluenceShape : uint32 { Box = 1, Ellipsoid = 2 };

struct ReflectionProbe {
    // Local influence volume. Like PostProcessVolume, the entity transform owns
    // the probe range: a unit box/sphere is scaled and oriented by WorldTransform.
    // @ge-tooltip Fade width near the influence edge. Visible when the camera is near the probe boundary.
    float32 BlendDistance = 2.0f;
    // @ge-tooltip Tie-breaker when more than one probe influences the camera.
    uint32 Priority = 0;

    // @ge-tooltip The influence volume's shape: a box blends to its faces, an ellipsoid to its surface.
    ReflectionProbeInfluenceShape InfluenceShape = ReflectionProbeInfluenceShape::Box;
    // Projection box in the influence volume's local axes. Extent multipliers
    // and world-unit offsets default to the original transform-owned bounds.
    // @ge-tooltip Size of the parallax projection box along local X, as a multiple of the influence volume.
    float32 ProjectionScaleX = 1.0f; // @ge-range 0.001
    // @ge-tooltip Size of the parallax projection box along local Y, as a multiple of the influence volume.
    float32 ProjectionScaleY = 1.0f; // @ge-range 0.001
    // @ge-tooltip Size of the parallax projection box along local Z, as a multiple of the influence volume.
    float32 ProjectionScaleZ = 1.0f; // @ge-range 0.001
    // @ge-tooltip Offset of the parallax projection box along local X, in meters.
    float32 ProjectionOffsetX = 0.0f;
    // @ge-tooltip Offset of the parallax projection box along local Y, in meters.
    float32 ProjectionOffsetY = 0.0f;
    // @ge-tooltip Offset of the parallax projection box along local Z, in meters.
    float32 ProjectionOffsetZ = 0.0f;
    // @ge-tooltip Offset of the capture and projection origin along the probe's local X axis.
    float32 OriginOffsetX = 0.0f;
    // @ge-tooltip Offset of the capture and projection origin along the probe's local Y axis.
    float32 OriginOffsetY = 0.0f;
    // @ge-tooltip Offset of the capture and projection origin along the probe's local Z axis.
    float32 OriginOffsetZ = 0.0f;

    // Dynamic multiplier applied through EnvData, so changing it does not force a
    // cubemap rebake.
    // @ge-tooltip Runtime IBL multiplier. Updates immediately without rebaking the cubemap.
    float32 Intensity = 1.0f;
    // @ge-tooltip Exposure applied when baking the probe capture.
    float32 ExposureEV = 0.0f;
    // @ge-tooltip Yaw rotation applied to sky/HDRI probe lighting during bake.
    float32 RotationDegrees = 0.0f;
    // @ge-tooltip Darkens lighting sampled from below the horizon. 0 keeps it bright; 1 makes it black.
    float32 IblLowerHemisphereDarkness = 0.3f;

    // Include the global sky/HDRI in captured geometry's environment lighting.
    // Local probes are excluded from every probe capture, even when enabled,
    // so captures cannot feed themselves or form cross-probe feedback loops.
    bool CaptureEnvironment = false;

    // Scene capture behavior. Probes use the active sky/HDRI as background
    // when present, then capture nearby scene content.
    // @ge-tooltip Cubemap face resolution for the scene capture. Higher values sharpen mirror-like reflections but make probe bakes more expensive.
    uint32 CaptureResolution = 256;
    // @ge-tooltip When the probe re-bakes. Once: only when the probe's own settings, the sky or the geometry in its world are added or removed. Realtime (on change): also whenever its world changes: lights, the sky, moved or edited renderables, material edits, and animated or depth-writing content such as skinned characters, terrain, an animated ocean or grass. Shader-time material animation, playing video textures and particles do not re-bake it; set the interval to 0 to capture them every frame.
    ReflectionProbeUpdateMode UpdateMode = ReflectionProbeUpdateMode::Realtime;
    // Shortest time between realtime captures while the probe's world changes (scene edits, lights,
    // materials, render content, shadow casters, dynamic depth); a world at rest is not recaptured.
    // 0 recaptures on every change. All probes share the global face and filter budget.
    // @ge-tooltip Realtime (on change): the fastest the probe may re-bake while the world changes, in seconds; 0 = on every change.
    float32 RealtimeUpdateInterval = 0.5f; // @ge-range 0
    // @ge-tooltip Far clip distance for scene capture. 0 uses the probe volume.
    float32 MaxDistance = 0.0f;
    // @ge-tooltip The render layers the probe's capture draws.
    Rendering::RenderLayerMask CapturedLayers = Rendering::RenderLayerMask::All();
};

} // namespace Components
} // namespace GameEngine
