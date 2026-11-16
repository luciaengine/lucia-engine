#pragma once

#include "Mathematics/Octahedral.h"
#include "Mathematics/Vector3.h"
#include "Rendering/Camera/CameraTypes.h"

#include <cstdint>

namespace GameEngine::Mathematics
{
struct BoundingBox;
}

namespace GameEngine::Engine::Renderer
{

/// The screen axes of an impostor frame seen from `viewDirection` (unit, from the object toward
/// the camera). Up is +Y, or +Z for a view within about 8 degrees of straight up or down, where
/// +Y has no stable horizontal. The bake and impostor_surface.glsl use the same rule.
struct ImpostorFrameAxes
{
    Mathematics::Vector3 Right;
    Mathematics::Vector3 Up;
};

ImpostorFrameAxes MakeImpostorFrameAxes(const Mathematics::Vector3& viewDirection);

/// The orthographic camera that bakes frame (column, row) of an impostor atlas: it looks at the
/// center of `bounds` from the frame's direction, and its square view and depth range just hold
/// the bounding sphere. Reverse-Z, so the window depth is 1 at the sphere's near side and 0 at its
/// far side, linear in between.
Rendering::CameraData MakeImpostorFrameCamera(Mathematics::OctahedralLayout layout,
                                              uint32_t column,
                                              uint32_t row,
                                              uint32_t framesPerSide,
                                              const Mathematics::BoundingBox& bounds);

} // namespace GameEngine::Engine::Renderer
