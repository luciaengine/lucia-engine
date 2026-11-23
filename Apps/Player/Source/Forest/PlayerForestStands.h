#pragma once

#include "Types/ScopedSubscription.h"

#include <span>
#include <vector>

namespace GameEngine
{
struct ForestStandConfig;
class SceneLoadInstantiated;

class PlayerForestStands
{
public:
    /// Register the startup scene's stands on the main thread. A failed stand refuses the scene.
    bool Attach(SceneLoadInstantiated& scene, std::span<const ForestStandConfig> configs);

    /// Release the draw registrations before the renderer shuts down.
    void Clear();

private:
    std::vector<ScopedSubscription> m_Registrations;
};
} // namespace GameEngine
