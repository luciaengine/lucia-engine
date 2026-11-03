#include "Sky/SkyEnvironmentComponentTraits.h"

#include "Components/Rendering/SkyEnvironment.h"
#include "ECS/ECS.h"
#include "Editor/Entities/EditorComponentTraits.h"
#include "Sky/SkySunLinkUndo.h"

#include <utility>

namespace GameEngine::Editor
{

void RegisterSkyEnvironmentComponentTraits()
{
    EditorComponentTraits traits;
    traits.DisplayName = "Sky Environment";
    traits.Category = "Rendering";
    traits.Tooltip = "Procedural sky with atmospheric scattering and stars";
    traits.InspectorIconClass = "inspector-section-icon-sky";
    // The sky drives its sun light's color, so an edit that starts the drive records that color.
    traits.BeforeGenericEdit = SkySunLinkUndo::BeforeGenericEdit;
    EditorComponentTraitsRegistry::Get().Register(ECS::GetComponentTypeId<Components::SkyEnvironment>(),
                                                  std::move(traits));
}

} // namespace GameEngine::Editor
