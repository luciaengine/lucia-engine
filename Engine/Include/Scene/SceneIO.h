#pragma once

#include "AssetCore/AssetRegistry.h" // AssetReference
#include "AssetCore/GUID.h"
#include "Components/SceneEntityTag.h"
#include "ECS/Entity.h"
#include "Scene/SceneAssetResolver.h"
#include "Scene/SceneEmbedMaterializer.h"
#include "Scene/SceneIOContext.h"
#include "Scene/SceneValue.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
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

// What WriteSceneFile returns. A set error means the call failed. It converts to true when no error is
// set.
struct SceneIOResult
{
    std::optional<SceneIOError> Error;

    explicit operator bool() const { return !Error.has_value(); }
};

// Everything one load could not apply. Empty => the scene loaded cleanly.
struct SceneDegradation
{
    // What a load-report row records: a line the load could not apply, an override line of a prefab instance
    // whose address names no entity of the prefab, or an override line under a child that the same instance
    // removes.
    enum class SkipKind
    {
        Unapplied,
        OrphanedOverride,
        UnderRemovedEntity,
    };

    // One component or field assignment the loader could not apply. The load CONTINUED past it, so the
    // entity exists and everything else about it is live — this records exactly what did not make it in.
    // A scene authored against a newer build than the one opening it (a field or an enumerator that this
    // binary does not know), a field name the component does not have (renamed, removed or misspelled),
    // or a value whose syntax this parser cannot read at all produces one of these per assignment instead
    // of costing the user the whole scene. A component type no module registers is kept, not recorded
    // here. Structural errors are not in this class and still fail the load: a dangling
    // entity or resource reference is a claim about the document, not a value a component can decline.
    struct Skip
    {
        std::string entityId;       // instantiated scene entity id (include/prefab prefix applied)
        std::string component;      // authored component name
        std::string field;          // authored property key; empty when the whole component failed
        std::string message;        // the schema's error, or the fix for a field the component does not have
        std::filesystem::path file; // file the line came from (an include, for included entities)
        int line = 0;               // 0 when unknown

        // The authored text was kept in the world's preserved-field side table AT LOAD TIME, so a save
        // re-emits it verbatim rather than writing the default the field fell back to. False means the
        // assignment exists nowhere but this record, and saving WILL drop it.
        //
        // A load-time answer, and the override can retire afterwards — see SkipIsOutstanding, which is
        // what any surface reporting on a save's behaviour must ask instead.
        bool preserved = false;

        // The entity the row describes, so SkipIsOutstanding can ask the live world about it without
        // resolving `entityId` back through the scene tags; the instance root for an OrphanedOverride or an
        // UnderRemovedEntity row. Null when the row describes no entity.
        ECS::EntityHandle entityHandle{};

        // The registered type of `component` on a field row, so SkipIsOutstanding can ask whether the
        // entity still carries it. 0 on a row that names no field or no registered component.
        ECS::ComponentTypeId componentType = 0;

        SkipKind kind = SkipKind::Unapplied;
        // The address under the instance root that an OrphanedOverride or an UnderRemovedEntity row names
        // (entity ids joined by '.'); else empty.
        std::string address;
    };

    std::vector<Skip> skips;

    bool IsDegraded() const { return !skips.empty(); }

    // Skips whose text was NOT preserved — i.e. what saving this scene would actually lose. The
    // distinction is the whole point: a fully-preserved degraded scene round-trips without loss,
    // and telling the user otherwise would be crying wolf.
    //
    // On the outstanding view (see SkipIsOutstanding), this is what a save drops now: a row whose
    // entity or component is gone does not count.
    std::size_t DroppedCount() const
    {
        std::size_t n = 0;
        for (const Skip& s : skips)
            n += s.preserved ? 0u : 1u;
        return n;
    }
};

// Whether this skip still describes text a save has to answer for, asked of the live world on each
// call. A never-preserved row stands while its entity (and, on a field row, its component) is alive,
// or until a save when it names no entity. A preserved row stands while the world holds its override.
// An UnderRemovedEntity row never stands: the instance removes the child, and a save drops the line.
bool SkipIsOutstanding(const ECS::World& world, const SceneDegradation::Skip& skip);

// How many of a census's skips are still outstanding against `world`. See SkipIsOutstanding.
std::size_t OutstandingSkipCount(const ECS::World& world, const SceneDegradation& census);

