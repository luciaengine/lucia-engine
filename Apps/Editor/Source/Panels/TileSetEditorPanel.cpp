#include "Panels/TileSetEditorPanel.h"

#include "EditorContext.h"
#include "Panels/ConfirmActionModal.h"
#include "Panels/TileSetCollisionEditor.h"
#include "Panels/TileSetTerrainBitEditor.h"
#include "Inspectors/InspectorDragHelpers.h"
#include "UI/Controls/FloatField.h"
#include "UI/AssetField.h"

#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Assets/TextureAsset.h"
#include "AssetCore/Asset.h"
#include "AssetCore/AssetTypes.h"
#include "Types/Types.h"
#include "Logger/Logger.h"
#include "TileMap/TileSetAsset.h"
#include "TileMap/TileSetAtlasLookup.h"
#include "TileMap/TerrainPeering.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/Dropdown.h"
#include "UI/Controls/IntField.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/ScrollView.h"
#include "UI/UIEvents.h"
#include "UI/Controls/TextField.h"
#include "UI/UIElement.h"
#include "UI/UIManager.h"
#include "UI/UIPrimitive.h"
#include "UI/UITextureRegistry.h"

#include <algorithm>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace GameEngine
{

namespace
{

// Grid defaults and limits. 16px is the de-facto tile size for 2D pixel-art
// atlases, so a fresh tileset slices sensibly with no further input.
constexpr int32_t kDefaultTileSizePx = 16;
constexpr int32_t kMinTileSizePx = 1;
constexpr int32_t kMaxTileSizePx = 4096;
constexpr int32_t kMinOffsetPx = 0;
constexpr int32_t kMaxOffsetPx = 4096;

constexpr const char* kNoTileSelectedText = "No tile selected.";

// Collision-mode dropdown option values. The Dropdown carries strings, so the
// two TileCollisionMode enumerators are named here rather than spelled inline.
constexpr const char* kCollisionModeSolidValue = "0";
constexpr const char* kCollisionModeOneWayValue = "1";
constexpr int32_t kCollisionModeSolidIndex = 0;
constexpr int32_t kCollisionModeOneWayIndex = 1;

// Authored as 0xRRGGBBAA; handed out in order by DefaultTerrainColor.
constexpr uint32_t kTerrainPalette[] = {0x4CAF50FFu, 0x8D6E63FFu, 0x42A5F5FFu, 0xFFCA28FFu,
                                        0xAB47BCFFu, 0xEF5350FFu, 0x26A69AFFu, 0xEC407AFFu};
constexpr size_t kTerrainPaletteCount = sizeof(kTerrainPalette) / sizeof(kTerrainPalette[0]);

const std::vector<std::string> kMatchModeLabels{"Corners + Sides", "Corners", "Sides"};
constexpr const char* kStyleAssetPath = "UI/theme/tileset-editor.css";

// Preview canvas geometry, in CSS logical pixels (scaled by ctx.ContentScale).
constexpr float kPreviewPaddingPx = 8.0f;
constexpr float kGridLineThicknessPx = 1.0f;
constexpr float kPreviewCornerRadiusPx = 3.0f;
constexpr float kAtlasBorderWidthPx = 1.0f;
// Below this on-screen tile size the overlay is denser than it is readable, so
// the grid is dropped and only the atlas plus its outline are drawn.
constexpr float kMinGridTileScreenPx = 3.0f;

const uint32_t kPreviewBackground = UI::PackColor(0.07f, 0.07f, 0.08f, 1.0f);
const uint32_t kAtlasBackground = UI::PackColor(0.14f, 0.15f, 0.17f, 1.0f);
const uint32_t kAtlasOutline = UI::PackColor(0.30f, 0.33f, 0.36f, 1.0f);
const uint32_t kGridLine = UI::PackColor(0.41f, 0.66f, 1.0f, 0.85f);

std::unique_ptr<Label> MakeLabel(const std::string& cssClass, const std::string& text)
{
    auto label = std::make_unique<Label>();
    label->AddClass(cssClass);
    label->SetText(text);
    return label;
}

/** A labelled action button. An empty glyph class leaves it text-only. */
std::unique_ptr<Button> MakeActionButton(const std::string& id, const std::string& text,
                                         const std::string& glyphClass)
{
    auto button = std::make_unique<Button>();
    button->SetId(id);
    button->SetText(text);
    button->AddClass("small");
    if (!glyphClass.empty())
    {
        button->AddClass("tm-icon-label");
        button->AddClass(glyphClass);
    }
    return button;
}

/** A glyph-only button. The tooltip is the only place its name appears. */
std::unique_ptr<Button> MakeIconButton(const std::string& id, const std::string& glyphClass)
{
    auto button = std::make_unique<Button>();
    button->SetId(id);
    button->AddClass("small");
    button->AddClass("tm-icon");
    button->AddClass(glyphClass);
    return button;
}

void RegisterTileSetPath(AssetManager* assets, const std::filesystem::path& path)
{
    if (!assets || path.empty())
        return;
    try
    {
        (void)assets->GetRegistry().RegisterAsset(path);
    }
    catch (...)
    {
        Logger::Log::Error("TileSetEditorPanel: failed to register '{}'", path.string());
    }
}

/** Screen-space rect the atlas occupies inside `area`, preserving aspect ratio. */
struct FittedRect
{
    float X = 0.0f;
    float Y = 0.0f;
    float W = 0.0f;
    float H = 0.0f;
    float Scale = 0.0f; // screen px per atlas px
};

FittedRect FitAtlas(float areaX, float areaY, float areaW, float areaH,
                    int32_t atlasWidthPx, int32_t atlasHeightPx)
{
    FittedRect fit;
    if (areaW <= 0.0f || areaH <= 0.0f || atlasWidthPx <= 0 || atlasHeightPx <= 0)
        return fit;

    const float aw = static_cast<float>(atlasWidthPx);
    const float ah = static_cast<float>(atlasHeightPx);
    fit.Scale = std::min(areaW / aw, areaH / ah);
    fit.W = aw * fit.Scale;
    fit.H = ah * fit.Scale;
    fit.X = areaX + (areaW - fit.W) * 0.5f;
    fit.Y = areaY + (areaH - fit.H) * 0.5f;
    return fit;
}

} // namespace

// ---------------------------------------------------------------------------
// TileSetAtlasPreview
// ---------------------------------------------------------------------------

void TileSetAtlasPreview::SetAtlas(const GUID& textureGuid)
{
    if (m_TextureGuid == textureGuid)
        return;
    m_TextureGuid = textureGuid;
    MarkDirty(VisualDirty);
}

void TileSetAtlasPreview::SetGrid(int32_t tileWidthPx, int32_t tileHeightPx,
                                  int32_t marginPx, int32_t spacingPx)
{
    if (m_TileWidthPx == tileWidthPx && m_TileHeightPx == tileHeightPx &&
        m_MarginPx == marginPx && m_SpacingPx == spacingPx)
        return;
    m_TileWidthPx = tileWidthPx;
    m_TileHeightPx = tileHeightPx;
    m_MarginPx = marginPx;
    m_SpacingPx = spacingPx;
    MarkDirty(VisualDirty);
}

void TileSetAtlasPreview::OnGeneratePrimitives(UI::PrimitiveEmitContext& ctx, const ResolvedStyle&,
                                               float x, float y, float w, float h)
{
    // Parallel-drain thread contract: texture resolution touches shared
    // UIManager cache state, so this never runs on a JobSystem worker.
    if (ctx.OffThread)
    {
        if (ctx.EscalateFlag)
            *ctx.EscalateFlag = true;
        return;
    }

    if (w <= 1.0f || h <= 1.0f)
        return;

    const float cs = ctx.ContentScale;
    const float radius = kPreviewCornerRadiusPx * cs;
    ctx.Emit(UI::MakeRect(x, y, w, h, kPreviewBackground, radius, radius, radius, radius));

    if (!ctx.Manager || !ctx.Textures || m_TextureGuid.IsNull())
        return;

    uint32_t atlasWidthPx = 0;
    uint32_t atlasHeightPx = 0;
    const Rendering::TextureHandle texture =
        ctx.Manager->TryGetBackgroundTextureHandleByGuid(m_TextureGuid, &atlasWidthPx, &atlasHeightPx);
    if (!texture.IsValid() || atlasWidthPx == 0 || atlasHeightPx == 0)
        return;

    // Pixel art must not blur: the nearest-clamp sampler is keyed alongside the
    // texture handle, so the same image can also be drawn filtered elsewhere.
    const uint32_t slot = ctx.Textures->Register(texture, ctx.Manager->GetNearestClampSampler());
    if (slot == 0)
        return;

    const float pad = kPreviewPaddingPx * cs;
    const FittedRect fit = FitAtlas(x + pad, y + pad, w - 2.0f * pad, h - 2.0f * pad,
                                    static_cast<int32_t>(atlasWidthPx),
                                    static_cast<int32_t>(atlasHeightPx));
    if (fit.W <= 0.0f || fit.H <= 0.0f)
        return;

    UI::UIPrimitive backdrop = UI::MakeRect(fit.X, fit.Y, fit.W, fit.H, kAtlasBackground);
    UI::AddBorder(backdrop, kAtlasBorderWidthPx * cs, kAtlasOutline);
    ctx.Emit(backdrop);
    ctx.Emit(UI::MakeTexturedQuad(fit.X, fit.Y, fit.W, fit.H, slot));

    if (m_TileWidthPx < kMinTileSizePx || m_TileHeightPx < kMinTileSizePx ||
        m_MarginPx < 0 || m_SpacingPx < 0)
        return;

    const int32_t columns = TileMap::TileSetAsset::GridColumns(
        static_cast<int32_t>(atlasWidthPx), m_TileWidthPx, m_MarginPx, m_SpacingPx);
    const int32_t rows = TileMap::TileSetAsset::GridRows(
        static_cast<int32_t>(atlasHeightPx), m_TileHeightPx, m_MarginPx, m_SpacingPx);
    if (columns <= 0 || rows <= 0)
        return;

    const float tileScreenW = static_cast<float>(m_TileWidthPx) * fit.Scale;
    const float tileScreenH = static_cast<float>(m_TileHeightPx) * fit.Scale;
    if (tileScreenW < kMinGridTileScreenPx || tileScreenH < kMinGridTileScreenPx)
        return;

    // Both edges of every cell are drawn so the spacing gutters stay visible;
    // with spacing 0 the two coincide and read as a single shared line.
    const float thickness = kGridLineThicknessPx * cs;
    const float strideX = static_cast<float>(m_TileWidthPx + m_SpacingPx) * fit.Scale;
    const float strideY = static_cast<float>(m_TileHeightPx + m_SpacingPx) * fit.Scale;
    const float originX = fit.X + static_cast<float>(m_MarginPx) * fit.Scale;
    const float originY = fit.Y + static_cast<float>(m_MarginPx) * fit.Scale;

    for (int32_t column = 0; column < columns; ++column)
    {
        const float left = originX + static_cast<float>(column) * strideX;
        ctx.Emit(UI::MakeRect(left, fit.Y, thickness, fit.H, kGridLine));
        ctx.Emit(UI::MakeRect(left + tileScreenW - thickness, fit.Y, thickness, fit.H, kGridLine));
    }
    for (int32_t row = 0; row < rows; ++row)
    {
        const float top = originY + static_cast<float>(row) * strideY;
        ctx.Emit(UI::MakeRect(fit.X, top, fit.W, thickness, kGridLine));
        ctx.Emit(UI::MakeRect(fit.X, top + tileScreenH - thickness, fit.W, thickness, kGridLine));
    }
}

// ---------------------------------------------------------------------------
// TileSetEditorPanel
// ---------------------------------------------------------------------------

TileSetEditorPanel::TileSetEditorPanel()
    : DockPanel("Tile Set Editor")
    , m_AsyncLifetime(std::make_shared<int>(0))
{
    AddClass("tileset-editor-panel");
    // The panel carries its own scoped styles; only the row scaffolding it
    // borrows from the inspector helpers resolves from the global theme.
    RequestSubtreeStyleAssetPath(kStyleAssetPath);
    RequestSubtreeStyleAssetPath("UI/theme/tilemap-icons.css");
    BuildLayout();
}

namespace
{
// A modal parented to the panel itself already goes down with it; only the
// root-parented case needs detaching, and reaching back into a half-destroyed
// owner would be the worse bug.
void DetachConfirmModal(const UIElement* owner, ConfirmActionModal*& modal)
{
    if (!modal)
        return;
    UIElement* parent = modal->GetParent();
    if (parent && parent != owner)
        parent->RemoveChild(modal);
    modal = nullptr;
}
} // namespace

TileSetEditorPanel::~TileSetEditorPanel()
{
    // The confirmation modals are parented to the UI root, so nothing else takes
    // them down with the panel: left alone one stays on screen, and confirming it
    // would write a file through a freed panel. Detaching is the visible half —
    // m_AsyncLifetime expiring with this object is what makes an already
    // in-flight or deferred callback a no-op.
    DetachConfirmModal(this, m_ReSliceConfirmModal);
    DetachConfirmModal(this, m_DiskConflictModal);
}

void TileSetEditorPanel::BuildLayout()
{
    auto root = std::make_unique<UIElement>();
    root->AddClass("ts-root");

    auto form = std::make_unique<UIElement>();
    form->AddClass("ts-form");
    UIElement* formRaw = form.get();
    root->AddChild(std::move(form));

    m_AtlasField = InspectorUI::AddAssetFieldRow(
        formRaw, "Atlas Texture", GUID::Null(), {AssetType::Texture}, nullptr,
        [this](const GUID& guid) { OnAtlasTextureChanged(guid); }, nullptr,
        "Texture sliced into tiles by this tile set");

    m_TileWidthField = InspectorDrag::AddIntRowWithDrag(
        formRaw, "Tile Width", kDefaultTileSizePx,
        [this](const int&) { OnGridFieldChanged(); },
        [this](const int&) { OnGridFieldChanged(); },
        kDefaultTileSizePx, "Width of one tile, in atlas pixels");
    m_TileWidthField->SetRange(kMinTileSizePx, kMaxTileSizePx);

    m_TileHeightField = InspectorDrag::AddIntRowWithDrag(
        formRaw, "Tile Height", kDefaultTileSizePx,
        [this](const int&) { OnGridFieldChanged(); },
        [this](const int&) { OnGridFieldChanged(); },
        kDefaultTileSizePx, "Height of one tile, in atlas pixels");
    m_TileHeightField->SetRange(kMinTileSizePx, kMaxTileSizePx);

    m_MarginField = InspectorDrag::AddIntRowWithDrag(
        formRaw, "Margin", kMinOffsetPx,
        [this](const int&) { OnGridFieldChanged(); },
        [this](const int&) { OnGridFieldChanged(); },
        kMinOffsetPx, "Border inset around the whole atlas, in pixels");
    m_MarginField->SetRange(kMinOffsetPx, kMaxOffsetPx);

    m_SpacingField = InspectorDrag::AddIntRowWithDrag(
        formRaw, "Spacing", kMinOffsetPx,
        [this](const int&) { OnGridFieldChanged(); },
        [this](const int&) { OnGridFieldChanged(); },
        kMinOffsetPx, "Gutter between neighboring tiles, in pixels");
    m_SpacingField->SetRange(kMinOffsetPx, kMaxOffsetPx);

    auto summary = MakeLabel("ts-summary", "No atlas texture.");
    m_SummaryLabel = summary.get();
    root->AddChild(std::move(summary));

    auto actions = std::make_unique<UIElement>();
    actions->AddClass("ts-actions");
    auto applyButton = MakeActionButton("TileSetApplyButton", "Apply & Save", "tm-glyph-apply");
    applyButton->SetTooltip("Re-slice the tile set with these values and write the .tileset file");
    applyButton->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { (void)ApplyAndSave(); });
    actions->AddChild(std::move(applyButton));
    root->AddChild(std::move(actions));

    auto status = MakeLabel("ts-status", "");
    m_StatusLabel = status.get();
    root->AddChild(std::move(status));

    auto preview = std::make_unique<TileSetAtlasPreview>();
    preview->AddClass("ts-preview");
    m_Preview = preview.get();
    root->AddChild(std::move(preview));

    root->AddChild(BuildCollisionSection());
    root->AddChild(BuildTerrainSection());

    // The panel is usually docked into a short bottom strip, and the terrain
    // canvas needs real estate: scroll rather than clip the authoring surface
    // out of reach.
    auto scroll = std::make_unique<ScrollView>();
    scroll->SetId("TileSetEditorScroll");
    scroll->AddClass("ts-scroll");
    scroll->GetViewport()->AddChild(std::move(root));
    AddChild(std::move(scroll));
    OnGridFieldChanged();
}


