#include "SceneView/SplineTool/SplineOwnerQuery.h"

#include "Components/RuntimeOnlyEntity.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"

#include <utility>

namespace GameEngine::Editor
{

namespace
{
SplineOwnerQuery& OwnerQuery()
{
    static SplineOwnerQuery query;
    return query;
}
} // namespace

void SetSplineOwnerQuery(SplineOwnerQuery query)
{
    OwnerQuery() = std::move(query);
}

SplineOwnerClaim QuerySplineOwner(const ECS::World& world, ECS::EntityHandle entity)
{
    // A generated spline (a paving's approach piece) is its generator's: no spline tool shows it.
    if (world.GetComponent<Components::RuntimeOnlyEntity>(entity) != nullptr)
        return {.Claimed = true, .Hidden = true, .ClosedRowTooltip = "A generated spline follows the recipe that made it"};
    const SplineOwnerQuery& query = OwnerQuery();
    return query ? query(world, entity) : SplineOwnerClaim{};
}

} // namespace GameEngine::Editor
