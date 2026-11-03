#include "Editor/Entities/PlanarReflectionComponentTraits.h"
#include "Editor/Entities/EditorComponentTraits.h"
#include "Components/Rendering/PlanarReflection.h"

#include <utility>

namespace GameEngine::Editor
{
void RegisterPlanarReflectionComponentTraits()
{
    EditorComponentTraits traits;
    traits.DisplayName = "Planar Reflection";
    traits.InspectorIconClass = "inspector-section-icon-reflection-probe";
    traits.Category = "Rendering";
    traits.SortKey = 141;
    EditorComponentTraitsRegistry::Get().Register(
        ECS::GetComponentTypeId<Components::PlanarReflection>(), std::move(traits));
}
}
