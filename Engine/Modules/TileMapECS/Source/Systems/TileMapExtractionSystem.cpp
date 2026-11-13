#include "TileMapECS/Systems/TileMapExtractionSystem.h"

#include "Components/TileMap/TileMapLayerRef.h"
#include "Components/Transform.h"
#include "Engine/Rendering/RenderSorting.h"
#include "Engine/Rendering/SortingLayers.h"
#include "ECS/ECS.h"
#include "Engine/Rendering/RenderServices.h"
#include "TileMapECS/TileMapRenderFeature.h"
#include "TileMapECS/TileMapRuntimeStore.h"

#include <algorithm>
#include <vector>

namespace GameEngine
{
namespace TileMapECS
{

TileMapExtractionSystem::TileMapExtractionSystem(Engine::Renderer::RenderServices* renderServices)
    : m_RenderServices(renderServices)
{
    if (m_RenderServices)
        m_RenderServices->EnsureFeature<TileMapRenderFeature>(*m_RenderServices);
}

void TileMapExtractionSystem::Update(ECS::World& world, float32 /*deltaTime*/)
{
    if (!m_RenderServices)
        return;

    auto* store = TileMapRuntimeStore::TryGet();
    if (!store)
        return;
    store->DrainPendingReloads();

    const auto sortingLayers = Rendering::SortingLayers::Current();
    std::optional<bool> hasSortingGroups;
    const std::size_t sortingScanBudget = world.Query<ECS::Read<Components::TileMapLayerRef>>().Count();
    std::vector<TileMapLayerDraw> draws;
    world.Query<ECS::Read<Components::TileMapLayerRef>, ECS::Read<Components::WorldTransform>>()
        .Without<ECS::Disabled>()
        .Each(
            [&](ECS::EntityHandle entity, const Components::TileMapLayerRef& layerRef,
                const Components::WorldTransform& worldXf)
            {
                if (!layerRef.Visible || layerRef.LayerId == 0 || layerRef.TileMap.IsNull() ||
                    layerRef.Opacity <= 0.0f)
                    return;

                TileMapLayerDraw draw{};
                draw.Entity = entity;
                draw.Map = layerRef.TileMap.ToGuid();
                draw.RenderLayers = layerRef.RenderLayers;
                // Keep the world position exact until per-view packing.
                draw.Position = worldXf.GetPosition();
                draw.Basis = worldXf.Basis;
                draw.LayerId = layerRef.LayerId;
                draw.ZOffset = layerRef.ZOffset;
                // Transform the local layer plane before resolving its depth per view.
                draw.WorldZ = worldXf.Position.Z + static_cast<float64>(worldXf.Basis[2].z) * layerRef.ZOffset;
                draw.Sorting = Engine::Renderer::ResolveRenderSortingForBatch(world, entity, sortingLayers, hasSortingGroups, sortingScanBudget);
                draw.Opacity = std::clamp(layerRef.Opacity, 0.0f, 1.0f);
                draw.TintRGBA = layerRef.TintRGBA;
                draw.ParallaxX = layerRef.ParallaxX;
                draw.ParallaxY = layerRef.ParallaxY;
                draws.push_back(draw);
            });

    // Pipeline and GPU state remain lazy until a layer can draw.
    if (draws.empty())
    {
        if (auto* feature = m_RenderServices->GetFeature<TileMapRenderFeature>())
            feature->SetLayerDraws(world.GetWorldId(), {});
        return;
    }

    // Stable producer order breaks ties after the view resolves authored sorting.
    std::stable_sort(draws.begin(), draws.end(),
                     [](const TileMapLayerDraw& a, const TileMapLayerDraw& b)
                     {
                         if (!(a.Map == b.Map))
                             return a.Map < b.Map;
                         return a.LayerId < b.LayerId;
                     });

    auto* feature = m_RenderServices->GetFeature<TileMapRenderFeature>();
    if (feature)
        feature->SetLayerDraws(world.GetWorldId(), std::move(draws));
}

} // namespace TileMapECS
} // namespace GameEngine
