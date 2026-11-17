#include "SceneView/SceneViewEye.h"

#include "Components/SceneViewHidden.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "Editor/Entities/EditorComponentTraits.h"

namespace GameEngine::Editor
{

bool IsHiddenInSceneView(ECS::World& world, ECS::EntityHandle entity)
{
    return world.IsValid(entity) && (world.HasComponent<Components::SceneViewHidden>(entity) ||
                                     world.HasComponent<Components::SceneViewHiddenInHierarchy>(entity));
}

void RegisterSceneViewEyeTraits()
{
    EditorComponentTraits traits;
    traits.HideInInspector = true;
    EditorComponentTraitsRegistry::Get().Register(ECS::GetComponentTypeId<Components::SceneViewHiddenInHierarchy>(),
                                                  std::move(traits));
}

} // namespace GameEngine::Editor
