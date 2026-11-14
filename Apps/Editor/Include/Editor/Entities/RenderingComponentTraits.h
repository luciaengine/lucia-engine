#pragma once

namespace GameEngine::Editor
{

// Registers the editor traits of the rendering components that have no
// inspector of their own: GPU bookkeeping, bounds, layers, LOD groups,
// reflection probes and video textures.
void RegisterRenderingComponentTraits();

} // namespace GameEngine::Editor
