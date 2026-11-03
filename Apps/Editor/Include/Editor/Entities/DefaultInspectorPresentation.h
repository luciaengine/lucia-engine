#pragma once

// How the reflection-driven default inspector presents one component's fields:
// row labels, enumerator labels and preset dropdowns. A component registers it
// in its EditorComponentTraits, so a label or preset applies to that component
// only and never to another component's field of the same name. Tooltips are
// not here: they live on the field's reflection (// @ge-tooltip).
//
// Every view points at static storage in the registering file; the registry
// copies the spans, never the tables.

#include <cstdint>
#include <span>
#include <string_view>

namespace GameEngine::Editor
{

struct DefaultInspectorFieldLabel
{
    std::string_view Field;
    std::string_view Label;
};

struct DefaultInspectorEnumLabel
{
    std::string_view Field;
    std::string_view Enumerator;
    std::string_view Label;
};

struct DefaultInspectorUIntPreset
{
    std::uint32_t Value = 0;
    std::string_view Label;
};

struct DefaultInspectorFloatPreset
{
    float Value = 0.0f;
    std::string_view Label;
};

struct DefaultInspectorUIntPresets
{
    std::string_view Field;
    std::span<const DefaultInspectorUIntPreset> Presets;
};

struct DefaultInspectorFloatPresets
{
    std::string_view Field;
    std::span<const DefaultInspectorFloatPreset> Presets;
};

struct DefaultInspectorPresentation
{
    std::span<const DefaultInspectorFieldLabel> FieldLabels;
    std::span<const DefaultInspectorEnumLabel> EnumLabels;
    std::span<const DefaultInspectorUIntPresets> UIntPresets;
    std::span<const DefaultInspectorFloatPresets> FloatPresets;
};

} // namespace GameEngine::Editor
