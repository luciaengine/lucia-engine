#include "SplineRecipes/RecipeControllers.h"

#include "Core/CpuProfiler.h"
#include "Paving/PavingApproachGround.h"
#include "SplineRecipes/Paving/SplinePavingController.h"
#include "SplineRecipes/SplineExtrudeController.h"
#include "SplineRecipes/SplineFenceController.h"
#include "SplineRecipes/SplineGroundAuthority.h"
#include "SplineRecipes/SplinePlacementController.h"
#include "SplineRecipes/SplineWall/SplineWallController.h"

namespace GameEngine::SplineRecipes
{

RecipeControllers::RecipeControllers()
    : m_Placement(std::make_unique<SplinePlacementController>()),
      m_Fence(std::make_unique<SplineFenceController>()),
      m_Paving(std::make_unique<SplinePavingController>()),
      m_Extrude(std::make_unique<SplineExtrudeController>()),
      m_Wall(std::make_unique<SplineWallController>()),
      m_GroundAuthority(std::make_unique<SplineGroundAuthorityController>()),
      m_ApproachGround(std::make_unique<PavingApproachGround>())
{
}

RecipeControllers::~RecipeControllers() = default;

void RecipeControllers::UpdateGenerators(ECS::World& world, Engine::Renderer::RenderServices& renderServices,
                                         float32 deltaSeconds)
{
    {
        GE_CPU_PROFILE_SCOPE("RecipeControllers.SplinePlacement");
        m_Placement->Update(world, renderServices, deltaSeconds, m_BuildStates);
    }
    {
        GE_CPU_PROFILE_SCOPE("RecipeControllers.SplineFence");
        m_Fence->Update(world, renderServices, deltaSeconds, m_BuildStates);
    }
    {
        GE_CPU_PROFILE_SCOPE("RecipeControllers.SplinePaving");
        m_Paving->Update(world, renderServices, deltaSeconds, m_BuildStates);
    }
    {
        GE_CPU_PROFILE_SCOPE("RecipeControllers.SplineExtrude");
        m_Extrude->Update(world, renderServices, deltaSeconds, m_BuildStates, m_Paving->RoadTrims(),
                          m_Paving->HeldRoads());
    }
    {
        GE_CPU_PROFILE_SCOPE("RecipeControllers.SplineWall");
        m_Wall->Update(world, renderServices, deltaSeconds, m_BuildStates);
    }
}

void RecipeControllers::Update(ECS::World& world, Engine::Renderer::RenderServices* renderServices,
                               RecipeEditJournal* journal, float32 deltaSeconds)
{
    m_BuildStates.BeginUpdate(world);
    if (renderServices)
        UpdateGenerators(world, *renderServices, deltaSeconds);
    else
        m_BuildStates.FailEveryRecipe(world, kNoRenderDeviceReason);
    {
        GE_CPU_PROFILE_SCOPE("RecipeControllers.SplineGroundAuthority");
        m_GroundAuthority->Update(world, journal);
        m_ApproachGround->Update(world, m_Paving->Approaches(), m_Paving->ApproachesVersion());
    }
    m_BuildStates.EndUpdate(world);
}

void RecipeControllers::ReleaseSceneScoped(ECS::World& world,
                                           Engine::Renderer::RenderServices* renderServices)
{
    m_Placement->Shutdown(world);
    m_Fence->Shutdown(world, renderServices);
    m_Paving->Shutdown(world, renderServices);
    m_Extrude->Shutdown(world, renderServices);
    m_Wall->Shutdown(world, renderServices);
    m_GroundAuthority->Shutdown();
    m_ApproachGround->Shutdown(world);
}

std::span<const uint32> RecipeControllers::FenceRunSpanCounts(ECS::EntityHandle entity) const
{
    return m_Fence->RunSpanCounts(entity);
}

std::span<const FenceMeshBounds> RecipeControllers::FencePartBounds(ECS::EntityHandle entity) const
{
    return m_Fence->PartBounds(entity);
}

const PavingStatus* RecipeControllers::PavingStatusOf(ECS::EntityHandle entity) const
{
    return m_Paving->StatusOf(entity);
}

void RecipeControllers::PrefillVergeMaterial(const ECS::World& world, ECS::EntityHandle road,
                                             Components::SplineExtrude& recipe) const
{
    m_Paving->PrefillVergeMaterial(world, road, recipe);
}

} // namespace GameEngine::SplineRecipes
