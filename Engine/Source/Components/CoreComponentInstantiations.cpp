// Engine owns typed World operations for core components consumed across DLLs.
// Explicit exports remain available after optimized builds inline every local caller.

#include "Components/Hierarchy.h"
#include "Components/Name.h"
#include "Components/PrefabInstance.h"
#include "Components/Rendering/Light.h"
#include "Components/Rendering/LocalBounds.h"
#include "Components/Rendering/MeshGPUData.h"
#include "Components/RuntimeOnlyEntity.h"
#include "Components/SceneEntityTag.h"
#include "Components/Transform.h"

#include "ECS/ECSTemplates.h"

namespace GameEngine::ECS
{
GE_INSTANTIATE_ENGINE_COMPONENT(Components::Transform);
GE_INSTANTIATE_ENGINE_COMPONENT(Components::WorldTransform);
GE_INSTANTIATE_ENGINE_COMPONENT(Components::Parent);
GE_INSTANTIATE_ENGINE_COMPONENT(Components::Light);
GE_INSTANTIATE_ENGINE_COMPONENT(Components::RuntimeOnlyEntity);
// Editor and test consumers reach these Engine-owned components through the typed World API.
GE_INSTANTIATE_ENGINE_COMPONENT(Components::Name);
GE_INSTANTIATE_ENGINE_COMPONENT(Components::SceneEntityTag);
GE_INSTANTIATE_ENGINE_COMPONENT(Components::PrefabInstance);
GE_INSTANTIATE_ENGINE_COMPONENT(Components::LocalBounds);
GE_INSTANTIATE_ENGINE_COMPONENT(Components::MeshGPUData);
} // namespace GameEngine::ECS
