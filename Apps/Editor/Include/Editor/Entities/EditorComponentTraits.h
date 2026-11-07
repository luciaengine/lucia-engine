#pragma once

// EditorComponentTraits — data-level editor behavior for a component type,
// registered by the component's owning editor code or package module. Each
// field is a per-component hook the editor consults without naming the
// component: the inspector section's title, icon, tooltip, order and removal,
// the default inspector's field labels, the Add Component category, the
// hierarchy row class, pick-root and gizmo-pivot behavior, which sections it
// hosts, what switching the component off means, and the undo record for what
// a generic edit makes the component's system overwrite on other entities.
//
// Lives in EditorSDK.dll so Editor.exe and editor-kind package modules share
// one registry instance. Registration is replace-by-typeId (module hot-reload
// re-registers; the old module DLL stays mapped, so replaced callables never
// dangle).

#include "ECS/ECS.h"
#include "ECS/ModuleRegistration.h"
#include "Editor/Entities/DefaultInspectorPresentation.h"

#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace GameEngine
{
class UIElement;
}

namespace GameEngine::ECS
{
class World;
}

namespace GameEngine::Editor
{
class UndoRedoService;

// Runs once a generic edit is applied, inside the edit's undo step (see
// EditorComponentTraits::BeforeGenericEdit). `label` names that step.
using GenericEditUndoRecorder = std::function<void(UndoRedoService& undo, const std::string& label)>;

// Inspector section order for a component that registers no SortKey: after
// every component that does.
inline constexpr int kDefaultInspectorSortKey = 1000;
// Inspector section order that leads the entity's sections, above Transform:
// for a component that is the one thing an author edits on an entity whose
// other sections are generated (a fence's span piece).
inline constexpr int kLeadingInspectorSortKey = -1;

// The parts of a Hierarchy row a decoration may style (see
// EditorComponentTraits::DecorateHierarchyRow). Title is null while the row has none.
struct HierarchyRowParts
{
    UIElement& Row;
    UIElement* Title = nullptr;
};

struct EditorComponentTraits
{
    // Inspector section title (empty = the reflected type name).
    std::string DisplayName;
    // Add Component picker category (empty = "Component").
    std::string Category;
    // Inspector section order: lower sorts first. Built-in bands: entity core
    // below 30, renderers and animation 30-39, physics 40-49, the terrain
    // modifier stack 50-119, camera, lights and probes from 120.
    int SortKey = kDefaultInspectorSortKey;
    // Inspector section header tooltip (empty = none).
    std::string Tooltip;
    // The component renders no inspector section and is not offered in the Add
    // Component picker: bookkeeping state the author never edits directly.
    bool HiddenInInspector = false;
    // The section header offers Remove. False for a component every entity
    // keeps, and for a section that stands for no component of its own.
    bool Removable = true;
    // The section starts collapsed until the author expands it.
    bool CollapsedByDefault = false;
    // What the component's copied values mean, appended in parentheses to the
    // header's Copy and Paste items (empty = nothing to add).
    std::string ClipboardValuesNote;
    bool IsPickClimbRoot = false;
    bool LeadsInspector = false;
    // For a component that owns a stack of other components, the way a volume
    // owns its effects: true for each component type that is an entry of that
    // stack. On an entity with this component, an entry's inspector section
    // renders inside this component's section and drag-reorders among the other
    // entries there; an entry is added only through the host's own picker,
    // never the Add Component picker. Register refuses traits that claim a
    // component type, registered before them, that another host already
    // claims; a type registered after two hosts that claim it is not refused
    // (#2216). The claim runs outside the traits map's lock, but inside Register's
    // lock, so a claim must not register traits. Unset = the section hosts no
    // other sections.
    std::function<bool(ECS::ComponentTypeId)> HostsInspectorSection;
    // CSS class added to the body of each section this one hosts (empty = none).
    std::string HostedSectionBodyClass;
    // The enable dot's tooltip for every entry this component hosts that registers none of
    // its own, in the form of EnableToggleTooltip below, naming what it switches as an effect
    // ("Switch this effect on or off. Off ..."). Empty = the generic tooltip.
    std::string HostedEnableToggleTooltip;
    // An entry on an entity without this component is inactive and stays a
    // top-level section; its header carries this badge, with this warning as the
    // badge's and the header's tooltip.
    std::string MissingHostBadge;
    std::string MissingHostWarning;
    // Removing this component removes the entries of its stack (the components
    // HostsInspectorSection claims) with it, once the author confirms.
    bool RemovingRemovesStackEffects = false;
    // Writes an entry's position in this component's stack (0 = first) into the
    // entry when the author reorders its section. False when the entity does
    // not carry `effectTypeId` or the entry keeps no order; such a section
    // takes no stack position.
    std::function<bool(ECS::World&, ECS::EntityHandle, ECS::ComponentTypeId effectTypeId, int32_t order)>
        SetStackEffectOrder;
    // CSS class for the inspector section icon (empty = the default icon).
    std::string InspectorIconClass;
    // Per-entity icon class for a component whose icon follows its state (a
    // light's type); empty result = InspectorIconClass.
    std::function<std::string(const ECS::World&, ECS::EntityHandle)> InspectorIconClassForEntity;
    // Field labels and presets for the default inspector (empty = the
    // reflected field names and plain rows).
    DefaultInspectorPresentation FieldPresentation;
    // CSS class added to the entity's hierarchy row when the component is
    // present (empty = no contribution).
    std::string HierarchyRowClass;
    // Runs for all bound rows so a feature can mark descendants of its component too.
    std::function<void(ECS::World&, ECS::EntityHandle, HierarchyRowParts&)> DecorateHierarchyRow;
    // The entity is its own pick target: the instance-root upward walk stops
    // here instead of grouping it under a model root.
    bool IsPickInstanceRoot = false;
    // The transform gizmo pivots at the entity origin (the local pivot
    // transformed to world), not at rendered-content anchors.
    bool GizmoPivotAtEntityOrigin = false;
    // What switching the component off does to this type, one sentence in the
    // form "Off stops its sound.", which the section header's enable toggle adds
    // to its tooltip after the generic one (ComponentEnabledToggleTooltip).
    // Empty = the generic sentence alone.
    std::string EnableToggleTooltip;
    // Unset preserves built-in inspector section ordering.
    // For a component whose system writes to other entities (a sky drives its
    // linked light's color): editor code that edits without knowing the
    // component (a component's enable toggle, an entity's creation) calls
    // this before the edit and runs the recorder it returns once the edit is
    // applied, inside the same undo step, so undoing the edit also puts back
    // what the system overwrites because of it. Called before
    // every such edit, whatever entity it touches: what the system writes
    // follows from the whole world, not from the edited entity alone. Unset,
    // or an empty recorder, records nothing.
    std::function<GenericEditUndoRecorder(ECS::World&)> BeforeGenericEdit;
};

class EditorComponentTraitsRegistry
{
public:
    static EditorComponentTraitsRegistry& Get();

