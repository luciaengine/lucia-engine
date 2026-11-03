#pragma once

#include "AssetCore/Types.h"
#include "UI/UIElement.h"

#include <functional>
#include <vector>

namespace GameEngine
{

namespace TileMap
{
struct TerrainSetDef;
}

class Button;

/**
 * @brief The palette's terrain chooser: one labelled, colour-swatched chip per
 * terrain of every terrain set the tileset defines, plus the erase chip that
 * clears cells and repairs their neighbours.
 *
 * It is a view over the tileset's terrain-set table — the panel hands it that
 * table whenever the tileset reloads, and it reports back which terrain the
 * next stroke should paint. It holds no tileset and resolves no assets.
 */
class TilePaletteTerrainList : public UIElement
{
public:
    TilePaletteTerrainList();

    /** Rebuilds the chips. Any previous selection is dropped with the old chips. */
    void SetTerrainSets(const std::vector<TileMap::TerrainSetDef>& sets);

    /** Highlights the chip for (terrainSet, terrain); anything unknown clears the highlight. */
    void SetSelection(int16 terrainSet, int16 terrain);

    void SetOnTerrainChosen(std::function<void(int16 terrainSet, int16 terrain)> callback);

    bool HasTerrains() const { return !m_Chips.empty(); }

private:
    struct Chip
    {
        int16 TerrainSet = -1;
        int16 Terrain = -1;
        Button* Control = nullptr;
    };

    std::vector<Chip> m_Chips;
    // What the displayed chips were built from. Rebuilding tears every child
    // out and back in, which collapses the section for a frame; switching
    // between layers whose tilesets carry the same terrains must not do that.
    std::string m_AppliedSignature;
    std::function<void(int16, int16)> m_OnTerrainChosen;
};

} // namespace GameEngine
