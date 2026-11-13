#pragma once

#include "AssetCore/AssetTypes.h"
#include "AssetCore/GUID.h"
#include "Events/Event.h"
#include "Types/FlatSet.h"

#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace GameEngine
{
namespace ECS { class World; }
class AssetRegistry;
class DeferredActionQueue;
struct EditorContext;

// One scene asset reference whose GUID is unknown to the AssetRegistry.
// Populated by MissingAssetTracker::Rescan; consumed by the Missing Assets
// panel UI and by the AssetsPanel's ghost-thumb rendering.
struct MissingAssetEntry
{
    GUID Guid;
    AssetType Type = AssetType::Unknown;
    std::string AuthoredPath;     // path string from the saved scene, may be empty
    // Last path anyone recorded for this GUID: what the scene text named when
    // the load could not bind the reference, else the registry's tombstoned
    // record. Empty when neither exists — the GUID reached this project with
    // no path attached at all.
    std::string LastKnownPath;
    std::string EntityTag;        // SceneEntityTag of the owning entity (if any)
    std::string ComponentName;    // e.g. "MeshRenderer"
    std::string PropertyName;     // e.g. "material"
};

// Editor-side service that scans the active scene for asset references whose
// GUID is unknown to the AssetRegistry. Single instance owned by EditorApplication.
//
// Thread model: rescans run on the main thread (they walk the ECS world). Reads
// are gated by a shared mutex so listeners can query from the UI thread without
// racing a concurrent rescan. Listeners fire on the rescanning thread — keep
// listener bodies short or `PostAction` back to the UI thread inside.
class MissingAssetTracker
{
  public:
    using Listener = std::function<void()>;
    // Registration handle: unsubscribes when destroyed, and does nothing if the
    // tracker died first. Subscribers are UI panels, which the editor destroys
    // after the services they observe.
    using Subscription = EventSubscription;

    void Clear();

    // Resolve world+registry from `ctx` and listen to its AssetManager's FilesChanged: a batch that
    // can change the missing set queues one rescan per frame on `preUiActions`. Both are borrowed.
    // Pass nullptr to unbind.
    void Bind(EditorContext* ctx, DeferredActionQueue* preUiActions);

    // Resolve world+registry through the bound EditorContext, then dispatch
    // to the explicit-arg implementation. UI callers (panels, controllers)
    // use these — the explicit-arg forms are private to keep the active-
    // world lookup in one place.
    void RescanActive();
    size_t ClearAllReferencesToActive(const GUID& guid);

    // Snapshot accessors. Returned references are stable until the next Rescan.
    std::vector<MissingAssetEntry> GetMissing() const;
    bool IsAssetMissing(const GUID& guid) const;
    size_t GetMissingCount() const;
    uint32_t GetVersion() const;

    // Listeners fire on the rescanning thread. Unsubscribing does not join a
    // callback already running, so a subscriber that can be destroyed
    // mid-callback needs its own liveness flag as well.
    [[nodiscard]] Subscription AddListener(Listener cb);

  private:
    // Explicit-arg implementations. Private because callers should go
    // through the *Active overloads above, which resolve world+registry
    // from the bound EditorContext — keeps the world lookup in one place.
    // Returns true when the set of missing GUIDs changed. Does not notify listeners.
    bool Rescan(ECS::World& world, AssetRegistry& registry);

    // Queues a rescan when the batch names a GUID that the scene referenced or that was missing at
    // the previous scan, or a GUID with no registry record (a deleted file).
    void OnFilesChanged(std::span<const GUID> changed);
    // The queued rescan: notifies listeners only when the missing set changed.
    void RescanForChangedFiles();

    // Clear every component property in `world` whose AssetReference points
    // to `guid`, by invoking each schema's ApplyProperty with the clear-slot
    // sentinel ("0"). Returns the number of slots cleared. Triggers a Rescan
    // when at least one slot was cleared so subscribers (panel UI, asset
    // browser ghost rows) update without an extra round-trip.
    size_t ClearAllReferencesTo(ECS::World& world, AssetRegistry& registry, const GUID& guid);

    // Resolve the bound context's world+registry. Returns false (and logs)
    // if the tracker is unbound or the world isn't ready yet.
    bool ResolveActiveTargets(ECS::World*& outWorld, AssetRegistry*& outRegistry) const;

    mutable std::mutex m_Mutex;
    std::vector<MissingAssetEntry> m_Entries;
    std::unordered_set<GUID> m_MissingGuids;
    // Every GUID the scene referenced at the last rescan, missing or not.
    FlatSet<GUID> m_ReferencedGuids;
    uint32_t m_Version = 0;

    Event<> m_Changed;
    // The bound AssetManager's FilesChanged; the handle is inert when the manager dies first.
    EventSubscription m_FilesChanged;

    EditorContext* m_BoundContext = nullptr;
    DeferredActionQueue* m_PreUiActions = nullptr;
};

} // namespace GameEngine
