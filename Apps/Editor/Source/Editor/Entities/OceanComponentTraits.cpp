#include "Editor/Entities/OceanComponentTraits.h"

#include "Components/Rendering/Ocean.h"
#include "ECS/ECS.h"
#include "Editor/Entities/EditorComponentTraits.h"

#include <utility>

namespace GameEngine::Editor
{
namespace
{

constexpr DefaultInspectorFieldLabel kSurfaceFieldLabels[] = {
    {"PlanarReflections", "Planar Reflections"},
    {"PlanarReflectionStrength", "Planar Strength"},
    {"PlanarReflectionScale", "Planar Scale"},
    {"FoamDebugMode", "Foam Debug"},
    {"ShallowRefractionReflectionSuppression", "Shallow Reflection Suppression"},
    {"DirectionalLightColor", "Glitter Color"},
};

constexpr DefaultInspectorUIntPreset kFoamDebugModePresets[] = {
    {0u, "Normal"},
    {1u, "Combined Foam"},
    {2u, "Wave Foam"},
    {3u, "Shoreline / Contact"},
    {4u, "Freshness / Age"},
    {5u, "Latest Deposit"},
    {6u, "Seabed Depth Ramp"},
    {7u, "Shallow Mask"},
    {8u, "Invalid / Deep Tiles"},
};

constexpr DefaultInspectorUIntPresets kSurfaceUIntPresets[] = {
    {"FoamDebugMode", kFoamDebugModePresets},
};

constexpr DefaultInspectorFloatPreset kPlanarReflectionScalePresets[] = {
    {1.0f, "1x"},
    {0.75f, "0.75x"},
    {0.5f, "0.5x"},
    {0.25f, "0.25x"},
    {0.125f, "0.125x"},
};

constexpr DefaultInspectorFloatPresets kSurfaceFloatPresets[] = {
    {"PlanarReflectionScale", kPlanarReflectionScalePresets},
};

constexpr DefaultInspectorFieldLabel kRendererFieldLabels[] = {
    {"CombineDisplacementCascade", "Combine Cascade"},
    {"LodDataResolution", "LOD Data Resolution"},
    {"RasterDepthCaptureResolution", "Raster Depth Resolution"},
    {"MinScale", "Min Scale"},
    {"MaxScale", "Max Scale"},
};

constexpr DefaultInspectorUIntPreset kCascadeResolutionPresets[] = {
    {0u, "Default"},
    {128u, "128"},
    {256u, "256"},
    {384u, "384"},
    {512u, "512"},
    {1024u, "1024"},
};

constexpr DefaultInspectorUIntPreset kRasterDepthResolutionPresets[] = {
    {256u, "256"},
    {512u, "512"},
    {1024u, "1024"},
    {2048u, "2048"},
    {4096u, "4096"},
};

constexpr DefaultInspectorUIntPresets kRendererUIntPresets[] = {
    {"LodDataResolution", kCascadeResolutionPresets},
    {"RasterDepthCaptureResolution", kRasterDepthResolutionPresets},
};

constexpr DefaultInspectorFloatPreset kMinScalePresets[] = {
    {8.0f, "8"},
    {16.0f, "16"},
    {32.0f, "32"},
    {64.0f, "64"},
    {128.0f, "128"},
    {256.0f, "256"},
};

constexpr DefaultInspectorFloatPreset kMaxScalePresets[] = {
    {64.0f, "64"},
    {128.0f, "128"},
    {256.0f, "256"},
    {512.0f, "512"},
    {1024.0f, "1024"},
    {2048.0f, "2048"},
    {4096.0f, "4096"},
};

constexpr DefaultInspectorFloatPresets kRendererFloatPresets[] = {
    {"MinScale", kMinScalePresets},
    {"MaxScale", kMaxScalePresets},
};

} // namespace

void RegisterOceanComponentTraits()
{
    {
        EditorComponentTraits traits;
        traits.InspectorIconClass = "inspector-section-icon-ocean";
        traits.FieldPresentation.FieldLabels = kSurfaceFieldLabels;
        traits.FieldPresentation.UIntPresets = kSurfaceUIntPresets;
        traits.FieldPresentation.FloatPresets = kSurfaceFloatPresets;
        EditorComponentTraitsRegistry::Get().Register(
            ECS::GetComponentTypeId<Components::OceanSurface>(), std::move(traits));
    }

    {
        EditorComponentTraits traits;
        traits.FieldPresentation.FieldLabels = kRendererFieldLabels;
        traits.FieldPresentation.UIntPresets = kRendererUIntPresets;
        traits.FieldPresentation.FloatPresets = kRendererFloatPresets;
        EditorComponentTraitsRegistry::Get().Register(
            ECS::GetComponentTypeId<Components::OceanRenderer>(), std::move(traits));
    }

    {
        EditorComponentTraits traits;
        traits.InspectorIconClass = "inspector-section-icon-ocean";
        EditorComponentTraitsRegistry::Get().Register(
            ECS::GetComponentTypeId<Components::OceanWaveSpectrum>(), std::move(traits));
    }

    {
        EditorComponentTraits traits;
        traits.InspectorIconClass = "inspector-section-icon-ocean";
        EditorComponentTraitsRegistry::Get().Register(
            ECS::GetComponentTypeId<Components::OceanBuoyancy>(), std::move(traits));
    }
}

} // namespace GameEngine::Editor