std::unique_ptr<UIElement> TileSetEditorPanel::BuildCollisionSection()
{
    auto section = std::make_unique<UIElement>();
    section->AddClass("ts-collision-section");

    auto header = std::make_unique<UIElement>();
    header->AddClass("ts-terrain-header");
    header->AddChild(MakeLabel("ts-section-label", "Collision"));
    section->AddChild(std::move(header));

    auto hint = MakeLabel("ts-terrain-hint",
                          "Left-click selects a tile; click it again to toggle its full-square "
                          "collision. Right-clicks drop custom outline points (three or more take "
                          "effect). The selected tile's mode and material are below.");
    section->AddChild(std::move(hint));

    auto canvas = std::make_unique<TileSetCollisionEditor>();
    canvas->SetOnCollisionChanged([this]() {
        SetStatus("Collision edited — Apply & Save to write it.", false);
    });
    canvas->SetOnSelectionChanged([this]() { ShowSelectedTileCollision(); });
    m_CollisionCanvas = canvas.get();
    section->AddChild(std::move(canvas));

    auto tools = std::make_unique<UIElement>();
    tools->AddClass("ts-collision-tools");
    UIElement* toolsRaw = tools.get();
    section->AddChild(std::move(tools));

    auto selection = MakeLabel("ts-terrain-hint", kNoTileSelectedText);
    m_CollisionSelectionLabel = selection.get();
    toolsRaw->AddChild(std::move(selection));

    m_CollisionModeDropdown = InspectorUI::AddDropdownRow(
        toolsRaw, "Collision Mode",
        {{kCollisionModeSolidValue, "Solid"}, {kCollisionModeOneWayValue, "One Way"}},
        kCollisionModeSolidIndex,
        "Solid blocks from every direction. One Way is solid only to a contact landing on its "
        "top face, so a character can jump up through the tile and land on it coming down.");
    m_CollisionModeDropdown->SetOnValueChanged(
        [this](const std::string& value) { OnSelectedTileCollisionModeChanged(value); });

    m_CollisionMaterialField = InspectorUI::AddAssetFieldRow(
        toolsRaw, "Physics Material", GUID::Null(), {AssetType::PhysicsMaterial}, nullptr,
        [this](const GUID& guid) { OnSelectedTilePhysicsMaterialChanged(guid); }, nullptr,
        "Contact surface (.physmat) this tile's collision is solved with. Unset falls back to the "
        "collider's own friction and restitution.");

    // Animation: the frames the selected tile cycles through at runtime. The
    // walk itself is data in the tileset; this is the only place it is authored.
    auto animationHeader = std::make_unique<UIElement>();
    animationHeader->AddClass("ts-terrain-header");
    animationHeader->AddChild(MakeLabel("ts-section-label", "Animation"));
    auto addFrame = std::make_unique<Button>();
    addFrame->SetId("TileSetAddAnimationFrame");
    addFrame->SetText("Add Frame");
    addFrame->AddClass("small");
    addFrame->AddClass("secondary");
    addFrame->SetTooltip("Append a frame; each frame shows another tile of this set for its duration");
    addFrame->RegisterEventHandler(kEventButtonClick,
                                   [this](UIEvent&) { AddAnimationFrameToSelectedTile(); });
    m_AddFrameButton = addFrame.get();
    animationHeader->AddChild(std::move(addFrame));
    toolsRaw->AddChild(std::move(animationHeader));

    auto animationRows = std::make_unique<UIElement>();
    animationRows->AddClass("ts-collision-tools");
    m_AnimationRows = animationRows.get();
    toolsRaw->AddChild(std::move(animationRows));

    ShowSelectedTileCollision();
    return section;
}

