#include "Editor/Entities/DDGIVolumeComponentTraits.h"

#include "Components/Rendering/DDGIVolume.h"
#include "ECS/ECS.h"
#include "Editor/Entities/EditorComponentTraits.h"

#include <utility>

namespace GameEngine::Editor
{
namespace
{

constexpr DefaultInspectorFieldLabel kFieldLabels[] = {
    {"SkyIntensity", "Sky Light"},
    {"ReflectionIntensity", "Local Reflections"},
    {"ShowProbes", "Show Probes"},
    {"ProbesLongAxis", "Divisions"},
    {"RaysPerProbe", "Rays / Probe"},
    {"DebugView", "Debug View"},
};

constexpr DefaultInspectorEnumLabel kEnumLabels[] = {
    {"Cascades", "SingleGrid", "single grid"},
    {"Cascades", "Cascaded2", "cascaded (2)"},
    {"JitterMode", "MonteCarlo", "Monte Carlo"},
    {"DebugView", "Coverage", "probe coverage"},
    {"DebugView", "ProbeState", "probe state"},
    {"DebugView", "CascadeWeight", "cascade weight"},
    {"DebugView", "NearestProbeIrradiance", "nearest probe irradiance"},
    {"DebugView", "ProbeStateFine", "probe state (fine)"},
    {"DebugView", "NearestProbeIrradianceFine", "nearest probe irradiance (fine)"},
    {"DebugView", "FineProbeCoord", "fine probe coord"},
    {"DebugView", "FineProbeSlot", "fine probe slot"},
    {"GlossyResolveScale", "Quarter", "25%"},
    {"GlossyResolveScale", "Half", "50%"},
    {"GlossyResolveScale", "ThreeQuarter", "75%"},
    {"GlossyResolveScale", "Full", "100%"},
};

} // namespace

void RegisterDDGIVolumeComponentTraits()
{
    EditorComponentTraits traits;
    traits.FieldPresentation.FieldLabels = kFieldLabels;
    traits.FieldPresentation.EnumLabels = kEnumLabels;
    // "DDGIVolume" reflects to a run-together section title; the spaced form
    // matches how every other volume component reads in the inspector.
    traits.DisplayName = "DDGI Volume";
    // One registration feeds both surfaces: InspectorPanel's ComponentIconClass
    // consults the registry before its built-in chain, and
    // HierarchyEntityIcon's traits scan adds HierarchyRowClass to the entity's
    // row. Both classes resolve to the same icon in views.css, so the
    // inspector section and the hierarchy row cannot drift apart.
    traits.InspectorIconClass = "inspector-section-icon-ddgi-volume";
    traits.HierarchyRowClass = "hierarchy-entity-ddgi-volume";
    EditorComponentTraitsRegistry::Get().Register(
        ECS::GetComponentTypeId<Components::DDGIVolume>(), std::move(traits));
}

} // namespace GameEngine::Editor