// Optional editor snapshot stored under [hierarchy_ui] in .scene files (foldout expansion + selection by SceneEntityTag).
struct SceneHierarchyUi
{
    std::vector<std::string> expandedSceneEntityTags;
    std::vector<std::string> selectionSceneEntityTagsOrdered;
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

// The phase split of one read and its instantiate, in milliseconds. ReadSceneDocument fills the read's
// phases, InstantiateSceneDocument ClearMs and InstantiateMs, ReadSceneInto all of them. A phase that did
// not run (an early failure, or Clear when ClearWorldFirst is false) stays 0.
struct SceneReadTimings
{
    double ReadMs = 0.0;
    double ParseMs = 0.0;     // tokenise + include expansion
    double ValidateMs = 0.0;
    double SourcesMs = 0.0;   // read, parse and validate each prefab and subscene source
    double ClearMs = 0.0;     // World::Clear, only when ClearWorldFirst
    double InstantiateMs = 0.0;
};

// How ReadSceneDocument finds files. The document keeps a copy: InstantiateSceneDocument resolves asset
// references through the same resolver, so the resolver must outlive the document's instantiate.
struct SceneReadOptions
{
    // Optional resolver for GUID/path/asset-root integration.
    // If null, SceneIO falls back to best-effort lexical GUID derivation and extension-based typing.
    ISceneAssetResolver* AssetResolver = nullptr;

    // Optional override for the project's asset root. If empty and AssetResolver is provided,
    // SceneIO uses AssetResolver->GetAssetRoot(). If both are empty, SceneIO falls back to paths
    // relative to the scene file's directory.
    std::filesystem::path AssetRootOverride;

    // The project directory. A file the loader names in an error message is spelled relative to it
    // ("Assets/props/crate.prefab"), the way the user finds it; empty, or a file outside it, the
    // message spells the path absolute.
    std::filesystem::path ProjectRoot;
};

// How InstantiateSceneDocument puts a document into a world.
struct SceneInstantiateOptions
{
    // True: clear the world, then place the scene in it. False: keep the world's entities; the scene's
    // entity ids get a unique "import_N." prefix so they cannot collide with them.
    bool ClearWorldFirst = true;

    // Optional embed executor/materializer. If provided, embed references (#id where id is from [embed])
    // can be materialized into real loadable assets (e.g., embedded materials).
    ISceneEmbedMaterializer* EmbedMaterializer = nullptr;
};

struct SceneWriteOptions
{
    bool ensureSceneEntityTags = true; // assign SceneEntityTag to entities that lack one

    // If true, the file is a prefab file: it starts with the [prefab version=1] header. The world
    // must hold one root entity, with the id "root", and every other id must be a valid prefab id.
    bool writePrefabHeader = false;

    // If true, the [scene] header holds the world's simulation tick (simulation_tick=N), and a load
    // that clears the world restores it. A game save sets it, so the systems that count simulation
    // ticks continue after a load. An authored scene leaves it false. A prefab file ignores it.
    bool writeSimulationTick = false;

    // If true, each prefab instance is saved as a [prefab] section (with its [resource] line) that holds
    // what the instance changes in its prefab, rather than flattened into [entity] blocks. The save
    // reads each prefab's path from assetResolver and refuses an instance it cannot name.
    bool preservePrefabInstances = true;

    // If true, entities that were loaded from [subscene] sections and tagged in-world will be re-saved
    // back to [subscene] sections (with [resource] lines), rather than being flattened into [entity] blocks.
    bool preserveSubscenes = true;

    // Optional resolver for GUID/path lookups during save. If provided, WriteSceneFile will try
    // to fill missing resource guid= fields (best-effort) using the resolver.
    ISceneAssetResolver* assetResolver = nullptr;

    // Optional asset root override (used when resolving relative resource paths).
    std::filesystem::path assetRootOverride;

    // When non-null, appended as a trailing [hierarchy_ui] section (editor foldout/selection by SceneEntityTag).
    const SceneHierarchyUi* hierarchyUi = nullptr;

    // When non-null, appended as a trailing [editor_camera] section with the scene-view camera pose.
    const SceneEditorCamera* editorCamera = nullptr;
};

// Thread-local failure record for parser-only preparation, projection and staged-session creation.
// Document reads, instantiation and writes return their errors directly.
const SceneIOError& GetLastSceneIOError();
void ClearLastSceneIOError();

// Immutable, owned parse/validation result. Preparing does not access an ECS
// world, register schemas, resolve assets or submit GPU work. It can run on a
// worker if the configured file reader supports it and is not being changed.
// Failure returns null; GetLastSceneIOError belongs to the preparing thread.
// Includes are captured now. Referenced prefab/subscene files retain the
// existing late-load semantics during instantiation; this is not a frozen
// dependency package or a promise of nonblocking instantiation.
struct PreparedScene;
std::shared_ptr<const PreparedScene> PrepareSceneFromFile(
    const std::filesystem::path& sceneFilePath, SceneReadTimings* outTimings = nullptr);

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
// prefab/subscene/embed documents and unparseable property values fail closed.
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
// Referenced sources are frozen at this call; errors and timings are returned in the result.
struct SceneInstantiateResult;
SceneInstantiateResult InstantiatePreparedScene(ECS::World& world, const PreparedScene& prepared,
                                                const SceneReadOptions& readOptions = {},
                                                const SceneInstantiateOptions& instantiateOptions = {});

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
// the existing world. Unsupported prefab/subscene/embed documents fail BEFORE
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
    const SceneDegradation& Degradation() const;
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

// The parsed files of one scene read. Private to the scene IO sources.
struct SceneDocumentFiles;

// One .scene file read with no world: the file with its includes expanded, and each distinct prefab and
// subscene source that it names (and that those sources name), each read, parsed and validated once, by
// the path that the read resolved. Copies share the files.
struct SceneDocument
{
    std::filesystem::path Path;
    std::shared_ptr<const SceneDocumentFiles> Files; // null when the read failed
    std::optional<SceneHierarchyUi> HierarchyUi;     // the [hierarchy_ui] section, when the file has one
    std::optional<SceneEditorCamera> EditorCamera;   // the [editor_camera] section, when the file has one
};

// What ReadSceneDocument returns. A set error means the read failed, and the document holds no files.
// It converts to true when no error is set.
struct SceneReadResult
{
    SceneDocument Document;
    std::optional<SceneIOError> Error;
    SceneReadTimings Timings; // ReadMs, ParseMs, ValidateMs and SourcesMs

