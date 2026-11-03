#include "ECSModules/Rendering/Systems/PlanarReflectionSystem.h"
#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "Engine/Rendering/PlanarReflectionFeature.h"
#include "Engine/Rendering/RenderServices.h"

namespace GameEngine::Engine::Renderer
{
std::vector<PlanarReflectionPlane> PlanarReflectionSystem::ResolvePlanes(ECS::World& world)
{
    std::vector<PlanarReflectionPlane> planes;
    world.Query<ECS::Read<Components::PlanarReflection>, ECS::Read<Components::WorldTransform>>()
        .Without<ECS::Disabled>().Each([&](ECS::EntityHandle entity,
            const Components::PlanarReflection& component, const Components::WorldTransform& transform)
        {
            PlanarReflectionPlane plane;
            if (ResolvePlanarReflectionPlane(transform.matrix, entity.id, component, plane))
                planes.push_back(plane);
        });
    return planes;
}
void PlanarReflectionSystem::Update(ECS::World& world, float32)
{
    if (!m_Services) return;
    auto planes = ResolvePlanes(world);
    auto* feature = m_Services->GetFeature<PlanarReflectionFeature>();
    if (!feature && planes.empty()) return;
    if (!feature) feature = &m_Services->EnsureFeature<PlanarReflectionFeature>();
    feature->SyncWorld(*m_Services, world.GetWorldId(), std::move(planes));
}
}
