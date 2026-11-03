#pragma once

#include "Platform/Environment.h"

namespace GameEngine::Engine::Renderer::SkinDiag {

// Returns true when the GE_RENDER_DIAG_SKINNING environment switch is on.
// Used by AnimationSystem, SkinningUploadSystem, and RenderExtractionSystem
// to trace the per-entity skinning pipeline.
inline bool IsEnabled()
{
    static bool s_Enabled = Platform::EnvironmentSwitchEnabled("GE_RENDER_DIAG_SKINNING", false);
    return s_Enabled;
}

} // namespace GameEngine::Engine::Renderer::SkinDiag