void TileSetEditorPanel::ShowSelectedTileAnimation()
{
    if (!m_AnimationRows)
        return;
    m_AnimationRows->RemoveAllChildren();

    const TileMap::TileSetTile* tile = SelectedCollisionTile();
    if (m_AddFrameButton)
        m_AddFrameButton->SetEnabled(tile != nullptr);
    if (!tile)
        return;
    if (tile->AnimationFrames.empty())
    {
        m_AnimationRows->AddChild(
            MakeLabel("ts-terrain-hint", "Static tile. Add a frame to make it cycle."));
        return;
    }

    const int32_t maxTileId = TileSet() ? static_cast<int32_t>(TileSet()->Tiles().size()) : 1;
    for (size_t frameIndex = 0; frameIndex < tile->AnimationFrames.size(); ++frameIndex)
    {
        const TileMap::TileAnimationFrame& frame = tile->AnimationFrames[frameIndex];
        const std::string ordinal = std::to_string(frameIndex + 1);
        const uint32_t tileId = tile->Id;

        // Edits land on the frame by index; the rows are rebuilt from the tile
        // after every structural change, so an index captured here stays true.
        auto* tileField = InspectorDrag::AddIntRowWithDrag(
            m_AnimationRows, "Frame " + ordinal + " Tile", static_cast<int>(frame.TileId),
            [](const int&) {},
            [this, tileId, frameIndex](const int& value)
            {
                TileMap::TileSetTile* edited = TileSet() ? TileSet()->FindTile(tileId) : nullptr;
                if (!edited || frameIndex >= edited->AnimationFrames.size())
                    return;
                edited->AnimationFrames[frameIndex].TileId = static_cast<uint32_t>(std::max(1, value));
                SetStatus("Animation edited — Apply & Save to write it.", false);
            },
            static_cast<int>(frame.TileId), "Which tile of this set this frame shows");
        tileField->SetRange(1, std::max(1, maxTileId));

        InspectorDrag::AddFloatRowWithDrag(
            m_AnimationRows, "Frame " + ordinal + " Seconds", frame.DurationSeconds,
            [](const float&) {},
            [this, tileId, frameIndex](const float& value)
            {
                TileMap::TileSetTile* edited = TileSet() ? TileSet()->FindTile(tileId) : nullptr;
                if (!edited || frameIndex >= edited->AnimationFrames.size())
                    return;
                edited->AnimationFrames[frameIndex].DurationSeconds = std::max(0.0f, value);
                SetStatus("Animation edited — Apply & Save to write it.", false);
            },
            frame.DurationSeconds, "How long this frame stays on screen", 0.0f, 60.0f);

        auto remove = std::make_unique<Button>();
        remove->SetId("TileSetRemoveAnimationFrame" + ordinal);
        remove->SetText("Remove Frame " + ordinal);
        remove->AddClass("small");
        remove->AddClass("secondary");
        remove->RegisterEventHandler(kEventButtonClick, [this, frameIndex](UIEvent&) {
            RemoveAnimationFrameFromSelectedTile(frameIndex);
        });
        m_AnimationRows->AddChild(std::move(remove));
    }
}

