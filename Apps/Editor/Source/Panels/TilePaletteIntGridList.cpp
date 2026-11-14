#include "Panels/TilePaletteIntGridList.h"

#include "TileMap/TileMapLayerData.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/Label.h"
#include "UI/UIEvents.h"
#include "UI/UIPrimitive.h"

#include <memory>
#include <string>
#include <utility>

namespace GameEngine
{
namespace
{

constexpr float kSwatchCornerRadiusPx = 2.0f;

/**
 * @brief Fixed-size colour chip for one IntGrid value.
 *
 * The value's colour is authored data, not a theme token, so it cannot come from
 * CSS; the swatch paints it directly and CSS only gives it a box.
 */
class IntGridSwatch : public UIElement
{
public:
    void SetColorRGBA(uint32 colorRGBA)
    {
        // UIPrimitive packs colour as r | g<<8 | b<<16 | a<<24, so a hex literal
        // reads 0xAABBGGRR — the authored 0xRRGGBBAA has to be taken apart.
        m_Packed = UI::PackColorU8(static_cast<uint8>((colorRGBA >> 24) & 0xFFu),
                                   static_cast<uint8>((colorRGBA >> 16) & 0xFFu),
                                   static_cast<uint8>((colorRGBA >> 8) & 0xFFu),
                                   static_cast<uint8>(colorRGBA & 0xFFu));
        MarkDirty(VisualDirty);
    }

    void OnGeneratePrimitives(UI::PrimitiveEmitContext& ctx, const ResolvedStyle&, float x, float y,
                              float w, float h) override
    {
        if (w <= 0.0f || h <= 0.0f)
            return;
        const float radius = kSwatchCornerRadiusPx * ctx.ContentScale;
        ctx.Emit(UI::MakeRect(x, y, w, h, m_Packed, radius, radius, radius, radius));
    }

private:
    uint32 m_Packed = UI::PackColorU8(0xFF, 0xFF, 0xFF, 0xFF);
};

} // namespace

TilePaletteIntGridList::TilePaletteIntGridList()
{
    AddClass("tp-terrain-list");
}

void TilePaletteIntGridList::SetOnValueChosen(std::function<void(uint8)> callback)
{
    m_OnValueChosen = std::move(callback);
}

void TilePaletteIntGridList::SetValues(const std::vector<TileMap::IntGridValueDef>& values)
{
    std::string signature;
    for (const TileMap::IntGridValueDef& def : values)
    {
        signature += std::to_string(def.Value);
        signature += ':';
        signature += def.Name;
        signature += ':';
        signature += std::to_string(def.ColorRGBA);
        signature += '\x1f';
    }
    if (signature == m_AppliedSignature && !m_Chips.empty())
        return;
    m_AppliedSignature = signature;

    m_Chips.clear();
    RemoveAllChildren();

    auto row = std::make_unique<UIElement>();
    row->AddClass("tp-terrain-row");

    auto appendChip = [this, &row](uint8 value, const std::string& name, uint32 colorRGBA,
                                   const std::string& tooltip)
    {
        auto entry = std::make_unique<UIElement>();
        entry->AddClass("tp-terrain-entry");

        auto swatch = std::make_unique<IntGridSwatch>();
        swatch->AddClass("tp-terrain-swatch");
        swatch->SetColorRGBA(colorRGBA);
        entry->AddChild(std::move(swatch));

        auto button = std::make_unique<Button>();
        button->AddClass("small");
        button->AddClass("secondary");
        button->AddClass("tp-terrain-chip");
        button->SetText(name);
        button->SetTooltip(tooltip);
        button->RegisterEventHandler(kEventButtonClick, [this, value](UIEvent&)
                           {
                               if (m_OnValueChosen)
                                   m_OnValueChosen(value);
                           });
        m_Chips.push_back(Chip{value, button.get()});
        entry->AddChild(std::move(button));

        row->AddChild(std::move(entry));
    };

    // Value 0 is the eraser of semantic painting, so it is a chip like any other.
    appendChip(0, "Erase", 0x33383CFFu, "Clear the value and update the generated tiles");
    for (const TileMap::IntGridValueDef& def : values)
    {
        const std::string name =
            def.Name.empty() ? ("Value " + std::to_string(def.Value)) : def.Name;
        appendChip(def.Value, name, def.ColorRGBA,
                   "Paint " + name + " (" + std::to_string(def.Value) + ")");
    }

    AddChild(std::move(row));

    if (values.empty())
    {
        auto hint = std::make_unique<Label>();
        hint->AddClass("tp-empty");
        hint->SetText("This layer defines no IntGrid values. Add them in the Tile Rules panel.");
        AddChild(std::move(hint));
    }
}

void TilePaletteIntGridList::SetSelection(uint8 value)
{
    for (const Chip& chip : m_Chips)
    {
        if (chip.Control == nullptr)
            continue;
        const bool selected = chip.Value == value;
        if (selected == chip.Control->HasClass("selected"))
            continue;
        if (selected)
            chip.Control->AddClass("selected");
        else
            chip.Control->RemoveClass("selected");
    }
}

} // namespace GameEngine
