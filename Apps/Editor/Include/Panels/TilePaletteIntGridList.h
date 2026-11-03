#pragma once

#include "AssetCore/Types.h"
#include "UI/UIElement.h"

#include <functional>
#include <vector>

namespace GameEngine
{

namespace TileMap
{
struct IntGridValueDef;
}

class Button;

/**
 * @brief The palette's IntGrid chooser: one labelled, colour-swatched chip per
 * value the target IntGrid layer defines, plus the erase chip that writes 0.
 *
 * A view over the layer's value table — the panel hands it that table whenever
 * the target layer changes and reads back which value the next stroke writes. It
 * resolves nothing and owns no map.
 */
class TilePaletteIntGridList : public UIElement
{
public:
    TilePaletteIntGridList();

    /** Rebuilds the chips. Any previous selection is dropped with the old chips. */
    void SetValues(const std::vector<TileMap::IntGridValueDef>& values);

    /** Highlights the chip for `value`; anything unknown clears the highlight. */
    void SetSelection(uint8 value);

    void SetOnValueChosen(std::function<void(uint8 value)> callback);

    bool HasValues() const { return m_Chips.size() > 1; }

private:
    struct Chip
    {
        uint8 Value = 0;
        Button* Control = nullptr;
    };

    std::vector<Chip> m_Chips;
    // Same reason as the terrain list: an unconditional rebuild collapses the
    // section for a frame, which reads as the panel jumping on a layer switch.
    std::string m_AppliedSignature;
    std::function<void(uint8)> m_OnValueChosen;
};

} // namespace GameEngine