void TileSetEditorPanel::AddAnimationFrameToSelectedTile()
{
    TileMap::TileSetTile* tile = SelectedCollisionTile();
    if (!tile)
        return;
    // A new frame starts on the tile itself: the loop then shows the tile
    // unchanged until the author points the frame elsewhere.
    tile->AnimationFrames.push_back(TileMap::TileAnimationFrame{tile->Id, 0.1f});
    SetStatus("Animation edited — Apply & Save to write it.", false);
    ShowSelectedTileAnimation();
}

void TileSetEditorPanel::RemoveAnimationFrameFromSelectedTile(size_t frameIndex)
{
    TileMap::TileSetTile* tile = SelectedCollisionTile();
    if (!tile || frameIndex >= tile->AnimationFrames.size())
        return;
    tile->AnimationFrames.erase(tile->AnimationFrames.begin() +
                                static_cast<std::ptrdiff_t>(frameIndex));
    SetStatus("Animation edited — Apply & Save to write it.", false);
    ShowSelectedTileAnimation();
}

TileMap::TileSetTile* TileSetEditorPanel::SelectedCollisionTile()
{
    if (!TileSet() || !m_CollisionCanvas)
        return nullptr;
    const uint32_t tileId = m_CollisionCanvas->SelectedTileId();
    if (tileId == 0)
        return nullptr;
    return TileSet()->FindTile(tileId);
}

void TileSetEditorPanel::ShowSelectedTileCollision()
{
    if (!m_CollisionSelectionLabel || !m_CollisionModeDropdown || !m_CollisionMaterialField)
        return;

    const TileMap::TileSetTile* tile = SelectedCollisionTile();
    const bool hasTile = tile != nullptr;

    m_CollisionSelectionLabel->SetText(
        hasTile ? "Tile #" + std::to_string(tile->Id) : std::string(kNoTileSelectedText));

    // The rows mirror the selected tile, so pushing its values back through the
    // change callbacks would re-author what was only being displayed.
    m_CollisionModeDropdown->SetSelectedIndexWithoutNotify(
        hasTile && tile->CollisionMode == TileMap::TileCollisionMode::OneWay
            ? kCollisionModeOneWayIndex
            : kCollisionModeSolidIndex);
    m_CollisionMaterialField->SetValue(hasTile ? tile->PhysicsMaterial : GUID::Null());

    m_CollisionModeDropdown->SetEnabled(hasTile);
    m_CollisionMaterialField->SetEnabled(hasTile);
    ShowSelectedTileAnimation();
}

void TileSetEditorPanel::OnSelectedTileCollisionModeChanged(const std::string& value)
{
    TileMap::TileSetTile* tile = SelectedCollisionTile();
    if (!tile)
        return;
    tile->CollisionMode = (value == kCollisionModeOneWayValue)
                              ? TileMap::TileCollisionMode::OneWay
                              : TileMap::TileCollisionMode::Solid;
    if (m_CollisionCanvas)
        m_CollisionCanvas->Refresh();
    SetStatus("Collision edited — Apply & Save to write it.", false);
}

void TileSetEditorPanel::OnSelectedTilePhysicsMaterialChanged(const GUID& materialGuid)
{
    TileMap::TileSetTile* tile = SelectedCollisionTile();
    if (!tile)
        return;
    tile->PhysicsMaterial = materialGuid;
    SetStatus("Collision edited — Apply & Save to write it.", false);
}

