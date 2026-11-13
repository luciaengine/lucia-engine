#pragma once

#include "Components/Hierarchy.h"
#include "ECS/Entity.h"
#include "Panels/HierarchyPanel.h"
#include "UI/Controls/TreeView.h"
#include "VersionControl/Ui/HierarchyVcsController.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace GameEngine
{

// The Hierarchy's row model: one root row, the scene's entities under it by their Parent, sibling order
// by the panel's sort mode. Rebuild builds it whole; the Apply* operations keep it current per entity.
class HierarchyDataProvider final : public TreeChangeTrackingProvider
{
  public:
    using EntityHandle = ECS::EntityHandle;
    using World = ECS::World;
    using Parent = Components::Parent;

    // The tree id of the single root row; every top-level entity is its child.
    static constexpr TreeId kRootId = Editor::kHierarchyRootTreeId;

    // Above this many per-Update deltas (created + destroyed entities, or actual reparent
    // moves) the per-entity ops lose to a single full Rebuild: each sorted-insert / erase into
    // a large sibling list is O(list), so an N-sized delta degrades to O(N^2) vector shifts
    // (a 100k bench spawn would memmove gigabytes and hang for seconds). At/under the threshold
    // the per-entity path wins; over it, Rebuild is the always-correct fallback (and reseeds all
    // bookkeeping). 512 is comfortably above normal editor/gameplay per-frame churn yet far below
    // the point where O(N^2) bites.
    static constexpr std::size_t kIncrementalDeltaMax = 512;

    // Whether the Hierarchy lists `e`. Physics world settings live in Project Settings > Physics. A
    // generated entity (RuntimeOnlyEntity) is rebuilt from its owner, and the owner's row stands for it.
    static bool IsListed(const World& world, const EntityHandle& e);

    // `e` when the Hierarchy lists it, else its nearest listed ancestor; invalid when no row stands for it.
    static EntityHandle ListedSelfOrAncestor(const World& world, EntityHandle e);

    // Writes to `ids` one row per picked entity, and the owner's row once for its generated picks.
    // Returns the row of the first pick that has a row, or 0 when no pick has one.
    static TreeId ListedRowIds(const World& world, std::span<const EntityHandle> picks, std::vector<TreeId>& ids);

    explicit HierarchyDataProvider(World* world)
        : m_World(world) {}

    void SetWorld(World* world);

    void SetSortMode(HierarchySortMode mode) { m_SortMode = mode; }
    void SetSortDirection(HierarchySortDirection dir) { m_SortDirection = dir; }
    void Rebuild();

    // ------------------------------------------------------------------
    // Incremental maintenance (design §2 Tier 1+2). Each op mutates the built
    // adjacency for the touched entity/entities only, ending with a structure/subset
    // change signal so TreeView pays only its O(visible) flat walk — not a full rebuild.
    // ------------------------------------------------------------------

    struct DiffOutcome
    {
        bool AnyDestroyed = false;        // a tracked row vanished → caller revalidates selection
        bool AnyMembershipChange = false; // any entity created OR destroyed this batch
        bool Escalate = false;            // delta too large for per-entity ops → caller full-rebuilds
    };

    // Entity create/destroy authority (there are no entity-level lifecycle events, only
    // per-component ones). Merge-diffs the live entity set against the tracked set — no
    // component reads, no sorting — then applies per-delta ops. Also the swap-generation
    // gap-heal backstop. Nothing is applied when the delta exceeds kIncrementalDeltaMax
    // (F1: per-entity inserts into large sibling lists are O(list), so a mass spawn/despawn
    // would be O(N^2)); the caller full-rebuilds instead.
    DiffOutcome ApplyIdentityDiff();

    // C6 (visibility inversion): an entity can be query-visible a frame before its Added
    // event, and the identity diff may have already inserted it — so this is insert-if-absent.
    void ApplyCreated(const EntityHandle& e);

    void ApplyDestroyed(const EntityHandle& e);

    // Idempotent within a window (LifecycleEvents contract): compares the current Parent
    // against tracked placement and no-ops when unchanged — so re-delivered Added/Removed
    // spans and chunk-granular Changed<Parent> over-reports cost nothing. Returns true only
    // when the entity actually moved (the caller counts real moves for the F1 escalation; a
    // no-op is O(1) so over-reports don't count against the threshold).
    bool ApplyReparented(const EntityHandle& e);

    // Tier 1: label update + Subset rebind (no flat rebuild). Under name-sensitive sort modes
    // (Alphabetical / Type) a rename can reorder siblings, so escalate to a one-list resort —
    // but only RECORD the touched list (FlushPendingResorts sorts each once). F2: early-return
    // when the freshly computed label is unchanged, so the chunk-stamp over-report (an archetype
    // move stamps every column of source+dest chunks, so Changed<Name> reports ~a chunk of
    // unrelated entities per unrelated create/destroy) costs one string compare, not a resort.
    void ApplyRenamed(const EntityHandle& e);

    // Sort each sibling list recorded by ApplyRenamed exactly once. Called by the panel after a
    // rename batch (the Changed<Name> scan and the editor Name-commit path both flush).
    void FlushPendingResorts();

// Debug-config-only parity check. `!defined(NDEBUG)` catches unoptimized debug builds on every
// compiler (incl. macOS/clang Debug where the MSVC-ism `_DEBUG` is unset); `!defined(GE_DEBUGFAST)`
// then excludes this engine's DebugFast config, which seeds Debug flags (so NDEBUG is unset there
// too) but is the daily-driver perf config that must NOT pay a per-frame Rebuild. Release defines
// NDEBUG and is excluded by the first clause.
#if !defined(NDEBUG) && !defined(GE_DEBUGFAST)
    // Behavioral-parity cross-check (design §4): the incremental result must be
    // byte-equivalent to a fresh Rebuild after every mutation batch. Save the incremental
    // state, rebuild from scratch, compare, then restore the incremental state so latent
    // cross-frame accumulation bugs surface instead of being masked.
    void DebugVerifyAgainstRebuild();
#endif

    // Baseline-only rows, injected by the VCS controller. The provider stores
    // and orders them but knows nothing about version control.
    struct GhostRow
    {
        TreeId Id = 0;
        TreeId ParentId = kRootId;
        std::string Label;
    };

    void SetGhostRows(std::vector<GhostRow> rows);

    int GhostChildCount(TreeId parent) const;

    // The entity's children in the order the tree lists them; empty when it has none.
    std::span<const EntityHandle> ChildrenOf(const EntityHandle& entity) const;

    // ITreeDataProvider
    int GetRootCount() const override { return 1; }
    TreeId GetRootId(int) const override { return kRootId; }
    int GetChildCount(TreeId parent) const override;
    TreeId GetChildId(TreeId parent, int index) const override;
    const char* GetLabel(TreeId id) const override;
    bool IsExpandable(TreeId id) const override;

    std::unordered_map<TreeId, std::vector<TreeId>> m_GhostChildren;
    std::unordered_map<TreeId, std::string> m_GhostLabels;

    static TreeId Encode(const EntityHandle& e);

    static EntityHandle Decode(TreeId id);

  private:
    // Decorated sort key for one entity under the active sort mode. Only the field(s) the mode
    // needs are filled; KeyLess reads only those. Shared by Rebuild's decorate-sort AND the
    // incremental sorted-inserts (via KeyOf/KeyLess/SortGroup) — one key helper so the two
    // ordering paths cannot diverge (the correctness crux, design §3).
    struct SortKey
    {
        uint32_t Id = 0;         // unique tie-break (EntityHandle::id) — makes KeyLess a total order
        std::int32_t Order = 0;  // Custom mode
        int TypeRank = 0;        // Type mode
        std::string NameKey;     // lowercased; Alphabetical / Type modes
    };
    struct SortScratchEntry
    {
        EntityHandle Handle{};
        SortKey Key;
    };

    std::int32_t OrderOf(const EntityHandle& h) const;
    std::string LowerNameOf(const EntityHandle& h) const;
    int TypeRankOf(const EntityHandle& h) const;

    // Read one entity's sort key (per active mode) from the ECS. One read per key field.
    SortKey KeyOf(const EntityHandle& e) const;

    // Strict total order matching Rebuild's old `entryLess` exactly, with direction applied.
    bool KeyLess(const SortKey& a, const SortKey& b) const;
    bool KeyLessAsc(const SortKey& a, const SortKey& b) const;

    // Decorate-sort one sibling list in place (Rebuild + rename escalation).
    void SortGroup(std::vector<EntityHandle>& group);

    // Insert e into a sorted sibling list at the position dictated by KeyLess (the same total
    // order SortGroup produces), so a sequence of inserts equals a full std::sort of the set.
    void SortedInsert(std::vector<EntityHandle>& list, const EntityHandle& e);

    // Remove e from the sibling list identified by its stored parent key; drops an emptied
    // m_Children bucket so the map key-set matches a fresh Rebuild (which never creates empty
    // buckets). Linear find by id: e's sort key may have changed since insertion (an unresorted
    // rename), so a key-based binary search could miss it.
    void RemoveFromSiblingList(TreeId storedParent, const EntityHandle& e);

    // Desired sibling-list key for e's CURRENT Parent: the parent's key if it is a tracked
    // (alive + visible) entity, else kRootId. Absent/invalid/dangling Parent ⇒ root — matches
    // Rebuild's alive-check semantics.
    TreeId DesiredParentKey(const EntityHandle& e) const;

    // Label rule shared by Rebuild/ApplyCreated (unconditional set) and ApplyRenamed (compare
    // first, F2). Name component if present, else the entity-id fallback.
    void ComputeLabel(const EntityHandle& e, std::string& out) const;

    void BuildLabel(const EntityHandle& e) { ComputeLabel(e, m_Labels[Encode(e)]); }

    // (Re)build the child→parent position index from the current adjacency. Called at the end
    // of every full Rebuild so the incremental ops have correct bookkeeping to mutate.
    void SeedParentKeyIndex();

    World* m_World = nullptr; // not owned
    std::vector<EntityHandle> m_Roots;
    std::unordered_map<uint32_t, std::vector<EntityHandle>> m_Children; // key = parent id
    std::unordered_map<TreeId, std::string> m_Labels;

    // Child key (Encode as uint32) → its sibling list key: kRootId for a root, else the parent's
    // TreeId. Doubles as the tracked-entity membership set for the identity diff and the O(1)
    // position lookup the reparent/destroy ops need. Kept in lockstep with m_Roots/m_Children.
    std::unordered_map<uint32_t, TreeId> m_EntityParentKey;

    std::vector<EntityHandle> m_SnapshotScratch; // reused alive-snapshot buffer (Rebuild + diff)
    std::vector<EntityHandle> m_DiffCreated;     // reused identity-diff delta buffers
    std::vector<EntityHandle> m_DiffDestroyed;
    std::vector<SortScratchEntry> m_SortScratch; // reused across rebuilds and sibling groups
    std::unordered_set<TreeId> m_PendingResortParents; // sibling lists to resort (coalesced renames)
    std::string m_LabelScratch;                  // reused by ApplyRenamed's compare-then-assign
    HierarchySortMode m_SortMode = HierarchySortMode::Custom;
    HierarchySortDirection m_SortDirection = HierarchySortDirection::Ascending;
};

} // namespace GameEngine
