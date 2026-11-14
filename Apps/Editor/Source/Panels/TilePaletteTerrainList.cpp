#include "Panels/TilePaletteTerrainList.h"

#include "TileMap/TerrainPeering.h"
#include "TileMap/TileSetAsset.h"
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

std::string MatchModeName(TileMap::TerrainMatchMode mode)
{
    switch (mode)
    {
    case TileMap::TerrainMatchMode::Corners: return "corners";
    case TileMap::TerrainMatchMode::Sides: return "sides";
    case TileMap::TerrainMatchMode::CornersAndSides: return "corners + sides";
    }
    return {};
}

/**
 * @brief Fixed-size colour chip for one terrain.
 *
 * The terrain's colour is authored data, not a theme token, so it cannot come
 * from CSS; the swatch paints it directly and CSS only gives it a box.
 */
class TerrainSwatch : public UIElement
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

std::unique_ptr<UIElement> MakeLabelRow(const std::string& cssClass, const std::string& text)
{
    auto label = std::make_unique<Label>();
    label->AddClass(cssClass);
    label->SetText(text);
    return label;
}

} // namespace

TilePaletteTerrainList::TilePaletteTerrainList()
{
    AddClass("tp-terrain-list");
}

void TilePaletteTerrainList::SetOnTerrainChosen(std::function<void(int16, int16)> callback)
{
    m_OnTerrainChosen = std::move(callback);
}

void TilePaletteTerrainList::SetTerrainSets(const std::vector<TileMap::TerrainSetDef>& sets)
{
    std::string signature;
    for (const TileMap::TerrainSetDef& set : sets)
    {
        signature += set.Name;
        signature += '\x1f';
        for (const TileMap::TerrainDef& terrain : set.Terrains)
        {
            signature += terrain.Name;
            signature += '\x1e';
        }
        signature += '\x1d';
    }
    if (signature == m_AppliedSignature && !m_Chips.empty() == !sets.empty())
        return;
    m_AppliedSignature = signature;

    m_Chips.clear();
    RemoveAllChildren();

    if (sets.empty())
    {
        AddChild(MakeLabelRow("tp-empty",
                              "This tileset defines no terrains. Add one in the Tile Set Editor."));
        return;
    }

    for (size_t setIndex = 0; setIndex < sets.size(); ++setIndex)
    {
        const TileMap::TerrainSetDef& set = sets[setIndex];
        const std::string setName = set.Name.empty() ? ("Terrain Set " + std::to_string(setIndex))
                                                     : set.Name;
        AddChild(MakeLabelRow("tp-terrain-set-label",
                              setName + "  (" + MatchModeName(set.MatchMode) + ")"));

        auto row = std::make_unique<UIElement>();
        row->AddClass("tp-terrain-row");

        // The erase chip is a terrain choice, not a separate tool: clearing a
        // cell has to re-resolve its neighbours exactly as painting one does.
        auto appendChip = [this, &row, setIndex](int16 terrain, const std::string& name,
                                                 uint32 colorRGBA, const std::string& tooltip) {
            auto entry = std::make_unique<UIElement>();
            entry->AddClass("tp-terrain-entry");

            auto swatch = std::make_unique<TerrainSwatch>();
            swatch->AddClass("tp-terrain-swatch");
            swatch->SetColorRGBA(colorRGBA);
            entry->AddChild(std::move(swatch));

            auto button = std::make_unique<Button>();
            button->AddClass("small");
            button->AddClass("secondary");
            button->AddClass("tp-terrain-chip");
            button->SetText(name);
            button->SetTooltip(tooltip);
            const int16 terrainSet = static_cast<int16>(setIndex);
            button->RegisterEventHandler(kEventButtonClick, [this, terrainSet, terrain](UIEvent&) {
                if (m_OnTerrainChosen)
                    m_OnTerrainChosen(terrainSet, terrain);
            });
            m_Chips.push_back(Chip{terrainSet, terrain, button.get()});
            entry->AddChild(std::move(button));

            row->AddChild(std::move(entry));
        };

        appendChip(-1, "Erase", 0x33383CFFu, "Clear cells and repair their neighbors");
        for (size_t terrainIndex = 0; terrainIndex < set.Terrains.size(); ++terrainIndex)
        {
            const TileMap::TerrainDef& terrain = set.Terrains[terrainIndex];
            const std::string name = terrain.Name.empty() ? ("Terrain " + std::to_string(terrainIndex))
                                                          : terrain.Name;
            appendChip(static_cast<int16>(terrainIndex), name, terrain.ColorRGBA,
                       "Paint " + name + " and re-resolve the cells it touches");
        }

        AddChild(std::move(row));
    }
}

void TilePaletteTerrainList::SetSelection(int16 terrainSet, int16 terrain)
{
    for (const Chip& chip : m_Chips)
    {
        if (chip.Control == nullptr)
            continue;
        const bool selected = chip.TerrainSet == terrainSet && chip.Terrain == terrain;
        if (selected == chip.Control->HasClass("selected"))
            continue;
        if (selected)
            chip.Control->AddClass("selected");
        else
            chip.Control->RemoveClass("selected");
    }
}

} // namespace GameEngine