std::unique_ptr<UIElement> TileSetEditorPanel::BuildTerrainSection()
{
    auto section = std::make_unique<UIElement>();
    section->AddClass("ts-terrain-section");

    auto header = std::make_unique<UIElement>();
    header->AddClass("ts-terrain-header");
    header->AddChild(MakeLabel("ts-section-label", "Terrains"));

    auto addSet = MakeActionButton("TileSetAddTerrainSet", "Add Terrain Set", "tm-glyph-add");
    addSet->SetTooltip("Add a terrain set: a group of terrains that blend into each other");
    addSet->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) {
        if (!TileSet())
        {
            SetStatus("Open or create a tile set first.", true);
            return;
        }
        TileMap::TerrainSetDef set;
        set.Name = "Terrain Set " + std::to_string(TileSet()->TerrainSets().size() + 1);
        TileSet()->TerrainSets().push_back(std::move(set));
        InvalidateTerrainRows();
    });
    header->AddChild(std::move(addSet));
    section->AddChild(std::move(header));

    auto list = std::make_unique<ScrollView>();
    list->AddClass("ts-terrain-list");
    m_TerrainSetList = list->GetViewport();
    section->AddChild(std::move(list));

    auto hint = MakeLabel("ts-terrain-hint",
                          "Left-drag paints the selected terrain into a tile's center or one of its "
                          "eight edge and corner matches; right-drag clears them.");
    m_TerrainHintLabel = hint.get();
    section->AddChild(std::move(hint));

    auto canvas = std::make_unique<TileSetTerrainBitEditor>();
    canvas->SetOnPeeringChanged([this]() {
        SetStatus("Terrain bits edited — Apply & Save to write them.", false);
    });
    m_TerrainCanvas = canvas.get();
    section->AddChild(std::move(canvas));

    return section;
}

uint32_t TileSetEditorPanel::DefaultTerrainColor(size_t terrainIndex)
{
    return kTerrainPalette[terrainIndex % kTerrainPaletteCount];
}

void TileSetEditorPanel::Update()
{
    if (!m_TerrainRowsDirty)
        return;
    m_TerrainRowsDirty = false;
    RebuildTerrainRows();
}

void TileSetEditorPanel::ShowTerrainState()
{
    if (m_TerrainCanvas)
    {
        m_TerrainCanvas->SetTileSet(TileSet());
        m_TerrainCanvas->SetActiveTerrain(m_ActiveTerrainSet, m_ActiveTerrain);
    }
    if (m_CollisionCanvas)
        m_CollisionCanvas->SetTileSet(TileSet());
    RebuildTerrainRows();
}

void TileSetEditorPanel::SetActiveTerrain(int16_t terrainSet, int16_t terrain)
{
    m_ActiveTerrainSet = terrainSet;
    m_ActiveTerrain = terrain;
    if (m_TerrainCanvas)
        m_TerrainCanvas->SetActiveTerrain(terrainSet, terrain);
    InvalidateTerrainRows();
}

void TileSetEditorPanel::RebuildTerrainRows()
{
    if (!m_TerrainSetList)
        return;
    m_TerrainSetList->RemoveAllChildren();

    if (!TileSet())
    {
        m_TerrainSetList->AddChild(MakeLabel("ts-terrain-empty", "No tile set open."));
        return;
    }
    if (TileSet()->TerrainSets().empty())
    {
        m_TerrainSetList->AddChild(
            MakeLabel("ts-terrain-empty", "No terrain sets yet — add one to start annotating."));
        return;
    }

    std::vector<TileMap::TerrainSetDef>& sets = TileSet()->TerrainSets();
    for (size_t setIndex = 0; setIndex < sets.size(); ++setIndex)
    {
        TileMap::TerrainSetDef& set = sets[setIndex];
        const int16_t setId = static_cast<int16_t>(setIndex);

        auto setRow = std::make_unique<UIElement>();
        setRow->AddClass("ts-terrain-set-row");

        auto name = std::make_unique<TextField>();
        name->AddClass("ts-terrain-name");
        name->SetValueWithoutNotify(set.Name);
        name->SetTooltip("Terrain set name");
        name->SetOnValueChanged([this, setId](const std::string& text) {
            if (!TileSet() || static_cast<size_t>(setId) >= TileSet()->TerrainSets().size())
                return;
            TileSet()->TerrainSets()[static_cast<size_t>(setId)].Name = text;
        });
        setRow->AddChild(std::move(name));

        auto mode = std::make_unique<Dropdown>();
        mode->AddClass("ts-terrain-mode");
        mode->SetTooltip("Which edge and corner matches this set uses");
        mode->SetOptionsFromLabels(kMatchModeLabels, static_cast<int>(set.MatchMode));
        Dropdown* modeRaw = mode.get();
        // The label list is authored in TerrainMatchMode order, so the selected
        // index is the mode; the callback's string value is display text.
        modeRaw->SetOnValueChanged([this, setId, modeRaw](const std::string&) {
            if (!TileSet() || static_cast<size_t>(setId) >= TileSet()->TerrainSets().size())
                return;
            const int index = std::clamp(modeRaw->GetSelectedIndex(), 0,
                                         static_cast<int>(kMatchModeLabels.size()) - 1);
            TileSet()->TerrainSets()[static_cast<size_t>(setId)].MatchMode =
                static_cast<TileMap::TerrainMatchMode>(index);
            SetStatus("Match mode changed — Apply & Save to write it.", false);
        });
        setRow->AddChild(std::move(mode));

        auto addTerrain = MakeActionButton("TileSetAddTerrain" + std::to_string(setIndex),
                                           "Add Terrain", "tm-glyph-add");
        addTerrain->SetTooltip("Add a terrain to this set");
        addTerrain->RegisterEventHandler(kEventButtonClick, [this, setId](UIEvent&) {
            if (!TileSet() || static_cast<size_t>(setId) >= TileSet()->TerrainSets().size())
                return;
            auto& terrains = TileSet()->TerrainSets()[static_cast<size_t>(setId)].Terrains;
            TileMap::TerrainDef terrain;
            terrain.Name = "Terrain " + std::to_string(terrains.size() + 1);
            terrain.ColorRGBA = DefaultTerrainColor(terrains.size());
            terrains.push_back(std::move(terrain));
            SetActiveTerrain(setId, static_cast<int16_t>(terrains.size() - 1));
        });
        setRow->AddChild(std::move(addTerrain));

        auto removeSet =
            MakeIconButton("TileSetRemoveTerrainSet" + std::to_string(setIndex), "tm-glyph-close");
        removeSet->SetTooltip("Remove this terrain set");
        removeSet->RegisterEventHandler(kEventButtonClick, [this, setId](UIEvent&) {
            if (!TileSet() || static_cast<size_t>(setId) >= TileSet()->TerrainSets().size())
                return;
            auto& allSets = TileSet()->TerrainSets();
            allSets.erase(allSets.begin() + setId);
            // Every tile annotated against this set now points at nothing, and
            // the indices above it have shifted; drop those annotations rather
            // than leave the tileset self-inconsistent.
            for (TileMap::TileSetTile& tile : TileSet()->Tiles())
            {
                if (tile.Peering.TerrainSet == setId)
                    tile.Peering = TileMap::TerrainPeering{};
                else if (tile.Peering.TerrainSet > setId)
                    --tile.Peering.TerrainSet;
            }
            SetActiveTerrain(-1, -1);
        });
        setRow->AddChild(std::move(removeSet));

        m_TerrainSetList->AddChild(std::move(setRow));

        for (size_t terrainIndex = 0; terrainIndex < set.Terrains.size(); ++terrainIndex)
        {
            const int16_t terrainId = static_cast<int16_t>(terrainIndex);
            auto row = std::make_unique<UIElement>();
            row->AddClass("ts-terrain-row");
            if (setId == m_ActiveTerrainSet && terrainId == m_ActiveTerrain)
                row->AddClass("active");

            auto arm = MakeActionButton(
                "TileSetArmTerrain" + std::to_string(setIndex) + "_" + std::to_string(terrainIndex),
                set.Terrains[terrainIndex].Name, std::string());
            arm->AddClass("ts-terrain-arm");
            arm->SetTooltip("Paint this terrain into the tiles below");
            arm->RegisterEventHandler(
                kEventButtonClick,
                [this, setId, terrainId](UIEvent&) { SetActiveTerrain(setId, terrainId); });
            row->AddChild(std::move(arm));

            auto terrainName = std::make_unique<TextField>();
            terrainName->AddClass("ts-terrain-name");
            terrainName->SetValueWithoutNotify(set.Terrains[terrainIndex].Name);
            terrainName->SetTooltip("Terrain name");
            terrainName->SetOnValueChanged([this, setId, terrainId](const std::string& text) {
                if (!TileSet() || static_cast<size_t>(setId) >= TileSet()->TerrainSets().size())
                    return;
                auto& terrains = TileSet()->TerrainSets()[static_cast<size_t>(setId)].Terrains;
                if (static_cast<size_t>(terrainId) >= terrains.size())
                    return;
                terrains[static_cast<size_t>(terrainId)].Name = text;
            });
            row->AddChild(std::move(terrainName));

            m_TerrainSetList->AddChild(std::move(row));
        }
    }
}

