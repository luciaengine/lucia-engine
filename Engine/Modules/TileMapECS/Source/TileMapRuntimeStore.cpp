#include "TileMapECS/TileMapRuntimeStore.h"

#include "FileSystem/FileSystem.h"
#include "Types/Fnv1a.h"
#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Core/Engine.h"
#include "Logger/Logger.h"
#include "TileMap/TileMapAsset.h"
#include "TileMap/TileSetAsset.h"

#include <algorithm>
#include <fstream>
#include <cassert>

namespace GameEngine
{
namespace TileMapECS
{

namespace
{

// References may predate a registry identity merge. Cache and invalidation
// keys must use the same identity as the asset event dispatcher.
GUID CanonicalGuid(const GUID& guid)
{
    auto& engine = EngineCore::GetInstance();
    if (guid.IsNull() || !engine.IsInitialized())
        return guid;
    const auto& registry = engine.GetAssetManager().GetRegistry();
    return registry.ResolveSessionAlias(registry.ResolveGuid(guid));
}

/** FNV-1a over the file's bytes; 0 when the file cannot be read. */
uint64 HashFileBytes(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open())
        return 0;
    uint64 hash = Hashing::kFnv1a64OffsetBasis;
    char buffer[4096];
    while (in.read(buffer, sizeof(buffer)) || in.gcount() > 0)
    {
        const std::streamsize count = in.gcount();
        hash = Hashing::Fnv1a64(buffer, static_cast<size_t>(count), hash);
        if (count < static_cast<std::streamsize>(sizeof(buffer)))
            break;
    }
    return hash;
}

} // namespace

std::unique_ptr<TileMapRuntimeStore> TileMapRuntimeStore::s_Instance;

TileMapRuntimeStore::TileMapRuntimeStore() = default;
TileMapRuntimeStore::~TileMapRuntimeStore()
{
    if (m_EventDispatcher && m_RemapCallback != 0)
        m_EventDispatcher->RemoveCallback(m_RemapCallback);
}

void TileMapRuntimeStore::Initialize()
{
    assert(!s_Instance && "TileMapRuntimeStore already initialized");
    s_Instance = std::make_unique<TileMapRuntimeStore>();
    s_Instance->InstallAssetReloadInvalidator();
}

void TileMapRuntimeStore::Shutdown()
{
    s_Instance.reset();
}

TileMapRuntimeStore& TileMapRuntimeStore::Get()
{
    assert(s_Instance && "TileMapRuntimeStore not initialized");
    return *s_Instance;
}

TileMapRuntimeStore* TileMapRuntimeStore::TryGet()
{
    return s_Instance.get();
}

bool TileMapRuntimeStore::IsInitialized()
{
    return s_Instance != nullptr;
}

void TileMapRuntimeStore::InstallAssetReloadInvalidator()
{
    auto& engine = EngineCore::GetInstance();
    if (!engine.IsInitialized())
        return;

    const auto pending = m_PendingEvents;
    m_EventDispatcher = &engine.GetAssetManager().GetEventDispatcher();
    m_RemapCallback = m_EventDispatcher->AddCallback([pending](const AssetEvent& event)
    {
        if (event.EventType != AssetEventType::GuidRemapped)
            return;
        const GUID oldGuid(event.Message);
        if (oldGuid.IsNull() || event.AssetGuid.IsNull() || oldGuid == event.AssetGuid)
            return;
        std::lock_guard lock(pending->Mutex);
        pending->Remaps.push_back(GuidRemap{oldGuid, event.AssetGuid, event.AssetPath});
    });

    // ContentEvents: the store decodes .tilemap files straight from disk, so a
    // plain file edit raises AssetModified and never AssetReloaded (which only
    // fires for assets resident in the AssetManager's loaded set). These can
    // arrive on the watcher thread, so the handler only enqueues.
    m_MapReloadInvalidator = AssetReloadInvalidator(
        engine.GetAssetManager().GetEventDispatcher(), AssetType::TileMap,
        [pending](const GUID& guid)
        {
            std::lock_guard lock(pending->Mutex);
            pending->Maps.push_back(guid);
        },
        AssetReloadInvalidator::EventSet::ContentEvents);

    m_TileSetReloadInvalidator = AssetReloadInvalidator(
        engine.GetAssetManager().GetEventDispatcher(), AssetType::TileSet,
        [pending](const GUID& guid)
        {
            std::lock_guard lock(pending->Mutex);
            pending->TileSets.push_back(guid);
        },
        AssetReloadInvalidator::EventSet::ContentEvents);
}

void TileMapRuntimeStore::NotifyMapFileChanged(const GUID& referencedGuid)
{
    std::lock_guard lock(m_PendingEvents->Mutex);
    m_PendingEvents->Maps.push_back(referencedGuid);
}

void TileMapRuntimeStore::NotifyTileSetFileChanged(const GUID& referencedGuid)
{
    std::lock_guard lock(m_PendingEvents->Mutex);
    m_PendingEvents->TileSets.push_back(referencedGuid);
}

bool TileMapRuntimeStore::IsSelfWriteEcho(const GUID& referencedGuid) const
{
    const GUID mapGuid = ResolveResidentGuid(referencedGuid);
    const auto it = m_SelfWrites.find(mapGuid);
    if (it == m_SelfWrites.end())
        return false;
    const uint64 onDisk = HashFileBytes(it->second.Path);
    return onDisk != 0 && onDisk == it->second.ContentHash;
}

void TileMapRuntimeStore::NotifyGuidRemapped(const GUID& oldGuid, const GUID& newGuid,
                                              const std::filesystem::path& path)
{
    if (oldGuid.IsNull() || newGuid.IsNull() || oldGuid == newGuid)
        return;
    std::lock_guard lock(m_PendingEvents->Mutex);
    m_PendingEvents->Remaps.push_back(GuidRemap{oldGuid, newGuid, path});
}

void TileMapRuntimeStore::RemapResidentGuid(const GUID& oldGuid, const GUID& newGuid) const
{
    if (oldGuid == newGuid || oldGuid.IsNull() || newGuid.IsNull())
        return;

    // Match AssetManager's identity move: the remapped document wins over a
    // speculative copy loaded under the destination identity.
    if (auto node = m_Maps.extract(oldGuid))
    {
        if (const auto destination = m_Maps.find(newGuid); destination != m_Maps.end())
        {
            node.mapped().EditVersion = std::max(node.mapped().EditVersion,
                                                destination->second.EditVersion) + 1;
            node.mapped().StructureVersion = std::max(node.mapped().StructureVersion,
                                                     destination->second.StructureVersion) + 1;
            m_Maps.erase(destination);
        }
        node.key() = newGuid;
        auto& map = m_Maps.insert(std::move(node)).position->second;
        for (const auto& layer : map.Data.Layers())
            MarkLayerAllChunksDirty(map, layer);
        m_SelfWrites.erase(newGuid);
    }
    if (auto node = m_SelfWrites.extract(oldGuid))
    {
        m_SelfWrites.erase(newGuid);
        node.key() = newGuid;
        m_SelfWrites.insert(std::move(node));
    }
    bool movedTileSet = false;
    if (auto node = m_TileSets.extract(oldGuid))
    {
        movedTileSet = true;
        m_TileSets.erase(newGuid);
        node.key() = newGuid;
        m_TileSets.insert(std::move(node));
    }
    if (auto node = m_TileSetVersions.extract(oldGuid))
        m_TileSetVersions[newGuid] = std::max(m_TileSetVersions[newGuid], node.mapped());
    if (movedTileSet)
        ++m_TileSetVersions[newGuid];
    if (m_DecodeWarned.erase(oldGuid) != 0)
        m_DecodeWarned.insert(newGuid);
}

void TileMapRuntimeStore::DrainIdentityRemaps() const
{
    std::vector<GuidRemap> remaps;
    {
        std::lock_guard lock(m_PendingEvents->Mutex);
        remaps.swap(m_PendingEvents->Remaps);
        for (const auto& remap : remaps)
        {
            for (GUID& guid : m_PendingEvents->Maps)
                if (guid == remap.OldGuid)
                    guid = remap.NewGuid;
            for (GUID& guid : m_PendingEvents->TileSets)
                if (guid == remap.OldGuid)
                    guid = remap.NewGuid;
        }
    }
    for (const auto& [oldGuid, newGuid, path] : remaps)
    {
        RemapResidentGuid(oldGuid, newGuid);
        if (!path.empty())
        {
            if (auto write = m_SelfWrites.find(newGuid); write != m_SelfWrites.end())
                write->second.Path = path;
        }
    }
}

GUID TileMapRuntimeStore::ResolveResidentGuid(const GUID& guid) const
{
    // Editor reads and edits can precede the extraction system's frame drain.
    // Apply identity moves before accepting either; file reloads remain deferred.
    DrainIdentityRemaps();
    const GUID resolved = CanonicalGuid(guid);
    RemapResidentGuid(guid, resolved);
    return resolved;
}

void TileMapRuntimeStore::DrainPendingReloads()
{
    DrainIdentityRemaps();
    std::vector<GUID> pendingMaps;
    std::vector<GUID> pendingTileSets;
    {
        std::lock_guard lock(m_PendingEvents->Mutex);
        pendingMaps.swap(m_PendingEvents->Maps);
        pendingTileSets.swap(m_PendingEvents->TileSets);
    }
    // Resolve after the queue drains: aliases can have appeared since enqueue.
    for (const GUID& notified : pendingMaps)
    {
        RemapResidentGuid(notified, CanonicalGuid(notified));
    }
    for (const GUID& notified : pendingTileSets)
    {
        RemapResidentGuid(notified, CanonicalGuid(notified));
    }
    for (const GUID& guid : pendingMaps)
    {
        // A notification for bytes SaveMap itself wrote is an echo, not an
        // external edit; reloading it would discard edits made since the save.
        if (IsSelfWriteEcho(guid))
            continue;
        ReloadMap(guid);
    }
    for (const GUID& notified : pendingTileSets)
    {
        const GUID guid = CanonicalGuid(notified);
        m_TileSets.erase(guid);
        // A re-slice moves the UV rects behind unchanged tile ids, so consumers
        // holding derived state have to rebuild it; the cells themselves are
        // untouched, so the version is the only signal there is.
        ++m_TileSetVersions[guid];
        // Collision geometry is baked from tileset polygons, so a tileset edit
        // re-arms every chunk of every resident map that draws from it.
        InvalidateMapsUsingTileSet(guid);
    }
}

void TileMapRuntimeStore::InvalidateMapsUsingTileSet(const GUID& referencedGuid)
{
    const GUID tileSetGuid = ResolveResidentGuid(referencedGuid);
    for (auto& [mapGuid, map] : m_Maps)
    {
        for (const TileMap::TileMapLayerData& layer : map.Data.Layers())
        {
            if (CanonicalGuid(layer.TileSetGuid) != tileSetGuid)
                continue;
            ++map.EditVersion;
            MarkLayerAllChunksDirty(map, layer);
        }
    }
}

bool TileMapRuntimeStore::LoadMapFromFile(const GUID& referencedGuid, const std::filesystem::path& path)
{
    const GUID mapGuid = ResolveResidentGuid(referencedGuid);
    TileMap::TileMapAsset asset(mapGuid, path);
    if (!asset.Load())
        return false;

    RuntimeMap& map = m_Maps[mapGuid];
    map.Data = std::move(asset.Data());
    ++map.EditVersion;
    ++map.StructureVersion;
    map.DirtyChunks.clear();
    map.CollisionDirtyChunks.clear();
    for (const TileMap::TileMapLayerData& layer : map.Data.Layers())
        MarkLayerAllChunksDirty(map, layer);
    return true;
}

bool TileMapRuntimeStore::SaveMapToFile(const GUID& referencedGuid, const std::filesystem::path& path)
{
    const GUID mapGuid = ResolveResidentGuid(referencedGuid);
    auto it = m_Maps.find(mapGuid);
    if (it == m_Maps.end() || path.empty())
        return false;

    for (TileMap::TileMapLayerData& layer : it->second.Data.Layers())
        layer.CompactEmptyChunks();

    TileMap::TileMapAsset asset(mapGuid, path);
    asset.Data() = it->second.Data;

    Vector<uint8> bytes;
    if (!asset.SaveToData(bytes))
    {
        Logger::Log::Error("TileMapRuntimeStore: failed to serialize tilemap {}", mapGuid.ToString());
        return false;
    }

    std::error_code directoryError;
    std::filesystem::create_directories(path.parent_path(), directoryError);
    const auto temporary = FileSystem::MakeTemporarySiblingPath(path);
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    output.close();
    if (!output.good())
    {
        std::error_code removeError;
        std::filesystem::remove(temporary, removeError);
        return false;
    }
    if (!FileSystem::PublishFile(temporary, path))
        return false;
    m_SelfWrites[mapGuid] = SelfWrite{path, Hashing::Fnv1a64(bytes.data(), bytes.size())};
    return true;
}

bool TileMapRuntimeStore::SaveMap(const GUID& referencedGuid)
{
    const GUID mapGuid = ResolveResidentGuid(referencedGuid);
    auto& engine = EngineCore::GetInstance();
    AssetMetadata meta{};
    if (!engine.IsInitialized() ||
        !engine.GetAssetManager().GetRegistry().TryGetAssetMetadata(mapGuid, meta) ||
        meta.Path.empty())
    {
        return false;
    }
    return SaveMapToFile(mapGuid, meta.Path);
}

void TileMapRuntimeStore::MarkStructuralChange(const GUID& referencedGuid)
{
    const GUID mapGuid = ResolveResidentGuid(referencedGuid);
    auto it = m_Maps.find(mapGuid);
    if (it == m_Maps.end())
        return;
    ++it->second.EditVersion;
    ++it->second.StructureVersion;
    MarkAllChunksDirty(mapGuid);
}

TileMapRuntimeStore::RuntimeMap* TileMapRuntimeStore::FindOrLoad(const GUID& referencedGuid)
{
    const GUID mapGuid = ResolveResidentGuid(referencedGuid);
    if (mapGuid.IsNull())
        return nullptr;

    if (auto it = m_Maps.find(mapGuid); it != m_Maps.end())
        return &it->second;

    // A failure here is retried on the next call rather than cached. Registry
    // resolution is a transient condition — the reference resolves as soon as
    // the asset scan reaches the file — and latching it would leave a map that
    // was merely asked for too early permanently unrenderable. Retrying costs
    // one hash lookup per frame; only the log is deduplicated.
    auto& engine = EngineCore::GetInstance();
    AssetMetadata meta{};
    if (!engine.IsInitialized() ||
        !engine.GetAssetManager().GetRegistry().TryGetAssetMetadata(mapGuid, meta) ||
        meta.Path.empty())
    {
        return nullptr;
    }

    if (!LoadMapFromFile(mapGuid, meta.Path))
    {
        if (m_DecodeWarned.insert(mapGuid).second)
            Logger::Log::Warning("TileMapRuntimeStore: failed to decode tilemap {}",
                                 mapGuid.ToString());
        return nullptr;
    }
    m_DecodeWarned.erase(mapGuid);
    return &m_Maps[mapGuid];
}

TileMap::TileMapData* TileMapRuntimeStore::GetMap(const GUID& referencedGuid)
{
    const GUID mapGuid = ResolveResidentGuid(referencedGuid);
    RuntimeMap* map = FindOrLoad(mapGuid);
    return map ? &map->Data : nullptr;
}

bool TileMapRuntimeStore::LoadTileSetFromFile(const GUID& referencedGuid,
                                              const std::filesystem::path& path)
{
    const GUID tileSetGuid = ResolveResidentGuid(referencedGuid);
    auto asset = std::make_unique<TileMap::TileSetAsset>(tileSetGuid, path);
    if (!asset->Load())
        return false;
    m_TileSets[tileSetGuid] = std::move(asset);
    return true;
}

const TileMap::TileSetAsset* TileMapRuntimeStore::GetTileSet(const GUID& referencedGuid)
{
    const GUID tileSetGuid = ResolveResidentGuid(referencedGuid);
    if (tileSetGuid.IsNull())
        return nullptr;
    if (auto it = m_TileSets.find(tileSetGuid); it != m_TileSets.end())
        return it->second.get();

    // Same retry policy as FindOrLoad: registry resolution is transient.
    auto& engine = EngineCore::GetInstance();
    AssetMetadata meta{};
    if (!engine.IsInitialized() ||
        !engine.GetAssetManager().GetRegistry().TryGetAssetMetadata(tileSetGuid, meta) ||
        meta.Path.empty())
    {
        return nullptr;
    }

    if (!LoadTileSetFromFile(tileSetGuid, meta.Path))
    {
        if (m_DecodeWarned.insert(tileSetGuid).second)
            Logger::Log::Warning("TileMapRuntimeStore: failed to decode tileset {}",
                                 tileSetGuid.ToString());
        return nullptr;
    }
    m_DecodeWarned.erase(tileSetGuid);
    return m_TileSets[tileSetGuid].get();
}

uint64 TileMapRuntimeStore::TileSetVersion(const GUID& referencedGuid) const
{
    const GUID tileSetGuid = ResolveResidentGuid(referencedGuid);
    auto it = m_TileSetVersions.find(tileSetGuid);
    return it != m_TileSetVersions.end() ? it->second : 0;
}

TileMap::TileCell TileMapRuntimeStore::GetCell(const GUID& referencedGuid, uint32 layerId, int32 cellX,
                                               int32 cellY)
{
    const GUID mapGuid = ResolveResidentGuid(referencedGuid);
    RuntimeMap* map = FindOrLoad(mapGuid);
    if (!map)
        return TileMap::kEmptyCell;
    const TileMap::TileMapLayerData* layer = map->Data.FindLayer(layerId);
    return layer ? layer->GetCell(cellX, cellY) : TileMap::kEmptyCell;
}

bool TileMapRuntimeStore::SetCell(const GUID& referencedGuid, uint32 layerId, int32 cellX, int32 cellY,
                                  TileMap::TileCell cell)
{
    const GUID mapGuid = ResolveResidentGuid(referencedGuid);
    RuntimeMap* map = FindOrLoad(mapGuid);
    if (!map)
        return false;
    TileMap::TileMapLayerData* layer = map->Data.FindLayer(layerId);
    if (!layer)
        return false;

    layer->SetCell(cellX, cellY, cell);
    layer->SetAutoCell(cellX, cellY, false);
    MarkCellDirty(*map, layerId, cellX, cellY);
    ++map->EditVersion;
    return true;
}

bool TileMapRuntimeStore::SetCellAuto(const GUID& referencedGuid, uint32 layerId, int32 cellX, int32 cellY,
                                      TileMap::TileCell cell, bool automatic)
{
    const GUID mapGuid = ResolveResidentGuid(referencedGuid);
    RuntimeMap* map = FindOrLoad(mapGuid);
    if (!map)
        return false;
    TileMap::TileMapLayerData* layer = map->Data.FindLayer(layerId);
    if (!layer)
        return false;

    layer->SetCell(cellX, cellY, cell);
    layer->SetAutoCell(cellX, cellY, automatic);
    MarkCellDirty(*map, layerId, cellX, cellY);
    ++map->EditVersion;
    return true;
}

uint8 TileMapRuntimeStore::GetIntGridValue(const GUID& referencedGuid, uint32 layerId, int32 cellX,
                                           int32 cellY)
{
    const GUID mapGuid = ResolveResidentGuid(referencedGuid);
    RuntimeMap* map = FindOrLoad(mapGuid);
    if (!map)
        return 0;
    const TileMap::TileMapLayerData* layer = map->Data.FindLayer(layerId);
    return layer ? layer->GetIntGridValue(cellX, cellY) : 0;
}

bool TileMapRuntimeStore::SetIntGridValue(const GUID& referencedGuid, uint32 layerId, int32 cellX,
                                          int32 cellY, uint8 value)
{
    const GUID mapGuid = ResolveResidentGuid(referencedGuid);
    RuntimeMap* map = FindOrLoad(mapGuid);
    if (!map)
        return false;
    TileMap::TileMapLayerData* layer = map->Data.FindLayer(layerId);
    if (!layer || layer->Type != TileMap::TileMapLayerType::IntGrid)
        return false;

    layer->SetIntGridValue(cellX, cellY, value);
    // An IntGrid layer draws no tiles, so nothing consumes its dirty chunks —
    // but the edit still has to move the version observers poll.
    ++map->EditVersion;
    return true;
}

void TileMapRuntimeStore::MarkCellDirty(RuntimeMap& map, uint32 layerId, int32 cellX, int32 cellY)
{
    const uint64 packed = TileMap::PackChunkCoord(TileMap::ChunkOfCell(cellX, cellY));
    map.DirtyChunks[layerId].insert(packed);
    map.CollisionDirtyChunks[layerId].insert(packed);
}

void TileMapRuntimeStore::RecordDressing(RuntimeMap& map, uint32 layerId,
                                         const std::vector<TileMap::RuleCellChange>& changes,
                                         std::vector<TileMapDressedChange>& outChanges)
{
    for (const TileMap::RuleCellChange& change : changes)
    {
        MarkCellDirty(map, layerId, change.Cell.X, change.Cell.Y);
        outChanges.push_back(TileMapDressedChange{layerId, change});
    }
    if (!changes.empty())
        ++map.EditVersion;
}

bool TileMapRuntimeStore::DressLayersFromSource(const GUID& referencedGuid, uint32 sourceLayerId,
                                                const std::vector<TileMap::CellCoord>& dirtyCells,
                                                std::vector<TileMapDressedChange>& outChanges)
{
    const GUID mapGuid = ResolveResidentGuid(referencedGuid);
    RuntimeMap* map = FindOrLoad(mapGuid);
    if (!map)
        return false;

    bool dressedAny = false;
    std::vector<TileMap::RuleCellChange> changes;
    // Layer ids are collected first: evaluation writes into the layer vector,
    // and holding references across that would be a dangling-pointer trap.
    std::vector<uint32> targets;
    for (const TileMap::TileMapLayerData& layer : map->Data.Layers())
    {
        if (layer.Type == TileMap::TileMapLayerType::Tiles &&
            layer.Rules.SourceLayerId == sourceLayerId && !layer.Rules.IsEmpty())
        {
            targets.push_back(layer.Id);
        }
    }

    for (const uint32 layerId : targets)
    {
        changes.clear();
        if (!TileMap::EvaluateRulesIncremental(map->Data, layerId, dirtyCells, changes))
            continue;
        RecordDressing(*map, layerId, changes, outChanges);
        dressedAny = true;
    }
    return dressedAny;
}

bool TileMapRuntimeStore::DressLayer(const GUID& referencedGuid, uint32 rulesLayerId,
                                     std::vector<TileMapDressedChange>& outChanges)
{
    const GUID mapGuid = ResolveResidentGuid(referencedGuid);
    RuntimeMap* map = FindOrLoad(mapGuid);
    if (!map)
        return false;

    std::vector<TileMap::RuleCellChange> changes;
    if (!TileMap::EvaluateRules(map->Data, rulesLayerId, changes))
        return false;
    RecordDressing(*map, rulesLayerId, changes, outChanges);
    return true;
}

bool TileMapRuntimeStore::ConsumeLedger(DirtyLedger& ledger, uint32 layerId,
                                        std::vector<TileMap::ChunkCoord>& outChunks)
{
    auto layerIt = ledger.find(layerId);
    if (layerIt == ledger.end() || layerIt->second.empty())
        return false;

    outChunks.reserve(layerIt->second.size());
    for (uint64 packed : layerIt->second)
        outChunks.push_back(TileMap::UnpackChunkCoord(packed));
    layerIt->second.clear();

    std::sort(outChunks.begin(), outChunks.end(),
              [](TileMap::ChunkCoord a, TileMap::ChunkCoord b)
              { return a.Y != b.Y ? a.Y < b.Y : a.X < b.X; });
    return true;
}

bool TileMapRuntimeStore::ConsumeDirtyChunks(const GUID& referencedGuid, uint32 layerId,
                                             std::vector<TileMap::ChunkCoord>& outChunks)
{
    const GUID mapGuid = ResolveResidentGuid(referencedGuid);
    outChunks.clear();
    auto mapIt = m_Maps.find(mapGuid);
    if (mapIt == m_Maps.end())
        return false;
    return ConsumeLedger(mapIt->second.DirtyChunks, layerId, outChunks);
}

bool TileMapRuntimeStore::ConsumeCollisionDirtyChunks(const GUID& referencedGuid, uint32 layerId,
                                                      std::vector<TileMap::ChunkCoord>& outChunks)
{
    const GUID mapGuid = ResolveResidentGuid(referencedGuid);
    outChunks.clear();
    auto mapIt = m_Maps.find(mapGuid);
    if (mapIt == m_Maps.end())
        return false;
    return ConsumeLedger(mapIt->second.CollisionDirtyChunks, layerId, outChunks);
}

uint64 TileMapRuntimeStore::EditVersion(const GUID& referencedGuid) const
{
    const GUID mapGuid = ResolveResidentGuid(referencedGuid);
    auto it = m_Maps.find(mapGuid);
    return it != m_Maps.end() ? it->second.EditVersion : 0;
}

uint64 TileMapRuntimeStore::StructureVersion(const GUID& referencedGuid) const
{
    const GUID mapGuid = ResolveResidentGuid(referencedGuid);
    auto it = m_Maps.find(mapGuid);
    return it != m_Maps.end() ? it->second.StructureVersion : 0;
}

void TileMapRuntimeStore::MarkLayerAllChunksDirty(RuntimeMap& map,
                                                  const TileMap::TileMapLayerData& layer)
{
    auto& dirty = map.DirtyChunks[layer.Id];
    auto& collisionDirty = map.CollisionDirtyChunks[layer.Id];
    for (TileMap::ChunkCoord coord : layer.SortedChunkCoords())
    {
        const uint64 packed = TileMap::PackChunkCoord(coord);
        dirty.insert(packed);
        collisionDirty.insert(packed);
    }
}

void TileMapRuntimeStore::MarkAllChunksDirty(const GUID& referencedGuid)
{
    const GUID mapGuid = ResolveResidentGuid(referencedGuid);
    auto it = m_Maps.find(mapGuid);
    if (it == m_Maps.end())
        return;
    for (const TileMap::TileMapLayerData& layer : it->second.Data.Layers())
        MarkLayerAllChunksDirty(it->second, layer);
}

void TileMapRuntimeStore::ReloadMap(const GUID& referencedGuid)
{
    const GUID mapGuid = ResolveResidentGuid(referencedGuid);
    m_Maps.erase(mapGuid);
    // The re-decode is lazy: the next GetMap resolves the (possibly new) path
    // and dirties every chunk, so a delete-then-recreate settles correctly.
}

} // namespace TileMapECS
} // namespace GameEngine
