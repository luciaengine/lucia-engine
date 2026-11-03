#pragma once

#include "AssetCore/GUID.h"
#include "AssetCore/Types.h"
#include "UI/UIElement.h"

#include <functional>
#include <vector>

namespace GameEngine
{

namespace TileMap
{
struct TileBrushDef;
struct TileUVRect;
}

class Button;

/**
 * @brief The palette's saved-brush chooser: one chip per brush the tileset
 * stores. A click arms the brush as the stamp; a right-click asks the panel
 * for the brush's menu.
 *
 * A view over the tileset's brush table — the panel hands it that table
 * whenever the tileset reloads or a brush is saved, and it reports back which
 * brush was chosen. It holds no tileset and resolves no assets.
 */
class TilePaletteBrushList : public UIElement
{
public:
    TilePaletteBrushList();

    /**
     * The atlas the icons draw from: its texture and one UV rect per tile id
     * (index = id - 1). Set before SetBrushes; an empty table draws no icons.
     */
    void SetAtlas(const GUID& atlasTexture, std::vector<TileMap::TileUVRect> tileUVs);

    /** Rebuilds the chips. Any previous highlight is dropped with the old chips. */
    void SetBrushes(const std::vector<TileMap::TileBrushDef>& brushes);

    /** Highlights the chip at `index`; anything out of range clears the highlight. */
    void SetSelection(size_t index);

    void SetOnBrushChosen(std::function<void(size_t index)> callback);
    /** Right-click on a chip, with the pointer position in window space. */
    void SetOnBrushMenuRequested(std::function<void(size_t index, int x, int y)> callback);

    bool HasBrushes() const { return !m_Chips.empty(); }

private:
    std::vector<Button*> m_Chips;
    GUID m_AtlasTexture;
    std::vector<TileMap::TileUVRect> m_TileUVs;
    // Same reason as the terrain list: an unconditional rebuild collapses the
    // section for a frame, which reads as the panel jumping on a layer switch.
    std::string m_AppliedSignature;
    std::function<void(size_t)> m_OnBrushChosen;
    std::function<void(size_t, int, int)> m_OnBrushMenuRequested;
};

} // namespace GameEngine