void TileSetEditorPanel::SetContext(const EditorContext* ctx)
{
    m_Context = ctx;
    if (!m_AtlasField)
        return;

    AssetRegistry* registry = ctx && ctx->Assets ? &ctx->Assets->GetRegistry() : nullptr;
    m_AtlasField->SetAssetRegistry(registry);
    if (ctx && ctx->Thumbnails)
        m_AtlasField->SetThumbnailProvider(ctx->Thumbnails);

    if (m_CollisionMaterialField)
    {
        m_CollisionMaterialField->SetAssetRegistry(registry);
        if (ctx && ctx->Thumbnails)
            m_CollisionMaterialField->SetThumbnailProvider(ctx->Thumbnails);
    }
}

void TileSetEditorPanel::OnOwnerManagerChanged(UIManager* owner)
{
    // The atlas size request needs a manager; a panel built before it is
    // attached asks again as soon as one exists.
    if (owner && m_AtlasWidthPx == 0)
        RequestAtlasSize();
}

std::filesystem::path TileSetEditorPanel::AssetPathForGuid(const GUID& guid) const
{
    if (!m_Context || !m_Context->Assets || guid.IsNull())
        return {};
    AssetMetadata metadata;
    if (!m_Context->Assets->GetRegistry().TryGetAssetMetadata(guid, metadata))
        return {};
    return metadata.Path;
}

std::filesystem::path TileSetEditorPanel::TileSetPathForAtlas(const GUID& textureGuid) const
{
    const std::filesystem::path stemPath =
        Editor::TileSetDocument::PathForAtlasTexture(AssetPathForGuid(textureGuid));
    if (!m_Context || !m_Context->Assets)
        return stemPath;

    const std::filesystem::path bound = Editor::FindTileSetForAtlas(
        m_Context->Assets->GetRegistry(), textureGuid, stemPath);
    return bound.empty() ? stemPath : bound;
}

int32_t TileSetEditorPanel::GridValue(const IntField* field, int32_t fallback) const
{
    return field ? static_cast<int32_t>(field->GetValue()) : fallback;
}

void TileSetEditorPanel::OpenTileSet(const GUID& tileSetGuid)
{
    const std::filesystem::path path = AssetPathForGuid(tileSetGuid);
    if (path.empty())
    {
        SetStatus("Tile set not found in the asset registry.", true);
        return;
    }
    OpenTileSetPath(path);
}

void TileSetEditorPanel::OpenTileSetPath(const std::filesystem::path& path)
{
    if (path.empty())
    {
        SetStatus("Tile set not found.", true);
        return;
    }

    GUID guid;
    if (m_Context && m_Context->Assets)
        guid = m_Context->Assets->GetRegistry().GetAssetGUID(path);

    if (m_Document.OpenOrCreate(guid, path) == Editor::TileSetDocument::OpenOutcome::Failed)
    {
        SetStatus("Failed to load " + path.filename().string() + ".", true);
        return;
    }

    ShowLoadedAsset();
    SetStatus("Opened " + path.filename().string() + ".", false);
}

void TileSetEditorPanel::OpenOrCreateForTexture(const GUID& textureGuid)
{
    const std::filesystem::path texturePath = AssetPathForGuid(textureGuid);
    if (texturePath.empty())
    {
        SetStatus("Texture not found in the asset registry.", true);
        return;
    }

    const std::filesystem::path path = TileSetPathForAtlas(textureGuid);
    if (std::filesystem::exists(path))
    {
        OpenTileSetPath(path);
        return;
    }

    if (m_Document.OpenOrCreate(GUID::Null(), path) == Editor::TileSetDocument::OpenOutcome::Failed)
    {
        SetStatus("Cannot create " + path.filename().string() + ".", true);
        return;
    }

    if (m_TileWidthField)
        m_TileWidthField->SetValueWithoutNotify(kDefaultTileSizePx);
    if (m_TileHeightField)
        m_TileHeightField->SetValueWithoutNotify(kDefaultTileSizePx);
    if (m_MarginField)
        m_MarginField->SetValueWithoutNotify(kMinOffsetPx);
    if (m_SpacingField)
        m_SpacingField->SetValueWithoutNotify(kMinOffsetPx);

    if (m_AtlasField)
        m_AtlasField->SetValue(textureGuid);
    OnAtlasTextureChanged(textureGuid);
    // The dimensions come from the image file, not from the GPU upload: the
    // upload needs a mounted panel and several frames, neither of which a
    // headless "make a tileset from this texture" call has.
    (void)ApplyAndSave();
}

void TileSetEditorPanel::ShowLoadedAsset()
{
    TileMap::TileSetAsset* tileSet = m_Document.Asset();
    if (!tileSet)
        return;

    // SetValueWithoutNotify bypasses IntField's range clamp, so a malformed
    // file's values are brought into range here rather than round-tripping out.
    const int32_t tileWidth = tileSet->TileWidthPx() > 0
                                  ? std::clamp(tileSet->TileWidthPx(), kMinTileSizePx, kMaxTileSizePx)
                                  : kDefaultTileSizePx;
    const int32_t tileHeight = tileSet->TileHeightPx() > 0
                                   ? std::clamp(tileSet->TileHeightPx(), kMinTileSizePx, kMaxTileSizePx)
                                   : kDefaultTileSizePx;

    if (m_TileWidthField)
        m_TileWidthField->SetValueWithoutNotify(tileWidth);
    if (m_TileHeightField)
        m_TileHeightField->SetValueWithoutNotify(tileHeight);
    if (m_MarginField)
        m_MarginField->SetValueWithoutNotify(std::clamp(tileSet->MarginPx(), kMinOffsetPx, kMaxOffsetPx));
    if (m_SpacingField)
        m_SpacingField->SetValueWithoutNotify(std::clamp(tileSet->SpacingPx(), kMinOffsetPx, kMaxOffsetPx));

    // Serialized dimensions are a starting point only — the live texture is
    // authoritative and replaces them once the upload resolves.
    m_AtlasWidthPx = tileSet->AtlasWidthPx();
    m_AtlasHeightPx = tileSet->AtlasHeightPx();

    if (m_AtlasField)
        m_AtlasField->SetValue(tileSet->AtlasTexture());
    if (m_Preview)
        m_Preview->SetAtlas(tileSet->AtlasTexture());

    RequestAtlasSize();
    OnGridFieldChanged();
    ShowTerrainState();
}

