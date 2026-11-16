#pragma once

#include "Types/Types.h"

#include <nlohmann/json_fwd.hpp>

namespace GameEngine
{

class EditorDebugServer;
namespace Engine::Renderer { class ImageBasedLightingFeature; }

// Reflection probe diagnostics without an inspector: capture readiness, pending
// work, completed bakes and the input and baked digests of each retained probe,
// plus a rebake request. Automated checks read them to see when a probe recaptures.
void RegisterReflectionProbeDebugHandlers(EditorDebugServer& server);

// The retained probes of worldId as get_reflection_probe_state reports them.
// ibl may be null: the reply then lists no probes.
nlohmann::json DescribeReflectionProbes(const Engine::Renderer::ImageBasedLightingFeature* ibl, uint64 worldId);

} // namespace GameEngine
