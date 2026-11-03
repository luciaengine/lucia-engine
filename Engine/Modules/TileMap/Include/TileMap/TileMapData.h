#pragma once

#include "TileMap/TileMapLayerData.h"

namespace GameEngine
{
namespace TileMap
{

/**
 * @brief In-memory model of a tilemap: an ordered stack of layers plus the
 * cell-to-world scale. Layer ids are stable across reorder/rename (assigned
 * from a persisted counter, never reused).
 */
class TileMapData
{
public:
    float32 CellWidth = 1.0f;
    float32 CellHeight = 1.0f;

    /**
     * Adds a layer and returns its stable id (layer storage reallocates on
     * add, so an id — not a reference — is the safe handle; resolve it with
     * FindLayer when needed).
     */
    uint32 AddLayer(const std::string& name, TileMapLayerType type);
    bool RemoveLayer(uint32 layerId);

    /**
     * Undo only: puts a removed layer back where it was, KEEPING ITS ORIGINAL
     * ID. Fails when that id is already present, or when `index` is past the
     * end.
     *
     * Preserving the id is the whole point. Ids are minted from a monotonic
     * counter and never reused, so a layer restored under a fresh id would
     * leave every already-recorded edit on the old id pointing at nothing: those
     * commands resolve no layer and silently no-op, and undo appears to stop
     * working while the stack keeps counting. Restoring the id keeps the
     * commands below this one on the stack valid.
     */
    bool InsertLayer(size_t index, TileMapLayerData layer);

    /**
     * Copies a layer -- cells, tileset, IntGrid values and rules -- in directly
     * above the original, under a fresh id. Returns the new id, or 0 when
     * `layerId` names no layer. Nothing in the scene references the copy yet:
     * a layer draws through an entity's TileMapLayerRef, so the caller supplies
     * one if the duplicate is meant to be visible.
     */
    uint32 DuplicateLayer(uint32 layerId, const std::string& name);

    TileMapLayerData* FindLayer(uint32 layerId);
    const TileMapLayerData* FindLayer(uint32 layerId) const;

    std::vector<TileMapLayerData>& Layers() { return m_Layers; }
    const std::vector<TileMapLayerData>& Layers() const { return m_Layers; }

    uint32 NextLayerId() const { return m_NextLayerId; }
    /** Serialization only: restores the persisted id counter (never lower than 1 + max existing id). */
    void RestoreNextLayerId(uint32 nextLayerId);

    void Clear();

private:
    std::vector<TileMapLayerData> m_Layers;
    uint32 m_NextLayerId = 1;
};

} // namespace TileMap
} // namespace GameEngine