void TileSetEditorPanel::OnAtlasTextureChanged(const GUID& textureGuid)
{
    m_AtlasWidthPx = 0;
    m_AtlasHeightPx = 0;
    if (m_Preview)
        m_Preview->SetAtlas(textureGuid);
    RequestAtlasSize();
    UpdateSummary();

    // Picking a texture with nothing open is the author asking for that
    // texture's tile set — including one they authored earlier, under any name.
    // Read it: the save below writes to the same path either way, so a panel
    // that stayed blank here would overwrite their work with an empty slice.
    if (m_Document.IsOpen() || textureGuid.IsNull())
        return;

    const std::filesystem::path path = TileSetPathForAtlas(textureGuid);
    if (path.empty() || !std::filesystem::exists(path))
        return;

    OpenTileSetPath(path);
}

void TileSetEditorPanel::OnGridFieldChanged()
{
    if (m_Preview)
    {
        m_Preview->SetGrid(GridValue(m_TileWidthField, kDefaultTileSizePx),
                           GridValue(m_TileHeightField, kDefaultTileSizePx),
                           GridValue(m_MarginField, kMinOffsetPx),
                           GridValue(m_SpacingField, kMinOffsetPx));
    }
    UpdateSummary();
}

void TileSetEditorPanel::RequestAtlasSize()
{
    UIManager* manager = GetOwnerManager();
    const GUID atlas = m_AtlasField ? m_AtlasField->GetValue() : GUID::Null();
    if (!manager || atlas.IsNull())
        return;

    std::weak_ptr<int> alive = m_AsyncLifetime;
    manager->RequestBackgroundTexture(
        atlas, [this, alive, atlas](Rendering::TextureHandle handle, uint32_t width, uint32_t height) {
            if (alive.expired() || !handle.IsValid())
                return;
            // A late answer for a texture the user has since replaced must not
            // overwrite the current selection's size.
            if (!m_AtlasField || m_AtlasField->GetValue() != atlas)
                return;
            OnAtlasSizeResolved(static_cast<int32_t>(width), static_cast<int32_t>(height));
        });
}

void TileSetEditorPanel::OnAtlasSizeResolved(int32_t widthPx, int32_t heightPx)
{
    m_AtlasWidthPx = widthPx;
    m_AtlasHeightPx = heightPx;
    UpdateSummary();
}

void TileSetEditorPanel::UpdateSummary()
{
    if (!m_SummaryLabel)
        return;

    if (m_AtlasWidthPx <= 0 || m_AtlasHeightPx <= 0)
    {
        const GUID atlas = m_AtlasField ? m_AtlasField->GetValue() : GUID::Null();
        m_SummaryLabel->SetText(atlas.IsNull() ? "No atlas texture." : "Loading atlas texture...");
        return;
    }

    const int32_t columns = TileMap::TileSetAsset::GridColumns(
        m_AtlasWidthPx, GridValue(m_TileWidthField, kDefaultTileSizePx),
        GridValue(m_MarginField, kMinOffsetPx), GridValue(m_SpacingField, kMinOffsetPx));
    const int32_t rows = TileMap::TileSetAsset::GridRows(
        m_AtlasHeightPx, GridValue(m_TileHeightField, kDefaultTileSizePx),
        GridValue(m_MarginField, kMinOffsetPx), GridValue(m_SpacingField, kMinOffsetPx));

    m_SummaryLabel->SetText(std::to_string(m_AtlasWidthPx) + " x " + std::to_string(m_AtlasHeightPx) +
                            " px  |  " + std::to_string(columns) + " x " + std::to_string(rows) +
                            " = " + std::to_string(columns * rows) + " tiles");
}

bool TileSetEditorPanel::ResolveAtlasSizeFromFile(const GUID& textureGuid, int32_t& outWidthPx,
                                                  int32_t& outHeightPx) const
{
    const std::filesystem::path path = AssetPathForGuid(textureGuid);
    if (path.empty())
        return false;

    // Decoding the image on the spot is the only answer available before the
    // renderer has uploaded it, and it is authoritative either way.
    TextureAsset texture(textureGuid, path);
    if (!texture.Load())
        return false;
    outWidthPx = static_cast<int32_t>(texture.GetWidth());
    outHeightPx = static_cast<int32_t>(texture.GetHeight());
    return outWidthPx > 0 && outHeightPx > 0;
}

bool TileSetEditorPanel::ApplyAndSave()
{
    if (!m_Context || !m_Context->Assets)
    {
        SetStatus("No asset manager available.", true);
        return false;
    }

    const GUID atlas = m_AtlasField ? m_AtlasField->GetValue() : GUID::Null();
    if (atlas.IsNull())
    {
        SetStatus("Pick an atlas texture first.", true);
        return false;
    }
    if ((m_AtlasWidthPx <= 0 || m_AtlasHeightPx <= 0) &&
        !ResolveAtlasSizeFromFile(atlas, m_AtlasWidthPx, m_AtlasHeightPx))
    {
        SetStatus("Cannot read the atlas texture's dimensions.", true);
        return false;
    }

    if (!m_Document.IsOpen())
    {
        const std::filesystem::path path = TileSetPathForAtlas(atlas);
        if (path.empty())
        {
            SetStatus("Cannot resolve a path for the atlas texture.", true);
            return false;
        }
        // Binding reads whatever is already at that path, so the save below can
        // never blank an existing tile set.
        if (m_Document.OpenOrCreate(GUID::Null(), path) ==
            Editor::TileSetDocument::OpenOutcome::Failed)
        {
            SetStatus("Cannot read " + path.filename().string() + " — not overwriting it.", true);
            return false;
        }
    }

    // Tile ids are grid positions, so a grid that slices to a different shape
    // leaves the authored data on tiles it no longer describes. That is the
    // author's call to make, not a side effect of pressing Save.
    const GridSpec grid = FormGrid();
    if (m_Document.ReSliceDropsAuthoring(grid.TileWidthPx, grid.TileHeightPx, grid.MarginPx,
                                         grid.SpacingPx))
    {
        if (!EnsureReSliceConfirmModal())
        {
            SetStatus("Cannot show the re-slice confirmation — not writing " +
                          m_Document.Path().filename().string() + ".",
                      true);
            return false;
        }
        // The confirmation names this grid, so this grid is what confirming
        // writes; the fields stay editable while the modal is up.
        m_PendingReSliceGrid = grid;
        m_ReSliceConfirmModal->Show(
            "Re-slice " + m_Document.Path().filename().string() + " to " +
                std::to_string(grid.TileWidthPx) + " x " + std::to_string(grid.TileHeightPx) +
                " px?",
            "This grid slices the atlas into a different number of tiles, so the\n"
            "collision, modes, materials and terrain bits already authored will no\n"
            "longer line up with the tiles they were drawn on.\n\n"
            "There is no undo for this.",
            "Re-slice");
        SetStatus("This grid would move authored tile data — confirm to continue.", true);
        return false;
    }

    return WriteWithGrid(grid);
}