    // Replace-semantics: a re-registration for the same type id overwrites the
    // previous traits (module hot-reload replays its registrations). Traits whose
    // HostsInspectorSection claims a registered component type that another
    // component's traits already host are refused with an error naming both
    // hosts and the type.
    void Register(ECS::ComponentTypeId typeId, EditorComponentTraits traits);

    // Copy-out lookup: callers get a stable snapshot, never a pointer into the
    // map (registrations can land at any time from module loads).
    bool TryGet(ECS::ComponentTypeId typeId, EditorComponentTraits& outTraits) const;

    // HiddenInInspector without copying the traits: the inspector polls it for
    // every component of the selected entity.
    bool IsHiddenInInspector(ECS::ComponentTypeId typeId) const;

    // Presence scans. Each returns only the types that set the trait, so a
    // per-row or per-node scan does not copy every registered component.
    std::vector<std::pair<ECS::ComponentTypeId, std::string>> HierarchyRowClasses() const;
    /// Snapshot of row decorations; callers run them outside the registry lock.
    std::vector<std::function<void(ECS::World&, ECS::EntityHandle, HierarchyRowParts&)>> HierarchyRowDecorators() const;
    std::vector<ECS::ComponentTypeId> PickInstanceRootTypes() const;
    std::vector<ECS::ComponentTypeId> GizmoPivotAtEntityOriginTypes() const;
    // Every registered BeforeGenericEdit; the caller runs them outside the
    // registry's lock.
    std::vector<std::function<GenericEditUndoRecorder(ECS::World&)>> BeforeGenericEditHooks() const;

    // The component whose DisplayName is `displayName`, or 0 when none is.
    ECS::ComponentTypeId FindByDisplayName(std::string_view displayName) const;

    // The component whose traits host `hostedTypeId`'s inspector section (see
    // EditorComponentTraits::HostsInspectorSection), with its traits; false when
    // no registered component hosts it. Register keeps the host unique for a
    // component type registered before its hosts; for a type registered after
    // two hosts that claim it, the first match in map order answers (#2216).
    std::vector<std::pair<ECS::ComponentTypeId, EditorComponentTraits>> Snapshot() const;

    bool TryGetSectionHost(ECS::ComponentTypeId hostedTypeId, ECS::ComponentTypeId& outHostTypeId,
                           EditorComponentTraits& outHostTraits) const;

    // C12 editor-kind unload refusal diagnostics: append a description of
    // every entry attributed to `moduleId` (the traits' accessor callables are
    // module code and pin its images mapped).
    void AppendModulePins(std::string_view moduleId, std::vector<std::string>& outPins) const;

private:
    EditorComponentTraitsRegistry() = default;

    mutable std::mutex m_Mutex;
    // Held for a whole Register, so a host's claim check and its insert are one
    // step; the claim predicates run outside m_Mutex.
    std::mutex m_RegistrationMutex;
    std::unordered_map<ECS::ComponentTypeId, EditorComponentTraits> m_Traits;
    // Module stamp per registration (attribution is registry bookkeeping, not
    // part of the module-facing traits struct).
    std::unordered_map<ECS::ComponentTypeId, ECS::ModuleRegistrationStamp> m_ModuleOwners;
};

/// Resolves an inspector icon from registered traits. A null world skips per-entity overrides.
std::string ComponentIconClass(ECS::World* world, ECS::EntityHandle entity, ECS::ComponentTypeId typeId);

} // namespace GameEngine::Editor
