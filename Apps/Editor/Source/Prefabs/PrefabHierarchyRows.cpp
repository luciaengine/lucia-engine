#include "Prefabs/PrefabHierarchyRows.h"

#include "Prefabs/PrefabInstanceDescriptions.h"

#include "Editor/Entities/EditorComponentTraits.h"

#include "AssetCore/GUID.h"
#include "AssetCore/PathNormalization.h"
#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Components/Hierarchy.h"
#include "Components/PrefabInstance.h"
#include "Core/Engine.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "Scene/PrefabAuthoring.h"
#include "Types/FlatMap.h"
#include "UI/Controls/Label.h"
#include "UI/Layout/ElementOverrideHelpers.h"
#include "UI/UIElement.h"

#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace GameEngine::Editor
{

namespace
{

constexpr const char* kRootClass = "hierarchy-prefab-root";
constexpr const char* kMemberClass = "hierarchy-prefab-member";
constexpr const char* kMissingClass = "hierarchy-prefab-missing";
constexpr const char* kOverriddenClass = "hierarchy-prefab-overridden";
constexpr const char* kAddedClass = "hierarchy-prefab-added";
constexpr const char* kOverriddenTooltip = "This entity differs from its prefab.";
constexpr const char* kAddedTooltip = "Added on this instance. The prefab does not have this entity.";

// How a described instance shows one entity under its root.
enum class PrefabEntityMark
{
    Unknown,     // The instance has no description: its prefab is not resident or is missing.
    NotInPrefab, // Under an entity added on the instance.
    Member,
    Overridden,
    Added,
};

// The missing prefabs' last paths, read again after a change to the world's structure.
struct MissingPrefabPaths
{
    uint64 WorldId = 0;
    std::size_t StructuralVersion = 0;
    bool Read = false;
    FlatMap<GUID, std::string> Paths; // prefab GUID -> its last path under the asset root
};

MissingPrefabPaths& MissingPathsFor(const ECS::World& world)
{
    static MissingPrefabPaths s_Paths;
    if (s_Paths.WorldId != world.GetWorldId() || s_Paths.StructuralVersion != world.GetStructuralChangeVersion())
    {
        s_Paths.WorldId = world.GetWorldId();
        s_Paths.StructuralVersion = world.GetStructuralChangeVersion();
        s_Paths.Read = false;
    }
    return s_Paths;
}

PrefabEntityMark FindEntityMark(ECS::World& world, ECS::EntityHandle entity)
{
    const DescribedPrefabInstance* instance = PrefabInstanceDescriptions::Get().FindInstance(world, entity);
    if (!instance)
        return PrefabEntityMark::Unknown;
    const Scene::PrefabInstanceEntity* described = instance->Find(entity);
    if (!described)
        return PrefabEntityMark::NotInPrefab;
    if (described->AddedOnInstance)
        return PrefabEntityMark::Added;
    return described->HasOverrides() ? PrefabEntityMark::Overridden : PrefabEntityMark::Member;
}

// The path under the asset root that the registry last had for the missing prefab `source`; empty
// when it has none.
const std::string& FindMissingPrefabPath(MissingPrefabPaths& missingPaths, const AssetRegistry& registry,
                                         const GUID& source)
{
    if (!missingPaths.Read)
    {
        missingPaths.Read = true;
        missingPaths.Paths.Clear();
        const std::filesystem::path assetRoot = registry.GetAssetRoot();
        for (const AssetRegistry::MissingAssetInfo& missing : registry.GetMissingAssets())
        {
            std::string relative;
            if (!AssetPaths::TryMakeCanonicalRelativePath(assetRoot, missing.lastKnownPath, relative))
                relative = missing.lastKnownPath.generic_string();
            missingPaths.Paths.InsertOrAssign(missing.guid, std::move(relative));
        }
    }
    static const std::string s_None;
    const std::string* path = missingPaths.Paths.Find(source);
    return path ? *path : s_None;
}

std::string MissingPrefabTooltip(const std::string& path, const GUID& source)
{
    const std::string file = path.empty() ? source.ToString() : path + " (" + source.ToString() + ")";
    return "Prefab file missing: " + file +
           ". Restore it or delete this entity. While it is missing, the scene cannot be opened again.";
}

// Shows the override dot or the added plus in the row's left gutter, drawn by the row's own
// background (views.css), with its tooltip on the row.
void ApplyMarker(UIElement& row, PrefabEntityMark mark)
{
    const bool hadMarker = row.HasClass(kOverriddenClass) || row.HasClass(kAddedClass);
    row.RemoveClass(kOverriddenClass);
    row.RemoveClass(kAddedClass);
    if (mark == PrefabEntityMark::Overridden)
    {
        row.AddClass(kOverriddenClass);
        row.SetTooltip(kOverriddenTooltip);
    }
    else if (mark == PrefabEntityMark::Added)
    {
        row.AddClass(kAddedClass);
        row.SetTooltip(kAddedTooltip);
    }
    else if (hadMarker)
        row.SetTooltip("");
}

UIElement* FindGlyph(UIElement& row)
{
    for (const auto& child : row.GetChildren())
    {
        if (child->HasClass("hierarchy-preview-icon"))
            return child.get();
    }
    return nullptr;
}

UIElement* FindMemberBadge(UIElement& glyph)
{
    for (const auto& child : glyph.GetChildren())
    {
        if (child->HasClass("hierarchy-prefab-member-badge"))
            return child.get();
    }
    return nullptr;
}

bool TreeShowsBadges(const UIElement& row)
{
    for (const UIElement* ancestor = &row; ancestor; ancestor = ancestor->GetParent())
    {
        if (ancestor->HasClass("tree-icons-show-badges"))
            return true;
    }
    return false;
}

} // namespace

ECS::EntityHandle FindInstanceRoot(ECS::World& world, ECS::EntityHandle entity)
{
    for (ECS::EntityHandle walk = entity; walk.IsValid() && world.IsValid(walk);)
    {
        if (world.HasComponent<Components::PrefabInstance>(walk))
            return walk;
        const auto* parent = world.GetComponent<Components::Parent>(walk);
        walk = parent ? parent->parent : ECS::EntityHandle{};
    }
    return {};
}

std::string PrefabNameOf(ECS::World& world, ECS::EntityHandle root)
{
    const auto* instance = root.IsValid() ? world.GetComponent<Components::PrefabInstance>(root) : nullptr;
    AssetMetadata metadata;
    if (!instance ||
        !EngineCore::GetInstance().GetAssetManager().GetRegistry().TryGetAssetMetadata(instance->Source, metadata))
        return {};
    return std::string(metadata.Name);
}

ECS::EntityHandle FindOutermostInstanceRoot(ECS::World& world, ECS::EntityHandle entity)
{
    ECS::EntityHandle root;
    for (ECS::EntityHandle at = entity; at.IsValid() && world.IsValid(at);)
    {
        if (world.HasComponent<Components::PrefabInstance>(at))
            root = at;
        const auto* parent = world.GetComponent<Components::Parent>(at);
        at = parent ? parent->parent : ECS::EntityHandle{};
    }
    return root;
}

PrefabRowState ClassifyPrefabRow(ECS::World& world, ECS::EntityHandle entity, const AssetRegistry& registry)
{
    PrefabRowState state;
    if (!entity.IsValid() || !world.IsValid(entity))
        return state;

    if (const auto* instance = world.GetComponent<Components::PrefabInstance>(entity))
    {
        AssetMetadata metadata;
        state.Role = PrefabRowRole::InstanceRoot;
        state.SourceMissing = !registry.TryGetAssetMetadata(instance->Source, metadata);
    }
    for (const auto* parent = world.GetComponent<Components::Parent>(entity);
         parent && parent->parent.IsValid() && world.IsValid(parent->parent);
         parent = world.GetComponent<Components::Parent>(parent->parent))
    {
        if (world.HasComponent<Components::PrefabInstance>(parent->parent))
        {
            state.InsideInstance = true;
            break;
        }
    }
    if (state.Role == PrefabRowRole::None && state.InsideInstance)
        state.Role = PrefabRowRole::Member;
    return state;
}

void UpdatePrefabRowPresentation(HierarchyRowParts& parts)
{
    UIElement* glyph = FindGlyph(parts.Row);
    if (!glyph)
        return;
    const bool missing = parts.Row.HasClass(kMissingClass);
    glyph->RemoveClass("hierarchy-prefab-missing-icon");
    if (missing)
        glyph->AddClass("hierarchy-prefab-missing-icon");
    if (missing || parts.Row.HasClass(kRootClass))
        UI::Layout::DisableBackgroundOverride(*glyph);

    const bool showBadge = parts.Row.HasClass(kMemberClass) && TreeShowsBadges(parts.Row);
    UIElement* badge = FindMemberBadge(*glyph);
    if (showBadge)
    {
        glyph->AddClass("hierarchy-prefab-badge-host");
        if (!badge)
        {
            auto memberBadge = std::make_unique<UIElement>();
            memberBadge->AddClass("hierarchy-prefab-member-badge");
            glyph->AddChild(std::move(memberBadge));
        }
    }
    else
    {
        glyph->RemoveClass("hierarchy-prefab-badge-host");
        if (badge)
            glyph->RemoveChild(badge);
    }
}

void DecoratePrefabRow(ECS::World& world, ECS::EntityHandle entity, HierarchyRowParts& parts)
{
    const AssetRegistry& registry = EngineCore::GetInstance().GetAssetManager().GetRegistry();
    const PrefabRowState state = ClassifyPrefabRow(world, entity, registry);
    const PrefabEntityMark mark = state.Role == PrefabRowRole::None
                                      ? PrefabEntityMark::Unknown
                                      : FindEntityMark(world, entity);

    const bool wasMissing = parts.Row.HasClass(kMissingClass);
    parts.Row.RemoveClass(kRootClass);
    parts.Row.RemoveClass(kMemberClass);
    parts.Row.RemoveClass(kMissingClass);
    ApplyMarker(parts.Row, mark);

    // An entity added on an instance, and the entities under it, are not part of the prefab.
    const bool inPrefab = mark != PrefabEntityMark::Added && mark != PrefabEntityMark::NotInPrefab;
    if (state.Role == PrefabRowRole::InstanceRoot)
        parts.Row.AddClass(state.SourceMissing ? kMissingClass : kRootClass);
    // The root of a nested instance has both classes: its glyph is a root's, its badge a member's.
    if (state.InsideInstance && inPrefab)
        parts.Row.AddClass(kMemberClass);

    UpdatePrefabRowPresentation(parts);

    // The tooltip is this decoration's only while the row shows a missing file.
    if (!parts.Title)
        return;
    if (state.SourceMissing)
    {
        const GUID source = world.GetComponent<Components::PrefabInstance>(entity)->Source;
        const std::string& path = FindMissingPrefabPath(MissingPathsFor(world), registry, source);
        parts.Title->SetTooltip(MissingPrefabTooltip(path, source));
    }
    else if (wasMissing)
        parts.Title->SetTooltip("");
}

void SetShownText(Label& label, const std::string& text)
{
    label.SetText(text);
    if (text.empty())
        label.AddClass("hidden");
    else
        label.RemoveClass("hidden");
}

} // namespace GameEngine::Editor
