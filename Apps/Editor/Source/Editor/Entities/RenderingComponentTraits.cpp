#include "Editor/Entities/RenderingComponentTraits.h"

#include "Components/Rendering/LODGroup.h"
#include "Components/Rendering/LocalBounds.h"
#include "Components/Rendering/MeshGPUData.h"
#include "Components/Rendering/ReflectionProbe.h"
#include "Components/Rendering/RenderLayer.h"
#include "Components/Video/VideoTextureComponent.h"
#include "ECS/ECS.h"
#include "Editor/Entities/EditorComponentTraits.h"

#include <utility>

namespace GameEngine::Editor
{
namespace
{

constexpr DefaultInspectorUIntPreset kProbeCaptureResolutionPresets[] = {
    {64u, "64"},
    {128u, "128"},
    {256u, "256"},
    {512u, "512"},
    {1024u, "1024"},
};

constexpr DefaultInspectorUIntPresets kProbeUIntPresets[] = {
    {"CaptureResolution", kProbeCaptureResolutionPresets},
};

} // namespace

void RegisterRenderingComponentTraits()
{
    constexpr int kReflectionProbeSortKey = 140;

    // GPU buffer handles the renderer owns.
    {
        EditorComponentTraits traits;
        traits.HiddenInInspector = true;
        EditorComponentTraitsRegistry::Get().Register(
            ECS::GetComponentTypeId<Components::MeshGPUData>(), std::move(traits));
    }

    // Derived from the mesh geometry.
    {
        EditorComponentTraits traits;
        traits.DisplayName = "World Bounds";
        traits.HiddenInInspector = true;
        EditorComponentTraitsRegistry::Get().Register(
            ECS::GetComponentTypeId<Components::LocalBounds>(), std::move(traits));
    }

    {
        EditorComponentTraits traits;
        traits.DisplayName = "Render Layer";
        traits.Category = "Rendering";
        traits.InspectorIconClass = "inspector-section-icon-render-layer";
        EditorComponentTraitsRegistry::Get().Register(
            ECS::GetComponentTypeId<Components::RenderLayer>(), std::move(traits));
    }

    {
        EditorComponentTraits traits;
        traits.DisplayName = "LOD Group";
        traits.Category = "Rendering";
        traits.InspectorIconClass = "inspector-section-icon-lod-group";
        EditorComponentTraitsRegistry::Get().Register(
            ECS::GetComponentTypeId<Components::LODGroup>(), std::move(traits));
    }

    {
        EditorComponentTraits traits;
        traits.DisplayName = "Reflection Probe";
        traits.Category = "Rendering";
        traits.SortKey = kReflectionProbeSortKey;
        traits.InspectorIconClass = "inspector-section-icon-reflection-probe";
        traits.FieldPresentation.UIntPresets = kProbeUIntPresets;
        EditorComponentTraitsRegistry::Get().Register(
            ECS::GetComponentTypeId<Components::ReflectionProbe>(), std::move(traits));
    }

    {
        EditorComponentTraits traits;
        traits.DisplayName = "Video Texture";
        traits.Category = "Rendering";
        traits.InspectorIconClass = "inspector-section-icon-video-texture";
        EditorComponentTraitsRegistry::Get().Register(
            ECS::GetComponentTypeId<Components::VideoTextureComponent>(), std::move(traits));
    }
}

} // namespace GameEngine::Editor
