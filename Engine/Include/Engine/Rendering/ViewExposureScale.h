#pragma once

#include "Types/Types.h"

namespace GameEngine::Rendering
{
struct ViewDesc;
} // namespace GameEngine::Rendering

namespace GameEngine::Engine::Renderer
{

class RenderServices;

/**
 * @brief The linear exposure scale the tonemap will apply to `view`, for a
 * pass that draws display-referred art into the scene-linear HDR buffer and
 * pre-divides by it so the art keeps its authored brightness.
 *
 * Reads the same effective post-process settings the AutoExposure and Tonemap
 * nodes consume. When auto exposure meters the view it turns the view's
 * exposure readback on and returns last frame's adapted scale; until the first
 * readback lands, the settings' base exposure. Clamped to [1e-4, 1e4] so a
 * degenerate metered scale cannot produce inf or NaN colours.
 */
float32 ResolveViewExposureScale(RenderServices& renderServices, const Rendering::ViewDesc& view);

} // namespace GameEngine::Engine::Renderer
