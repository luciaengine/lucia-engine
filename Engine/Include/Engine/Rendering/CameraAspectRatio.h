#pragma once

#include "Components/Rendering/Camera.h"
#include "Rendering/CameraTypes.h"
#include "Types/Types.h"
#include "Mathematics/Rect.h"

#include <string>

namespace GameEngine::Engine::Renderer
{

enum class CameraAspectPreset : uint32
{
    Native = 0,
    Ratio16x9 = 1,
    Ratio4x3 = 2,
    Ratio16x10 = 3,
    Ratio21x9 = 4,
    Custom = 5,
};

struct ViewLetterbox
{
    bool active = false;
    uint32 x = 0;
    uint32 y = 0;
    uint32 width = 0;
    uint32 height = 0;
};

struct CameraAspectResolution
{
    float projectionAspect = 16.0f / 9.0f;
    ViewLetterbox letterbox{};
};

// Per-view state driving the smooth sub-pixel pixel-perfect upscale path.
//
// When Active, the camera renders its world + post-FX into an offscreen render
// target sized PaddedWidth x PaddedHeight (the reference resolution plus a
// one-texel border on every side). The upscale pass then point-samples a
// ReferenceWidth x ReferenceHeight window out of that RT, shifted by the
// sub-pixel remainder (FracX/FracY, in texels of [-0.5, 0.5]) so the whole
// image scrolls smoothly, and blits the result at integer Zoom into the
// centered output rect (letterbox/pillarbox is the remaining screen).
struct PixelPerfectViewState
{
    bool Active = false;
    uint32 ReferenceWidth = 0;
    uint32 ReferenceHeight = 0;
    // Reference resolution + 1-texel border on each axis (the offscreen RT size).
    uint32 PaddedWidth = 0;
    uint32 PaddedHeight = 0;
    // Largest integer factor that fits ReferenceWidth/Height in the swapchain.
    uint32 Zoom = 1;
    // Sub-pixel camera remainder in source texels, range [-0.5, 0.5] per axis.
    float FracX = 0.0f;
    float FracY = 0.0f;
    // Texel size in world units (1 / pixels-per-unit): the grid the snapped
    // camera sits on, and the grid a sprite has to sit on to stay crisp.
    float WorldUnitsPerTexel = 0.0f;
    // The viewport this state was computed against — the extent the upscale
    // writes, letterbox bars included. This is the pipeline's OUTPUT extent for
    // the view: the render half rasters at PaddedWidth/Height and everything
    // past the crossing (bloom, tonemap, the LDR display effects) runs here.
    uint32 DisplayWidth = 0;
    uint32 DisplayHeight = 0;
};

float GetCameraAspectPresetRatio(CameraAspectPreset preset);
CameraAspectPreset GetCameraAspectPreset(const Components::Camera& camera);
std::string GetCameraAspectPresetDropdownValue(const Components::Camera& camera);
CameraAspectPreset CameraAspectPresetFromDropdownValue(const std::string& value);

CameraAspectResolution ResolveCameraAspect(const Components::Camera& camera,
                                           uint32_t viewportWidth,
                                           uint32_t viewportHeight);

// Resolve the offscreen-RT dimensions and integer zoom for a (potentially)
// pixel-perfect camera, WITHOUT snapping the camera or computing the sub-pixel
// remainder. Returns an inactive state when the camera isn't pixel-perfect
// orthographic. Callers size the offscreen render target from this before the
// view is finalized; ApplyActiveCameraAspect publishes the full state (with
// FracX/FracY) onto the view during projection setup.
PixelPerfectViewState ComputePixelPerfectViewState(const Components::Camera& camera,
                                                   uint32_t viewportWidth,
                                                   uint32_t viewportHeight);

ViewLetterbox ComputeLetterboxRect(uint32_t viewportWidth,
                                 uint32_t viewportHeight,
                                 float targetAspect);

// Camera rectangle in render-target UV; inactive or degenerate inputs use the whole target.
Mathematics::Rect ComputeViewportRect(const ViewLetterbox& letterbox,
                                      uint32_t targetWidth, uint32_t targetHeight);

class RenderServices;

struct Camera;

void ApplyActiveCameraAspect(RenderServices& rs,
                             ::GameEngine::Rendering::ViewId viewId,
                             ::GameEngine::Rendering::CameraId cameraId,
                             const Camera& activeCamera,
                             uint32_t viewportWidth,
                             uint32_t viewportHeight);

} // namespace GameEngine::Engine::Renderer
