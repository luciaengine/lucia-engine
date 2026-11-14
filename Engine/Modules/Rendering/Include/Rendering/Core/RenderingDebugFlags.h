#pragma once

#include "Platform/Environment.h"

// Centralized renderer debug env-var checks, evaluated once via Meyers singleton.
// Lightweight header — depends only on Platform's header-only environment switch.
namespace GameEngine::Rendering
{

struct RenderingDebugFlags
{
    bool traceRendering = false;
    bool srgbDiag = false;

    static inline const RenderingDebugFlags& Get()
    {
        static const RenderingDebugFlags instance = [] {
            RenderingDebugFlags f;
            f.traceRendering = Platform::EnvironmentSwitchEnabled("GE_DEBUG_TRACE_RENDERING", false);
            f.srgbDiag = Platform::EnvironmentSwitchEnabled("GE_DEBUG_SRGB_ENCODE", false);
            return f;
        }();
        return instance;
    }
};

} // namespace GameEngine::Rendering
