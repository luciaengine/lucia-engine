#pragma once

#include "Engine/Rendering/RenderServices.h" // RenderServices::WorldPassRG, WorldPassTargetsRG

#include <cstdint>

namespace GameEngine::Engine::Renderer
{

// The most point, spot and area lights one reflection probe capture face shades. The forward shader
// tests each of them for each pixel of the face.
inline constexpr uint32_t kMaxProbeCaptureLocalLights = 64;

// Declares the world pass of one reflection probe capture face with the Forward+ lit variant. One light
// cluster holds the point, spot and area lights whose reach meets the face frustum, strongest first, and
// the pass binds a ViewParams of the face camera. The view's camera, world and LightUBO must be current
// for the face. The face stores absolute radiance, so ViewParams carries exposure 1. Returns an invalid
// pass when an upload fails; the caller then arms the face again.
RenderServices::WorldPassRG AddProbeCaptureWorldPass(RenderServices& services,
                                                     Rendering::RenderGraph::RGFrame& frame,
                                                     Rendering::ViewId viewId,
                                                     const RenderServices::WorldPassTargetsRG& targets,
                                                     uint8_t cullingSlice);

} // namespace GameEngine::Engine::Renderer
