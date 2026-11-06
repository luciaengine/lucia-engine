#pragma once

#include "AssetCore/AssetRegistry.h" // AssetReference
#include "AssetCore/GUID.h"
#include "Components/SceneEntityTag.h"
#include "ECS/Entity.h"
#include "Scene/SceneAssetResolver.h"
#include "Scene/SceneEmbedMaterializer.h"
#include "Scene/SceneIOContext.h"
#include "Scene/SceneValue.h"

#include <cstdint>
#include <chrono>
#include <span>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace GameEngine::Scene
{
struct SceneIOError
{
    std::filesystem::path file;
    int line = 0; // 0 when unknown/not applicable
    std::string message;

    // A Replace load reached World::Clear before failing: the world now holds a
    // partially instantiated incoming scene, and whatever the caller had open is
    // gone. False for failures raised before the clear (read, parse, include
    // expansion, validation) and for additive loads, which never empty the world.
    bool worldCleared = false;
};

// Last SceneIO error for the calling thread (cleared on success, overwritten on failure).
const SceneIOError& GetLastSceneIOError();
void ClearLastSceneIOError();

// One component or field assignment the loader could not apply. The load CONTINUED past it, so the
// entity exists and everything else about it is live — this records exactly what did not make it in.
// A scene authored against a newer build than the one opening it (a component type, a field or an
// enumerator that this binary's reflection tables do not know), or one carrying a value whose syntax
// this parser cannot read at all, produces one of these per assignment instead of costing the user
// the whole scene. Structural errors are not in this class and still fail the load: a dangling
// entity or resource reference is a claim about the document, not a value a component can decline.
struct SceneLoadSkip
{
    enum class SkipKind { Unapplied, OrphanedOverride, UnderRemovedEntity };
    std::string entityId;       // instantiated scene entity id (include/blueprint prefix applied)
    std::string component;      // authored component name
    std::string field;          // authored property key; empty when the whole component failed
    std::string message;        // the error exactly as the schema reported it
    std::filesystem::path file; // file the line came from (an include, for included entities)
    int line = 0;               // 0 when unknown

    // The authored text was kept in the world's preserved-field side table AT LOAD TIME, so a save
    // re-emits it verbatim rather than writing the default the field fell back to. False means the
    // assignment exists nowhere but this record, and saving WILL drop it.
    //
    // A load-time answer, and the override can retire afterwards — see SkipIsOutstanding, which is
    // what any surface reporting on a save's behaviour must ask instead.
    bool preserved = false;

    // The entity the override was recorded on, so SkipIsOutstanding can find it in the store
    // without resolving `entityId` back through the scene tags. Meaningless when !preserved.
    ECS::EntityHandle entityHandle{};
    ECS::ComponentTypeId componentType = 0;
    SkipKind kind = SkipKind::Unapplied;
    std::string address;
};

// Everything one load could not apply. Empty => the scene loaded cleanly.
struct SceneLoadDegradation
{
    std::vector<SceneLoadSkip> skips;

    bool IsDegraded() const { return !skips.empty(); }

    // Skips whose text was NOT preserved — i.e. what saving this scene would actually lose. The
    // distinction is the whole point: a fully-preserved degraded scene round-trips without loss,
    // and telling the user otherwise would be crying wolf.
    //
    // Invariant under the outstanding filter below, which only ever retires PRESERVED rows — so a
    // discard can never turn a round-trip into a reported loss.
    std::size_t DroppedCount() const
    {
        std::size_t n = 0;
        for (const SceneLoadSkip& s : skips)
            n += s.preserved ? 0u : 1u;
        return n;
    }
};

// Whether this skip still describes something a save has to answer for.
//
// A never-preserved skip always does: nothing can resolve it, and saving still drops it. A
// preserved one does only while the world still holds its override — and three things retire that
// after the load, none of which the census can see: the inspector's Discard, removing the
// component, and destroying the entity on the handle-releasing path. Reporting a retired row as
// preserved tells the user a save will write back text that is no longer there.
bool SkipIsOutstanding(const ECS::World& world, const SceneLoadSkip& skip);

// How many of a census's skips are still outstanding against `world`. See SkipIsOutstanding.
std::size_t OutstandingSkipCount(const ECS::World& world, const SceneLoadDegradation& census);

enum class LoadMode
{
    Replace,
    Additive
};

// Optional editor snapshot stored under [hierarchy_ui] in .scene files (foldout expansion + selection by SceneEntityTag).
struct SceneHierarchyUi
{
    std::vector<std::string> expandedSceneEntityTags;
    std::vector<std::string> selectionSceneEntityTagsOrdered;
};

// Result of loading a scene file regarding [hierarchy_ui]: whether the section existed, and its payload when it did.
struct SceneHierarchyUiFromFile
{
    bool hadSection = false;
    SceneHierarchyUi ui{};
};

// Editor scene-view camera pose persisted under [editor_camera] in .scene files.
struct SceneEditorCamera
{
    float PosX = 0.0f;
    float PosY = 0.0f;
    float PosZ = 0.0f;
    float YawDeg = 0.0f;
    float PitchDeg = 0.0f;
    float Distance = 0.0f;
    bool Is2D = false;
};

struct SceneEditorCameraFromFile
{
    bool hadSection = false;
    SceneEditorCamera camera{};
};

// Main-thread phase split of one LoadSceneFromFile call, in milliseconds. The
// single "parse+instantiate" number a caller can measure from outside hides
// which phase dominates, and a delta-based reload can only ever skip the
// instantiate half — so the split is what makes that trade-off falsifiable.
struct SceneLoadTimings
{
    double ReadMs = 0.0;
    double ParseMs = 0.0;     // tokenise + include expansion
    double ValidateMs = 0.0;
    double ClearMs = 0.0;     // World::Clear, Replace mode only
    double InstantiateMs = 0.0;
};

struct LoadOptions
{
    LoadMode mode = LoadMode::Replace;

    // Optional resolver for GUID/path/asset-root integration.
    // If null, SceneIO falls back to best-effort lexical GUID derivation and extension-based typing.
    ISceneAssetResolver* assetResolver = nullptr;

    // Optional override for the project's asset root. If empty and assetResolver is provided,
    // SceneIO uses assetResolver->GetAssetRoot(). If both are empty, SceneIO falls back to paths
    // relative to the scene file's directory.
    std::filesystem::path assetRootOverride;

    // The project directory. A file the loader names in an error message is spelled relative to it
    // ("Assets/props/crate.blueprint"), the way the user finds it; empty, or a file outside it, the
    // message spells the path absolute.
    std::filesystem::path projectRoot;

    // Optional embed executor/materializer. If provided, embed references (#id where id is from [embed])
    // can be materialized into real loadable assets (e.g., embedded materials).
    ISceneEmbedMaterializer* embedMaterializer = nullptr;

    // When non-null, filled after a successful load when the file contained [hierarchy_ui].
    SceneHierarchyUiFromFile* outHierarchyUiFromFile = nullptr;

    // When non-null, filled after a successful load when the file contained [editor_camera].
    SceneEditorCameraFromFile* outEditorCameraFromFile = nullptr;

    // When non-null, filled with the main-thread phase split. Phases that did
    // not run (an early failure, or Clear on an additive load) stay 0.
    SceneLoadTimings* outTimings = nullptr;

    // When non-null, filled with every assignment the load could not apply. A non-empty census on a
    // load that RETURNED TRUE means the scene is degraded: it opened, but it is not all of what the
    // file says. The loader always logs the census; this is how a caller gets the full list rather
    // than the first few, and how the editor marks the document.
    SceneLoadDegradation* outDegradation = nullptr;
};

struct SaveOptions
{
    bool ensureSceneEntityTags = true; // assign SceneEntityTag to entities that lack one

    // Preserve existing [blueprint] instance sections when saving.
    bool preserveBlueprintInstances = true;

    // If true, the file is a prefab file: it starts with the [prefab version=1] header. The world
    // must hold one root entity, with the id "root", and every other id must be a valid prefab id.
    bool writePrefabHeader = false;

    // If true, each prefab instance is saved as a [prefab] section (with its [resource] line) that holds
    // what the instance changes in its prefab, rather than flattened into [entity] blocks. The save
    // reads each prefab's path from assetResolver and refuses an instance it cannot name.
    bool preservePrefabInstances = true;

    // If true, entities that were loaded from [subscene] sections and tagged in-world will be re-saved
    // back to [subscene] sections (with [resource] lines), rather than being flattened into [entity] blocks.
    bool preserveSubscenes = true;

    // Optional resolver for GUID/path lookups during save. If provided, SaveSceneToFile will try
    // to fill missing resource guid= fields (best-effort) using the resolver.
    ISceneAssetResolver* assetResolver = nullptr;

    // Optional asset root override (used when resolving relative resource paths).
    std::filesystem::path assetRootOverride;

    // When non-null, appended as a trailing [hierarchy_ui] section (editor foldout/selection by SceneEntityTag).
    const SceneHierarchyUi* hierarchyUi = nullptr;

    // When non-null, appended as a trailing [editor_camera] section with the scene-view camera pose.
    const SceneEditorCamera* editorCamera = nullptr;
};

// Immutable, owned parse/validation result. Preparing does not access an ECS
// world, register schemas, resolve assets or submit GPU work. It can run on a
// worker if the configured file reader supports it and is not being changed.
// Failure returns null; GetLastSceneIOError belongs to the preparing thread.
// Includes are captured now. Referenced blueprint/subscene files retain the
// existing late-load semantics during instantiation; this is not a frozen
// dependency package or a promise of nonblocking instantiation.
struct PreparedScene;
std::shared_ptr<const PreparedScene> PrepareSceneFromFile(
    const std::filesystem::path& sceneFilePath, SceneLoadTimings* outTimings = nullptr);

struct PreparedSceneEntityInfo
{
    std::string Id;
    std::string Name;
    std::optional<std::string> Parent;
    std::vector<std::string> Components;
    std::vector<std::string> DependencyIds;
};

// Canonical-parser projection for derived runtime scene packages. No world,
// schema registration, asset resolution or source writes. Flat scenes only;
// blueprint/subscene/embed documents and unparseable property values fail closed.
// DependencyIds includes parents, explicit $ references and every quoted/bare
// string matching a document entity id (including nested values). The latter is
// deliberately conservative: existing schemas also encode entity links as plain
// quoted strings. An ordinary string matching an id may over-include an entity.
// Runtime lookups by name, script-owned links and other semantic dependencies
// are NOT inferred: the project must explicitly include those roots.
std::optional<std::vector<PreparedSceneEntityInfo>> DescribePreparedScene(
    const PreparedScene& prepared);

// Select roots AND their transitive DependencyIds, preserving document order,
// authored ids/properties and the full resource table. Unknown roots fail; an
// empty root list produces an empty scene. No duplication within one result.
// Separately selected groups may overlap: do not instantiate them together
// without an explicit shared-entity ownership/binding policy.
std::shared_ptr<const PreparedScene> SelectPreparedSceneEntities(
    const PreparedScene& prepared, std::span<const std::string> rootIds);

// Batch equivalent, building the full document's reference catalog only once.
// Results preserve input-group order; each is independently closed and may
// overlap another. Any invalid group rejects the entire batch without mutation.
std::optional<std::vector<std::shared_ptr<const PreparedScene>>> SelectPreparedSceneGroups(
    const PreparedScene& prepared, std::span<const std::vector<std::string>> rootGroups);

// Emit a flat runtime document without instantiating ECS or reserializing
// component values through schemas. Includes are already expanded; editor-only
// hierarchy/camera snapshots are omitted. Relative asset paths are unchanged:
// load with the same asset root, and preserve the source directory semantics.
// Does not write a file or overwrite the authored source.
std::optional<std::string> SerializePreparedScene(const PreparedScene& prepared);

// Owner-thread phase. The prepared root and its includes are not read or parsed
// again. Resolver/materializer pointers are used only here and must be valid for
// this call. ECS creation is still synchronous; callers must not treat this as a
// frame-budgeted loader. Timings include the original preparation phases, not
// time spent waiting between preparation and instantiation. The immutable
// document may be reused; each additive instantiation gets its own namespace.
bool InstantiatePreparedScene(ECS::World& world, const PreparedScene& prepared,
                              const LoadOptions& options = {});

enum class SceneActivationPhase
{
    Creating, Applying, Parenting, Ready, Publishing, Complete, Cancelling, Cancelled, Failed
};

struct SceneActivationOptions
{
    // Non-owning; must outlive the activation, including its cancellation.
    ISceneAssetResolver* assetResolver = nullptr;
    std::filesystem::path assetRootOverride;
};

// Additive, owner-thread staging for prepared flat scene documents. Never clears
// the existing world. Unsupported blueprint/subscene/embed documents fail BEFORE
// any mutation; there is no hidden synchronous fallback. Schema callbacks may
// still do indivisible work, so budgets are checked BETWEEN entity operations,
// not a hard frame-time guarantee. Creation, components and parenting finish
// with entities excluded from native and type-erased queries through a transient
// PendingActivation tag, separate from authored Disabled state. Direct handle
// access and raw archetype enumeration are not hidden. RequestPublish explicitly
// begins query visibility changes; schema callbacks must not publish work through
// other channels or mutate the world/session outside their destination entity.
// Keep presentation covering the destination until publication is Complete.
//
// World, resolver and this session must stay on the owning thread; world must
// outlive the session. Do not modify its staged handles externally. A World::Clear
// invalidates the session, which then leaves replacement entities untouched.
// Normal engine command/hierarchy synchronization remains the caller's job;
// Step never flushes unrelated global command buffers.
class StagedSceneActivation
{
public:
    ~StagedSceneActivation();
    StagedSceneActivation(const StagedSceneActivation&) = delete;
    StagedSceneActivation& operator=(const StagedSceneActivation&) = delete;
    SceneActivationPhase Phase() const;
    std::size_t Step(std::size_t maxOperations, std::chrono::microseconds budget);
    bool RequestPublish();
    void RequestCancel();
    std::span<const ECS::EntityHandle> CreatedEntities() const;
    const SceneLoadDegradation& Degradation() const;
    const SceneIOError& Error() const;

    // Destruction before Complete rolls back synchronously as a safety net.
    // For responsive cancellation call RequestCancel and drain Step first.
    // Complete transfers ownership to the world; destruction then retains the
    // entities. Explicit RequestCancel after Complete can unload this group.
private:
    struct Impl;
    explicit StagedSceneActivation(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> m_Impl;
    friend std::unique_ptr<StagedSceneActivation> BeginStagedSceneActivation(
        ECS::World&, std::shared_ptr<const PreparedScene>, const SceneActivationOptions&);
};

std::unique_ptr<StagedSceneActivation> BeginStagedSceneActivation(
    ECS::World& world, std::shared_ptr<const PreparedScene> prepared,
    const SceneActivationOptions& options = {});

// Load a human-readable .scene file into an ECS world.
// - Replace: clears the world first.
// - Additive: keeps existing entities; loaded entity ids are namespace-prefixed where needed (subscenes/blueprints).
// A false return is reported through GetLastSceneIOError, not the log: the caller owns the outcome
// and logs the record once, at the level the outcome deserves. The loader logs only Debug detail.
bool LoadSceneFromFile(ECS::World& world,
                       const std::filesystem::path& sceneFilePath,
                       const LoadOptions& options = {});

// Serialize the world to a human-readable .scene file (INI-like schema format).
bool SaveSceneToFile(ECS::World& world,
                     const std::filesystem::path& sceneFilePath,
                     const SaveOptions& options = {});

// Helpers for AssetReference encoding in .scene files.
// The path is optional and non-authoritative (debugging only).
struct SceneAssetRef
{
    AssetReference ref{};
    std::string debugPath;
};

// Resolve a resource id (from a `#id` reference) to an AssetReference during scene load.
// Returns false if the load context has no scene file bound.
bool TryResolveResourceIdToAssetReference(const SceneLoadContext& ctx,
                                         std::string_view resourceId,
                                         AssetReference& outRef,
                                         std::string* outError = nullptr);

// Fetch an embed definition (type + properties) during scene load.
// Properties are returned as raw key/value strings (no further parsing).
bool TryGetEmbedDefinition(const SceneLoadContext& ctx,
                           std::string_view embedId,
                           std::string& outType,
                           std::unordered_map<std::string, std::string>& outProperties,
                           std::string* outError = nullptr);

// Format an asset reference for save. Uses ctx.Resolver to heal stale data:
//   - guid present, resolver knows it: emit canonical project-relative path + guid
//   - path present only: look up guid via resolver and emit both
//   - both present, agree: emit as-is (relativized to assetRoot)
//   - both present, guid resolves to a different path: trust guid, emit canonical path
//   - guid only, unknown to the resolver: emit the path the load recorded for it in
//     ctx.Unresolved (see TryResolveAssetReference), so the reference keeps its authored path
//     until the asset returns
//
// If ctx has no resolver, falls back to emitting [path="..." guid="..."]
// with the inputs as-given (path is NOT relativized in that case).
//
// Returns the formatted SceneValue string (e.g. `[path="Models/X.fbx" guid="<g>"]`).
// The caller embeds this verbatim in the line they push into outLines.
std::string FormatAssetReferenceForSave(const SceneSaveContext& ctx,
                                        const GUID& guid,
                                        std::string_view authoredPath);

// Resolve an SceneValue to an AssetReference during scene load. Handles both:
//   - SceneValueKind::String: legacy bare GUID — `"<guid>"`
//   - SceneValueKind::AssetRef: new path+guid form — `[path="..." guid="..."]`
//   - SceneValueKind::ResourceRef: forwards to TryResolveResourceIdToAssetReference
//
// Cross-project recovery: if the GUID isn't known to the registry but a path is
// present, the resolver mints a new GUID for that path (via GetOrCreateAssetGuid).
// Path-only AssetRefs are also accepted and resolved that way.
//
// A GUID that binds to neither a record nor a file loads as a degraded reference; it is recorded
// with its authored path on ctx.TargetWorld's UnresolvedComponentStore so a save writes it back.
//
// Returns false on parse error or when no fields can be resolved.
bool TryResolveAssetReference(const SceneLoadContext& ctx,
                              const SceneValue& value,
                              AssetType expectedType,
                              AssetReference& outRef,
                              std::string* outError = nullptr);

// Format an EntityHandle field for save: the referenced entity's stable scene-id
// string (quoted), resolved via ctx.EntityTagById. Returns "" for an invalid handle
// or one whose target is not part of this save (the caller then omits the line).
std::string FormatEntityReferenceForSave(const SceneSaveContext& ctx, ECS::EntityHandle handle);

// Resolve an EntityHandle field on load: parse the quoted scene-id string and look
// it up (prefixed by ctx.EntityIdPrefix) in ctx.EntityIdMap. Sets out to the live
// handle, or an invalid handle when the reference is empty/unknown (a dangling link
// just unlinks — it never fails the load). Returns false only on malformed text.
bool TryResolveEntityReference(const SceneLoadContext& ctx, std::string_view value, ECS::EntityHandle& out);

} // namespace GameEngine::Scene
