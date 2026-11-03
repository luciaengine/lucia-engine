#pragma once

#include "AssetCore/AssetReloadInvalidator.h"
#include "AssetCore/GUID.h"
#include "TileMap/TileMapData.h"
#include "TileMap/TileRuleEvaluator.h"

#include <filesystem>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace GameEngine
{
namespace TileMap
{
class TileSetAsset;
} // namespace TileMap

namespace TileMapECS
{

/** One cell a rule pass moved, tagged with the Tiles layer it moved it on. */
struct TileMapDressedChange
{
    uint32 LayerId = 0;
    TileMap::RuleCellChange Change;
};

/**
 * @brief Owns the decoded, mutable TileMapData for every tilemap in play, and
 * the per-(map, layer) dirty-chunk ledger the renderer drains.
 *
 * Every mutation goes through this service: SetCell is the only writer, so the
 * dirty ledger cannot drift from the data. The renderer holds no copy — it
 * re-reads the store and consumes dirty chunks, which is what keeps a one-cell
 * edit to a one-chunk re-upload.
 */
class TileMapRuntimeStore
{
public:
    TileMapRuntimeStore();
    ~TileMapRuntimeStore(); // out of line: members are unique_ptr over forward-declared types

    static void Initialize();
    static void Shutdown();
    static TileMapRuntimeStore& Get();
    static TileMapRuntimeStore* TryGet();
    static bool IsInitialized();

    /**
     * Decoded data for `mapGuid`, loading the .tilemap asset on first use.
     * Null when the GUID resolves to no asset or the asset fails to decode.
     * Failed decodes are retried; only their warning is deduplicated.
     */
    TileMap::TileMapData* GetMap(const GUID& mapGuid);

    /**
     * Decodes the .tilemap at `path` under `mapGuid`, replacing any resident
     * data and dirtying every chunk. GetMap calls this once it has resolved the
     * GUID through the asset registry; callers that already hold a path (asset
     * import, tools) drive it directly.
     */
    bool LoadMapFromFile(const GUID& mapGuid, const std::filesystem::path& path);

    TileMap::TileCell GetCell(const GUID& mapGuid, uint32 layerId, int32 cellX, int32 cellY);

    /**
     * Writes one cell by hand and marks its chunk dirty, clearing the cell's
     * auto flag: a cell someone painted is a cell rule evaluation must not
     * overwrite. False when map or layer is unknown.
     */
    bool SetCell(const GUID& mapGuid, uint32 layerId, int32 cellX, int32 cellY,
                 TileMap::TileCell cell);

    /**
     * Writes one cell and its provenance together. Undo/redo of a rule-dressed
     * cell needs both halves restored, which a hand-write cannot express.
     */
    bool SetCellAuto(const GUID& mapGuid, uint32 layerId, int32 cellX, int32 cellY,
                     TileMap::TileCell cell, bool automatic);

    uint8 GetIntGridValue(const GUID& mapGuid, uint32 layerId, int32 cellX, int32 cellY);

    /**
     * Writes one IntGrid value. Dressing is a separate call: a stroke paints
     * many cells and then re-dresses once, which is both cheaper and the only
     * way an incremental pass sees the whole edit.
     */
    bool SetIntGridValue(const GUID& mapGuid, uint32 layerId, int32 cellX, int32 cellY,
                         uint8 value);

    /**
     * Re-dresses every Tiles layer whose rules read `sourceLayerId`, over the
     * cells `dirtyCells` can have moved. Appends what moved to `outChanges` and
     * dirties the affected chunks for the renderer and the collision baker.
     */
    bool DressLayersFromSource(const GUID& mapGuid, uint32 sourceLayerId,
                               const std::vector<TileMap::CellCoord>& dirtyCells,
                               std::vector<TileMapDressedChange>& outChanges);

    /** Full re-dress of one Tiles layer — what a rule edit needs. */
    bool DressLayer(const GUID& mapGuid, uint32 rulesLayerId,
                    std::vector<TileMapDressedChange>& outChanges);

    /**
     * Moves this layer's dirty chunk coordinates into `outChunks` (sorted by
     * (Y, X) so a consumer's upload order is deterministic) and clears them.
     * False — leaving `outChunks` empty — when nothing was dirty.
     */
    bool ConsumeDirtyChunks(const GUID& mapGuid, uint32 layerId,
                            std::vector<TileMap::ChunkCoord>& outChunks);

    /**
     * The collision system's own dirty ledger, fed by the same mutations as the
     * renderer's but drained independently — two consumers on one queue would
     * starve each other. Same contract as ConsumeDirtyChunks.
     */
    bool ConsumeCollisionDirtyChunks(const GUID& mapGuid, uint32 layerId,
                                     std::vector<TileMap::ChunkCoord>& outChunks);

    /**
     * Decoded tileset for `guid`, loading the .tileset asset on first use and
     * dropping it on a hot-reload event. Null when the GUID resolves to no
     * asset or the asset fails to decode.
     */
    const TileMap::TileSetAsset* GetTileSet(const GUID& tileSetGuid);

    /**
     * Decodes the .tileset at `path` under `tileSetGuid`, replacing any
     * resident copy. GetTileSet drives this through the asset registry; tests
     * and tools that hold a path call it directly.
     */
    bool LoadTileSetFromFile(const GUID& tileSetGuid, const std::filesystem::path& path);

    /**
     * Bumps every time this tileset is re-decoded, so a consumer holding derived
     * state (the renderer's GPU UV/animation tables, an atlas binding) can tell
     * that its copy predates the current tiles. 0 for a tileset that has never
     * reloaded. Re-slicing a tileset changes the UV rects behind unchanged tile
     * ids, so nothing about the map's cells signals the change.
     */
    uint64 TileSetVersion(const GUID& tileSetGuid) const;

    /**
     * Serializes the resident map back to `path`, compacting empty chunks first
     * so a stroke that erased a region does not leave the chunk behind. The
     * compaction is deliberately part of the save rather than of SetCell: a
     * mid-stroke compaction would drop a chunk the very next cell re-creates.
     */
    bool SaveMapToFile(const GUID& mapGuid, const std::filesystem::path& path);

    /**
     * Writes the map back to the path the asset registry resolves for its GUID.
     * False when the GUID resolves to no asset, or the write fails.
     */
    bool SaveMap(const GUID& mapGuid);

    /**
     * Records a change to the map's layer *structure* (add, remove, rename).
     * Cell writes maintain the dirty ledger themselves; a structural change has
     * no cell to attribute, so it re-dirties everything and bumps the version
     * that observers poll.
     */
    void MarkStructuralChange(const GUID& mapGuid);

    /** Bumps on every accepted mutation and on re-decode. 0 for an unknown map. */
    uint64 EditVersion(const GUID& mapGuid) const;

    /**
     * Bumps only when the layer set or its bindings change (MarkStructuralChange,
     * a load or re-decode), never on a cell write. An observer that lists
     * layers polls this and stays quiet through a paint stroke.
     */
    uint64 StructureVersion(const GUID& mapGuid) const;

    /** Marks every chunk of every layer dirty (re-decode, first render). */
    void MarkAllChunksDirty(const GUID& mapGuid);

    /**
     * Drops the decoded data so the next GetMap re-reads it from disk, and
     * re-dirties every chunk. Called by the hot-reload lane; also usable
     * directly when an editor rewrites the asset behind the store.
     */
    void ReloadMap(const GUID& mapGuid);

    /**
     * Enqueues a map file-changed notification, exactly as the asset watcher
     * does (thread-safe; the reload happens at the next drain). The store's
     * own SaveMap writes echo back through this path — suppression of that
     * echo happens at drain time, by content hash.
     */
    void NotifyMapFileChanged(const GUID& mapGuid);

    /** The same enqueue for a tileset file; the drain drops it and bumps its version. */
    void NotifyTileSetFileChanged(const GUID& tileSetGuid);

    /**
     * Drains hot-reload notifications raised on the watcher thread. A
     * notification whose file content still matches what SaveMap last wrote is
     * the store's own write echoing back and is ignored — reloading it would
     * discard any edit made between the write and this drain. A genuinely
     * external edit hashes differently and reloads.
     */
    void DrainPendingReloads();

    /** Queues an identity move; resident edits follow it at the next drain. */
    void NotifyGuidRemapped(const GUID& oldGuid, const GUID& newGuid,
                            const std::filesystem::path& path = {});

private:
    // Packed chunk coords per layer id. A layer with no entry has nothing dirty.
    using DirtyLedger = std::unordered_map<uint32, std::unordered_set<uint64>>;

    struct RuntimeMap
    {
        TileMap::TileMapData Data;
        uint64 EditVersion = 0;
        uint64 StructureVersion = 0;
        DirtyLedger DirtyChunks;          // drained by the render feature
        DirtyLedger CollisionDirtyChunks; // drained by the collision system
    };

    struct SelfWrite
    {
        std::filesystem::path Path;
        uint64 ContentHash = 0;
    };

    void InstallAssetReloadInvalidator();
    void RemapResidentGuid(const GUID& oldGuid, const GUID& newGuid) const;
    void DrainIdentityRemaps() const;
    GUID ResolveResidentGuid(const GUID& guid) const;
    /** True when the file for `mapGuid` still holds the bytes SaveMap last wrote. */
    bool IsSelfWriteEcho(const GUID& mapGuid) const;
    RuntimeMap* FindOrLoad(const GUID& mapGuid);
    static void MarkCellDirty(RuntimeMap& map, uint32 layerId, int32 cellX, int32 cellY);
    static void RecordDressing(RuntimeMap& map, uint32 layerId,
                               const std::vector<TileMap::RuleCellChange>& changes,
                               std::vector<TileMapDressedChange>& outChanges);
    static void MarkLayerAllChunksDirty(RuntimeMap& map, const TileMap::TileMapLayerData& layer);
    static bool ConsumeLedger(DirtyLedger& ledger, uint32 layerId,
                              std::vector<TileMap::ChunkCoord>& outChunks);
    void InvalidateMapsUsingTileSet(const GUID& tileSetGuid);

    mutable std::unordered_map<GUID, RuntimeMap> m_Maps;
    mutable std::unordered_map<GUID, std::unique_ptr<TileMap::TileSetAsset>> m_TileSets;
    // Reload counter per tileset; only tilesets that have reloaded have an entry.
    mutable std::unordered_map<GUID, uint64> m_TileSetVersions;
    // Last bytes SaveMap wrote per map, for echo suppression (main thread only).
    mutable std::unordered_map<GUID, SelfWrite> m_SelfWrites;
    // Decode failures already reported. Gates only the log — a broken .tilemap
    // is still retried, so fixing the file on disk recovers without a restart.
    mutable std::unordered_set<GUID> m_DecodeWarned;

    AssetReloadInvalidator m_MapReloadInvalidator;
    AssetReloadInvalidator m_TileSetReloadInvalidator;
    struct GuidRemap
    {
        GUID OldGuid;
        GUID NewGuid;
        std::filesystem::path Path;
    };

    struct PendingEvents
    {
        std::mutex Mutex;
        std::vector<GUID> Maps;
        std::vector<GUID> TileSets;
        std::vector<GuidRemap> Remaps;
    };
    // Dispatcher callbacks can outlive removal after being copied for delivery.
    // They retain only this queue, never the store being destroyed.
    std::shared_ptr<PendingEvents> m_PendingEvents = std::make_shared<PendingEvents>();
    AssetEventDispatcher* m_EventDispatcher = nullptr;
    uint32 m_RemapCallback = 0;

    static std::unique_ptr<TileMapRuntimeStore> s_Instance;
};

} // namespace TileMapECS
} // namespace GameEngine