TileSetEditorPanel::GridSpec TileSetEditorPanel::FormGrid() const
{
    return GridSpec{GridValue(m_TileWidthField, kDefaultTileSizePx),
                    GridValue(m_TileHeightField, kDefaultTileSizePx),
                    GridValue(m_MarginField, kMinOffsetPx),
                    GridValue(m_SpacingField, kMinOffsetPx)};
}

bool TileSetEditorPanel::WriteWithGrid(const GridSpec& grid)
{
    TileMap::TileSetAsset* tileSet = m_Document.Asset();
    if (!tileSet)
        return false;

    // Asked before the asset is re-sliced: a save that is going to be refused
    // must not leave the panel holding a rebuilt tile table it was told was
    // never written.
    if (m_Document.HasChangedOnDisk())
        return OfferDiskConflictOverwrite(grid);

    const GUID atlas = m_AtlasField ? m_AtlasField->GetValue() : GUID::Null();
    tileSet->SetAtlasTexture(atlas, m_AtlasWidthPx, m_AtlasHeightPx);
    if (!tileSet->SliceGrid(grid.TileWidthPx, grid.TileHeightPx, grid.MarginPx, grid.SpacingPx))
    {
        SetStatus("That atlas and tile size slice to more tiles than a tileset can hold.", true);
        return false;
    }

    const std::filesystem::path path = m_Document.Path();
    switch (m_Document.Save())
    {
    case Editor::TileSetDocument::SaveOutcome::Written:
        break;
    case Editor::TileSetDocument::SaveOutcome::ChangedOnDisk:
        // The file moved between the check above and the write. The asset in
        // memory already carries this grid, so confirming re-applies the same
        // one and writes it.
        return OfferDiskConflictOverwrite(grid);
    case Editor::TileSetDocument::SaveOutcome::Failed:
        SetStatus("Failed to write " + path.filename().string() + ".", true);
        return false;
    }

    if (m_Context && m_Context->Assets)
        RegisterTileSetPath(m_Context->Assets, path);
    UpdateSummary();
    // A re-slice rebuilds the tile table, so the canvases are looking at tiles
    // that no longer exist until they re-read it.
    if (m_TerrainCanvas)
        m_TerrainCanvas->SetTileSet(tileSet);
    if (m_CollisionCanvas)
        m_CollisionCanvas->SetTileSet(tileSet);
    InvalidateTerrainRows();
    SetStatus("Saved " + path.filename().string() + " (" +
                  std::to_string(tileSet->Tiles().size()) + " tiles).",
              false);
    return true;
}

ConfirmActionModal* TileSetEditorPanel::AttachConfirmModal()
{
    auto modal = std::make_unique<ConfirmActionModal>();
    ConfirmActionModal* attached = modal.get();

    UIManager* ui = GetOwnerManager();
    UIElement* uiRoot = ui ? ui->GetRootElement() : nullptr;
    if (uiRoot)
        uiRoot->AddChild(std::move(modal));
    else
        AddChild(std::move(modal));
    return attached;
}

bool TileSetEditorPanel::EnsureReSliceConfirmModal()
{
    if (m_ReSliceConfirmModal)
        return true;

    m_ReSliceConfirmModal = AttachConfirmModal();

    // The modal lives on the UI root, so it survives this panel being closed or
    // re-docked with the confirmation still up. Confirming then reaches a freed
    // panel — and writes a file from it — unless the callbacks check the same
    // lifetime token the texture callbacks use.
    std::weak_ptr<int> alive = m_AsyncLifetime;
    m_ReSliceConfirmModal->SetOnConfirm([this, alive]() {
        if (alive.expired())
            return;
        (void)WriteWithGrid(m_PendingReSliceGrid);
    });
    m_ReSliceConfirmModal->SetOnCancel([this, alive]() {
        if (alive.expired())
            return;
        SetStatus("Re-slice cancelled — nothing was written.", false);
    });
    return true;
}

bool TileSetEditorPanel::EnsureDiskConflictModal()
{
    if (m_DiskConflictModal)
        return true;

    m_DiskConflictModal = AttachConfirmModal();

    std::weak_ptr<int> alive = m_AsyncLifetime;
    m_DiskConflictModal->SetOnConfirm([this, alive]() {
        if (alive.expired())
            return;
        // Declaring the file as it stands now to be this document's base is what
        // makes the retry write instead of refusing again. The author has been
        // told what it costs.
        m_Document.AcceptDiskState();
        (void)WriteWithGrid(m_PendingDiskConflictGrid);
    });
    m_DiskConflictModal->SetOnCancel([this, alive]() {
        if (alive.expired())
            return;
        SetStatus("Nothing was written — re-open the tile set to take the changes on disk.",
                  false);
    });
    return true;
}

bool TileSetEditorPanel::OfferDiskConflictOverwrite(const GridSpec& grid)
{
    const std::string name = m_Document.Path().filename().string();
    if (!EnsureDiskConflictModal())
    {
        SetStatus(name + " changed on disk since it was opened — nothing was written. Re-open it "
                         "to pick up those changes.",
                  true);
        return false;
    }

    // The form stays editable while the modal is up, so confirming writes the
    // grid the author was shown.
    m_PendingDiskConflictGrid = grid;
    // The modal body renders `white-space: pre`, so it wraps where this text
    // says to and nowhere else; a long unbroken line is clipped at the window
    // edge instead of flowing.
    m_DiskConflictModal->Show(
        "Overwrite " + name + " with this copy?",
        "This tile set changed on disk since it was opened — another panel, an\n"
        "external edit or a re-import wrote it. Those changes are not in this\n"
        "copy, so overwriting drops them with no trace.\n\n"
        "Cancel and re-open the tile set to take those changes instead.",
        "Overwrite");
    SetStatus(name + " changed on disk — confirm to overwrite it with this copy.", true);
    return false;
}

void TileSetEditorPanel::SetStatus(const std::string& text, bool isError)
{
    if (!m_StatusLabel)
        return;
    m_StatusLabel->SetText(text);
    if (isError)
        m_StatusLabel->AddClass("error");
    else
        m_StatusLabel->RemoveClass("error");
}

} // namespace GameEngine
