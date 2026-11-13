// Explicit instantiations of the typed World operations for core components. Code that does
// not include ECSTemplates.h links to them by name, inside and outside the Engine library. An
// optimized Mach-O link keeps no reliable implicit copy, so the owning library supplies them.

#include "Components/Hierarchy.h"
#include "Components/Name.h"
#include "Components/PrefabInstance.h"
#include "Components/Rendering/LensFlareSource.h"
#include "Components/Rendering/Light.h"
#include "Components/Rendering/LocalBounds.h"
#include "Components/Rendering/MeshGPUData.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Components/RuntimeOnlyEntity.h"
#include "Components/SceneEntityTag.h"
#include "Components/Spline/SplinePaving.h"
#include "Components/Transform.h"
#include "Components/UI/UIDocument.h"

#include "ECS/ECSTemplates.h"

namespace GameEngine::ECS
{
GE_INSTANTIATE_ENGINE_COMPONENT(Components::Transform);
GE_INSTANTIATE_ENGINE_COMPONENT(Components::WorldTransform);
GE_INSTANTIATE_ENGINE_COMPONENT(Components::Parent);
GE_INSTANTIATE_ENGINE_COMPONENT(Components::Light);
GE_INSTANTIATE_ENGINE_COMPONENT(Components::RuntimeOnlyEntity);
GE_INSTANTIATE_ENGINE_COMPONENT(Components::LensFlareSource);
GE_INSTANTIATE_ENGINE_COMPONENT(Components::UIDocument);
// Reached from editor code that EditorTests compiles without the editor's own
// EditorComponentInstantiations.cpp, so the Editor linked and the test binary did not;
// Engine owns these, so Engine exports them. MeshGPUData is the same shape: its only
// definitions in that link came from two test TUs that happen to touch it.
GE_INSTANTIATE_ENGINE_COMPONENT(Components::Name);
GE_INSTANTIATE_ENGINE_COMPONENT(Components::SceneEntityTag);
GE_INSTANTIATE_ENGINE_COMPONENT(Components::LocalBounds);
GE_INSTANTIATE_ENGINE_COMPONENT(Components::MeshGPUData);
// Engine code that does not include ECSTemplates.h reads these (SplineRecipes, Scene).
GE_INSTANTIATE_ENGINE_COMPONENT(Components::MeshRenderer);
GE_INSTANTIATE_ENGINE_COMPONENT(Components::SplinePaving);
GE_INSTANTIATE_ENGINE_COMPONENT(Components::PrefabInstance);
} // namespace GameEngine::ECS
