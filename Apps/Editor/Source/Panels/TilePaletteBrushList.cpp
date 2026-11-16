#include "Panels/TilePaletteBrushList.h"

#include "TileMap/TileSetAsset.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/Label.h"
#include "TileMap/TileCellEncoding.h"
#include "UI/UIEvents.h"
#include "UI/UIManager.h"
#include "UI/UIPrimitive.h"
#include "UI/UITextureRegistry.h"

#include <algorithm>

#include <memory>
#include <string>
#include <utility>

namespace GameEngine
{
namespace
{
// Pointer buttons as delivered in UIEvent::Button.
constexpr int32 kMouseButtonRight = 1;
const uint32 kIconTint = UI::PackColorU8(0xFF, 0xFF, 0xFF, 0xFF);

/**
 * @brief The brush's footprint drawn from the atlas, fitted into the chip's
 * icon box. Cells keep their aspect; a wide brush leaves the box's top and
 * bottom empty rather than squashing.
 */
class TileBrushIcon : public UIElement
{
public:
    TileBrushIcon() { AddClass("tp-brush-icon"); }

    void SetBrush(const GUID& atlasTexture, const std::vector<TileMap::TileUVRect>* tileUVs,
                  const TileMap::TileBrushDef& brush)
    {
        m_AtlasTexture = atlasTexture;
        m_TileUVs = tileUVs;
        m_Width = brush.Width;
        m_Height = brush.Height;
        m_Cells = brush.Cells;
        MarkDirty(VisualDirty);
    }

