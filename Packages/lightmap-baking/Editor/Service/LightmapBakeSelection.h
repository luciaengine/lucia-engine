#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace GameEngine::Editor
{
// Inspect scene meshes using the baker's geometry/material eligibility checks.
// Does not copy geometry, decode textures or start a job. UV overlap validation
// remains part of the bake itself. Skipped entries include a mesh name and reason.
std::vector<uint32_t> CollectSceneLightmapReceivers(std::vector<std::string>& skipped);
}