    explicit operator bool() const { return !Error.has_value(); }
};

// What InstantiateSceneDocument and ReadSceneInto return. A set error means the call failed; its
// worldCleared says whether the world was cleared first. The degradation holds every line that did not
// apply, also on a failure after the clear. It converts to true when no error is set.
struct SceneInstantiateResult
{
    std::optional<SceneIOError> Error;
    SceneDegradation Degradation;
    SceneReadTimings Timings; // ClearMs and InstantiateMs; every phase from ReadSceneInto

    explicit operator bool() const { return !Error.has_value(); }
};

// Read a .scene file and each prefab and subscene source that it names, with no world. It can run on a
// worker thread when options.AssetResolver can. It refuses a source loop, and the error is in the result.
SceneReadResult ReadSceneDocument(const std::filesystem::path& sceneFilePath, const SceneReadOptions& options = {});

// Put a document's entities into the world, on the main thread. It reads no file and resolves no source
// path again. When options.ClearWorldFirst is false, the scene's entity ids get a unique "import_N." prefix.
SceneInstantiateResult InstantiateSceneDocument(ECS::World& world,
                                                const SceneDocument& document,
                                                const SceneInstantiateOptions& options = {});

// ReadSceneDocument and InstantiateSceneDocument in one call on the main thread: the Player's scene load,
// a game's level load, tools and tests. The error is in the result, not the log: the caller owns the
// outcome and logs it once, at the level the outcome deserves. The loader logs only Debug detail.
SceneInstantiateResult ReadSceneInto(ECS::World& world,
                                     const std::filesystem::path& sceneFilePath,
                                     const SceneReadOptions& readOptions = {},
                                     const SceneInstantiateOptions& instantiateOptions = {});

// Serialize the world to a human-readable .scene file (INI-like schema format). The error is in the
// result.
SceneIOResult WriteSceneFile(ECS::World& world,
                             const std::filesystem::path& sceneFilePath,
                             const SceneWriteOptions& options = {});

// Helpers for AssetReference encoding in .scene files.
// The path is optional and non-authoritative (debugging only).
struct SceneAssetRef
{
    AssetReference ref{};
    std::string debugPath;
};

// Resolve a resource id (from a `#id` reference) to an AssetReference during scene load.
// Returns false if the load context has no scene file bound.
bool TryResolveResourceIdToAssetReference(const SceneReadContext& ctx,
                                          std::string_view resourceId,
                                          AssetReference& outRef,
                                          std::string* outError = nullptr);

// Fetch an embed definition (type + properties) during scene load.
// Properties are returned as raw key/value strings (no further parsing).
bool TryGetEmbedDefinition(const SceneReadContext& ctx,
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
std::string FormatAssetReferenceForSave(const SceneWriteContext& ctx,
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
bool TryResolveAssetReference(const SceneReadContext& ctx,
                              const SceneValue& value,
                              AssetType expectedType,
                              AssetReference& outRef,
                              std::string* outError = nullptr);

// Format an EntityHandle field for save: the referenced entity's stable scene-id
// string (quoted), resolved via ctx.EntityTagById. Returns "" for an invalid handle
// or one whose target is not part of this save (the caller then omits the line).
std::string FormatEntityReferenceForSave(const SceneWriteContext& ctx, ECS::EntityHandle handle);

// Resolve an EntityHandle field on load: parse the quoted scene-id string and look
// it up (prefixed by ctx.EntityIdPrefix) in ctx.EntityIdMap. Sets out to the live
// handle, or an invalid handle when the reference is empty/unknown (a dangling link
// just unlinks — it never fails the load). Returns false only on malformed text.
bool TryResolveEntityReference(const SceneReadContext& ctx, std::string_view value, ECS::EntityHandle& out);

} // namespace GameEngine::Scene