    void OnGeneratePrimitives(UI::PrimitiveEmitContext& ctx, const ResolvedStyle&, float x, float y,
                              float w, float h) override
    {
        // Same parallel-drain contract as the atlas grid: textured emission
        // touches the manager's texture registry, so it stays on the UI thread.
        if (ctx.OffThread)
        {
            if (ctx.EscalateFlag)
                *ctx.EscalateFlag = true;
            return;
        }
        if (w <= 0.0f || h <= 0.0f || m_Width <= 0 || m_Height <= 0 || m_TileUVs == nullptr ||
            ctx.Manager == nullptr || ctx.Textures == nullptr || m_AtlasTexture.IsNull())
            return;
        const Rendering::TextureHandle texture =
            ctx.Manager->BackgroundImages().TryGetHandle(m_AtlasTexture);
        if (!texture.IsValid())
            return;
        const uint32 slot = ctx.Textures->Register(texture, ctx.Manager->GetNearestClampSampler());
        if (slot == 0)
            return;

        const float cell = std::min(w / static_cast<float>(m_Width), h / static_cast<float>(m_Height));
        const float originX = x + (w - cell * static_cast<float>(m_Width)) * 0.5f;
        const float originY = y + (h - cell * static_cast<float>(m_Height)) * 0.5f;
        for (int32 row = 0; row < m_Height; ++row)
            for (int32 column = 0; column < m_Width; ++column)
            {
                const size_t index = static_cast<size_t>(row) * static_cast<size_t>(m_Width) +
                                     static_cast<size_t>(column);
                if (index >= m_Cells.size())
                    continue;
                const TileMap::TileCell packed = m_Cells[index];
                const uint32 tileId = TileMap::CellTileId(packed);
                if (tileId == 0 || tileId > m_TileUVs->size())
                    continue;
                const TileMap::TileUVRect& uv = (*m_TileUVs)[tileId - 1];
                float u0 = uv.U0, u1 = uv.U1, v0 = uv.V0, v1 = uv.V1;
                if (TileMap::CellFlipH(packed))
                    std::swap(u0, u1);
                if (TileMap::CellFlipV(packed))
                    std::swap(v0, v1);
                UI::UIPrimitive quad = UI::MakeTexturedQuad(
                    originX + cell * static_cast<float>(column), originY + cell * static_cast<float>(row),
                    cell, cell, slot, kIconTint, u0, v0, u1, v1, ctx.ClipIndex);
                quad.ModeAndFlags |= UI::kPrimPointFilterBit;
                ctx.Emit(quad);
            }
    }

private:
    GUID m_AtlasTexture;
    const std::vector<TileMap::TileUVRect>* m_TileUVs = nullptr;
    int32 m_Width = 0;
    int32 m_Height = 0;
    std::vector<uint32> m_Cells;
};
} // namespace

TilePaletteBrushList::TilePaletteBrushList()
{
    AddClass("tp-terrain-list");
}

void TilePaletteBrushList::SetOnBrushChosen(std::function<void(size_t)> callback)
{
    m_OnBrushChosen = std::move(callback);
}

void TilePaletteBrushList::SetOnBrushMenuRequested(std::function<void(size_t, int, int)> callback)
{
    m_OnBrushMenuRequested = std::move(callback);
}

void TilePaletteBrushList::SetAtlas(const GUID& atlasTexture, std::vector<TileMap::TileUVRect> tileUVs)
{
    m_AtlasTexture = atlasTexture;
    m_TileUVs = std::move(tileUVs);
    // The icons hold a pointer into the table, so a new table means new chips.
    m_AppliedSignature.clear();
}

void TilePaletteBrushList::SetBrushes(const std::vector<TileMap::TileBrushDef>& brushes)
{
    std::string signature = m_AtlasTexture.ToString();
    for (const TileMap::TileBrushDef& brush : brushes)
    {
        signature += brush.Name;
        signature += '\x1f';
        signature += std::to_string(brush.Width) + 'x' + std::to_string(brush.Height);
        signature += '\x1e';
    }
    if (signature == m_AppliedSignature && !m_Chips.empty() == !brushes.empty())
        return;
    m_AppliedSignature = signature;

    m_Chips.clear();
    RemoveAllChildren();

    if (brushes.empty())
    {
        auto label = std::make_unique<Label>();
        label->AddClass("tp-empty");
        label->SetText("No saved brushes. Pick tiles in the atlas and press Save Brush.");
        AddChild(std::move(label));
        return;
    }

    auto row = std::make_unique<UIElement>();
    row->AddClass("tp-terrain-row");
    for (size_t index = 0; index < brushes.size(); ++index)
    {
        const TileMap::TileBrushDef& brush = brushes[index];
        const std::string name = brush.Name.empty() ? ("Brush " + std::to_string(index + 1))
                                                    : brush.Name;
        auto entry = std::make_unique<UIElement>();
        entry->AddClass("tp-terrain-entry");
        auto icon = std::make_unique<TileBrushIcon>();
        icon->SetBrush(m_AtlasTexture, &m_TileUVs, brush);
        icon->RegisterEventHandler(kEventMouseDown, [this, index](UIEvent& e) {
            if (e.Button == kMouseButtonRight)
            {
                if (m_OnBrushMenuRequested)
                    m_OnBrushMenuRequested(index, static_cast<int>(e.X), static_cast<int>(e.Y));
            }
            else if (m_OnBrushChosen)
            {
                m_OnBrushChosen(index);
            }
            e.Stop();
        });
        entry->AddChild(std::move(icon));
        auto button = std::make_unique<Button>();
        button->AddClass("small");
        button->AddClass("secondary");
        button->AddClass("tp-terrain-chip");
        button->SetText(name);
        button->SetTooltip("Stamp " + name + " (" + std::to_string(brush.Width) + "x" +
                           std::to_string(brush.Height) + "); right-click for its menu");
        button->RegisterEventHandler(kEventButtonClick, [this, index](UIEvent&) {
            if (m_OnBrushChosen)
                m_OnBrushChosen(index);
        });
        button->RegisterEventHandler(kEventMouseDown, [this, index](UIEvent& e) {
            if (e.Button != kMouseButtonRight)
                return;
            if (m_OnBrushMenuRequested)
                m_OnBrushMenuRequested(index, static_cast<int>(e.X), static_cast<int>(e.Y));
            e.Stop();
        });
        m_Chips.push_back(button.get());
        entry->AddChild(std::move(button));
        row->AddChild(std::move(entry));
    }
    AddChild(std::move(row));
    // Swapping the empty-state label for a chip row changes this list's height
    // while it is on screen; the section above has to be re-measured or the
    // chips draw clipped under the atlas.
    MarkDirty(StyleDirty | LayoutDirty);
}

void TilePaletteBrushList::SetSelection(size_t index)
{
    for (size_t i = 0; i < m_Chips.size(); ++i)
    {
        Button* chip = m_Chips[i];
        if (chip == nullptr)
            continue;
        const bool selected = i == index;
        if (selected == chip->HasClass("selected"))
            continue;
        if (selected)
            chip->AddClass("selected");
        else
            chip->RemoveClass("selected");
    }
}

} // namespace GameEngine
