#include "Engine/Rendering/ClusterLOD/ClusterLODPrepareSystem.h"
#include "Engine/Rendering/ClusterLOD/ClusterLODFeature.h"

namespace GameEngine::Engine::Renderer
{
void ClusterLODPrepareSystem::Update(ECS::World& world, float32 /*deltaTime*/)
{
    if (m_Feature)
        m_Feature->Prepare(world);
}
} // namespace GameEngine::Engine::Renderer
