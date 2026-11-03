#include "Panels/TilePalettePanel.h"

#include "Panels/TilePaletteBrushList.h"
#include "Panels/TilePaletteGrid.h"
#include "Panels/TilePaletteIntGridList.h"
#include "Panels/TilePaletteLayerList.h"
#include "Panels/TilePaletteTerrainList.h"

#include "EditorContext.h"
#include "EditorContextMenu/UIContextMenu.h"
#include "Editor/Shortcuts/EditorShortcuts.h"
#include "TileMap/TileLayerCommands.h"
#include "TileMap/TileLayerEdits.h"
#include "TileMap/TileReplaceEdit.h"
#include "TileMap/TileSetDocument.h"
#include "UI/AssetField.h"
#include "UI/EditorIcons.h"

#include "UI/Controls/Button.h"
#include "UI/Controls/Checkbox.h"
#include "UI/Controls/IntField.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/Splitter.h"
#include "UI/Controls/WeightedPane.h"
#include "UI/UIElement.h"
#include "UI/UIEvents.h"

#include "FileSystem/FileSystem.h"
#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Logger/Logger.h"
#include "TileMap/TileCellEncoding.h"
#include "TileMap/TileMapAsset.h"
#include "TileMap/TileMapData.h"
#include "TileMap/TileMapLayerData.h"
#include "TileMap/TileSetAsset.h"
#include "TileMapECS/TileMapRuntimeStore.h"
#include "UndoRedo/UndoRedoService.h"
#include "ECS/World.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace GameEngine
{

namespace
{

std::unique_ptr<Label> MakeLabel(const std::string& cssClass, const std::string& text)
{
    auto label = std::make_unique<Label>();
    label->AddClass(cssClass);
    label->SetText(text);
    return label;
}

/** A glyph-only button. The tooltip is the only place its name appears. */
std::unique_ptr<Button> MakeIconButton(const std::string& id, const std::string& tooltip,
                                       const std::string& glyphClass)
{
    auto button = std::make_unique<Button>();
    button->SetId(id);
    button->AddClass("small");
    button->AddClass("secondary");
    button->AddClass("tm-icon");
    button->AddClass(glyphClass);
    button->SetTooltip(tooltip);
    return button;
}

/** Appends the user's current chord for a "Tile Map Tool" shortcut, when bound. */
std::string WithShortcut(std::string tooltip, const char* shortcutName)
{
    const std::string chord = Editor::CatalogShortcutLabel("Tile Map Tool", shortcutName);
    if (!chord.empty())
        tooltip += " (" + chord + ")";
    return tooltip;
}

std::string ToolDisplayName(Editor::TileMapTool tool)
{
    switch (tool)
    {
    case Editor::TileMapTool::Brush: return "Brush";
    case Editor::TileMapTool::Eraser: return "Eraser";
    case Editor::TileMapTool::Picker: return "Picker";
    case Editor::TileMapTool::Fill: return "Fill";
    case Editor::TileMapTool::Rect: return "Rect";
    case Editor::TileMapTool::Marquee: return "Marquee";
    case Editor::TileMapTool::Terrain: return "Terrain";
    case Editor::TileMapTool::IntGrid: return "IntGrid";
    }
    return "Brush";
}

std::string ToolTooltip(Editor::TileMapTool tool)
{
    switch (tool)
    {
    case Editor::TileMapTool::Brush: return "Stamp the selection along the stroke";
    case Editor::TileMapTool::Eraser: return "Clear cells along the stroke";
    case Editor::TileMapTool::Picker: return "Pick the cell under the cursor into the stamp";
    case Editor::TileMapTool::Fill: return "Flood-fill the contiguous region under the cursor";
    case Editor::TileMapTool::Rect: return "Drag a rectangle and fill it with the stamp";
    case Editor::TileMapTool::Marquee: return "Select a block of cells to move or copy";
    case Editor::TileMapTool::Terrain:
        return "Paint the chosen terrain, picking transition tiles automatically";
    case Editor::TileMapTool::IntGrid:
        return "Paint the chosen value; rule layers update the generated tiles live";
    }
    return {};
}

std::string ToolGlyphClass(Editor::TileMapTool tool)
{
    switch (tool)
    {
    case Editor::TileMapTool::Brush: return "tm-glyph-brush";
    case Editor::TileMapTool::Eraser: return "tm-glyph-eraser";
    case Editor::TileMapTool::Picker: return "tm-glyph-picker";
    case Editor::TileMapTool::Fill: return "tm-glyph-fill";
    case Editor::TileMapTool::Rect: return "tm-glyph-rect";
    case Editor::TileMapTool::Marquee: return "tm-glyph-marquee";
    case Editor::TileMapTool::Terrain: return "tm-glyph-terrain";
    case Editor::TileMapTool::IntGrid: return "tm-glyph-intgrid";
    }
    return "tm-glyph-brush";
}

} // namespace

TilePalettePanel::TilePalettePanel()
    : DockPanel("Tile Palette")
{
    AddClass("tile-palette-panel");
    RequestSubtreeStyleAssetPath("UI/theme/tile-palette.css");
    RequestSubtreeStyleAssetPath("UI/theme/tilemap-icons.css");
    BuildLayout();
}

void TilePalettePanel::BuildLayout()
{
    auto root = std::make_unique<UIElement>();
    root->AddClass("tp-root");

    root->AddChild(BuildTileSetRow());
    root->AddChild(BuildToolRow());
    root->AddChild(BuildTerrainSection());
    root->AddChild(BuildIntGridSection());
    root->AddChild(BuildBrushSection());
    root->AddChild(BuildReplaceSection());

    // Atlas and layer list share the panel's remaining height through a
    // draggable split, so a map with many layers can take the room from the
    // atlas instead of scrolling in a fixed strip.
    auto splitBody = std::make_unique<UIElement>();
    splitBody->AddClass("tp-split-body");
    m_SplitBody = splitBody.get();

    auto gridPane = std::make_unique<WeightedPane>(0.62f);
    gridPane->AddClass("tp-pane");

    auto grid = std::make_unique<TilePaletteGrid>();
    grid->SetOnSelectionChanged([this](int32 column0, int32 row0, int32 column1, int32 row1) {
        OnGridSelectionChanged(column0, row0, column1, row1);
    });
    m_Grid = grid.get();
    gridPane->AddChild(std::move(grid));

    auto emptyState = MakeLabel("tp-grid-empty", "Assign a tileset to see its tiles here.");
    m_EmptyStateLabel = emptyState.get();
    gridPane->AddChild(std::move(emptyState));

    // Hover readout and zoom buttons: the wheel is the only other way to zoom,
    // and a pixel artist needs to know the scale, not guess it.
    auto gridBar = std::make_unique<UIElement>();
    gridBar->AddClass("tp-row");
    gridBar->AddClass("tp-grid-bar");
    m_GridBar = gridBar.get();
    auto gridStatus = MakeLabel("tp-grid-status", std::string());
    m_GridStatusLabel = gridStatus.get();
    gridBar->AddChild(std::move(gridStatus));
    auto zoomFit = std::make_unique<Button>();
    zoomFit->SetId("TilePaletteZoomFit");
    zoomFit->AddClass("small");
    zoomFit->AddClass("secondary");
    zoomFit->SetText("Fit");
    zoomFit->SetTooltip("Fit the whole atlas in the pane");
    zoomFit->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) {
        if (m_Grid != nullptr)
            m_Grid->ZoomToFit();
    });
    gridBar->AddChild(std::move(zoomFit));
    auto zoomActual = std::make_unique<Button>();
    zoomActual->SetId("TilePaletteZoomActual");
    zoomActual->AddClass("small");
    zoomActual->AddClass("secondary");
    zoomActual->SetText("1:1");
    zoomActual->SetTooltip("One atlas pixel per screen pixel");
    zoomActual->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) {
        if (m_Grid != nullptr)
            m_Grid->ZoomToAtlasPixels();
    });
    gridBar->AddChild(std::move(zoomActual));
    gridPane->AddChild(std::move(gridBar));

    auto hint = MakeLabel("tp-hint", std::string());
    m_HintLabel = hint.get();
    gridPane->AddChild(std::move(hint));

    splitBody->AddChild(std::move(gridPane));

    auto splitter = std::make_unique<Splitter>();
    splitter->AddClass("splitter");
    splitter->AddClass("col");
    splitter->AddClass("tp-layers-splitter");
    m_Splitter = splitter.get();
    splitBody->AddChild(std::move(splitter));

    auto layersPane = std::make_unique<WeightedPane>(0.38f);
    layersPane->AddClass("tp-pane");
    layersPane->AddChild(BuildLayerSection());
    splitBody->AddChild(std::move(layersPane));

    root->AddChild(std::move(splitBody));

    AddChild(std::move(root));
    SetHint("Assign a tileset to start painting.");
}

std::unique_ptr<UIElement> TilePalettePanel::BuildTileSetRow()
{
    auto section = std::make_unique<UIElement>();
    section->AddClass("tp-tileset-section");

    auto row = std::make_unique<UIElement>();
    row->AddClass("tp-row");

    row->AddChild(MakeLabel("tp-row-label", "Tileset"));

    auto field = std::make_unique<AssetField>();
    // Reads as a picker, so it takes the shared dropdown outline the inspector's
    // asset fields use rather than the heavier standalone .asset-field border.
    field->AddClass("dropdown-asset-field");
    field->AddClass("tp-tileset-field");
    field->SetAcceptedTypes({AssetType::TileSet});
    field->SetOnValueChanged([this](const GUID& guid) { OnTileSetChanged(guid); });
    m_TileSetField = field.get();
    row->AddChild(std::move(field));
    section->AddChild(std::move(row));

    // Re-skin: the field binds its tileset to the target layer; this pushes it
    // to every layer of the map, for a level whose layers all move to a new
    // atlas laid out like the old one. Its own row, so it does not read as
    // part of the picker.
    auto actions = std::make_unique<UIElement>();
    actions->AddClass("tp-row");
    actions->AddClass("tp-tileset-actions");
    actions->AddClass("hidden");
    m_ApplyTileSetAllRow = actions.get();
    auto applyAll = std::make_unique<Button>();
    applyAll->SetId("TilePaletteApplyTileSetAll");
    applyAll->SetText("Bind to All Layers");
    applyAll->AddClass("small");
    applyAll->AddClass("secondary");
    applyAll->SetTooltip("Bind this tileset to every tile layer of the map; the cells keep their ids");
    applyAll->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { ApplyTileSetToAllLayers(); });
    actions->AddChild(std::move(applyAll));
    section->AddChild(std::move(actions));

    return section;
}

std::unique_ptr<UIElement> TilePalettePanel::BuildToolRow()
{
    auto row = std::make_unique<UIElement>();
    row->AddClass("tp-row");
    row->AddClass("tp-tool-row");
    row->AddClass("disarmed");
    m_ToolRow = row.get();

    const Editor::TileMapTool tools[] = {
        Editor::TileMapTool::Brush, Editor::TileMapTool::Eraser, Editor::TileMapTool::Picker,
        Editor::TileMapTool::Fill,  Editor::TileMapTool::Rect,   Editor::TileMapTool::Marquee,
        Editor::TileMapTool::Terrain, Editor::TileMapTool::IntGrid};

    for (Editor::TileMapTool tool : tools)
    {
        auto button = MakeIconButton(
            "TilePaletteTool" + ToolDisplayName(tool),
            WithShortcut(ToolTooltip(tool), Editor::TileMapToolShortcutName(tool)),
            ToolGlyphClass(tool));
        button->AddClass("tp-tool");
        button->RegisterEventHandler(kEventButtonClick, [this, tool](UIEvent&) {
            if (m_EditContext)
                m_EditContext->SetTool(tool);
            if (m_OnToolActivated)
                m_OnToolActivated();
        });
        m_ToolButtons.emplace_back(tool, button.get());
        row->AddChild(std::move(button));
    }

    // Stamp controls share the tool row: they modify what the armed tool lays
    // down, so a row of their own only cost height. Scatter is last because it
    // is a labelled toggle rather than a glyph and reads as the row's tail.
    auto flipH =
        MakeIconButton("TilePaletteFlipH", WithShortcut("Mirror the stamp horizontally", "Flip Horizontal"), "tm-glyph-flip-h");
    flipH->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) {
        if (m_EditContext)
            m_EditContext->FlipStampHorizontal();
    });
    row->AddChild(std::move(flipH));

    auto flipV =
        MakeIconButton("TilePaletteFlipV", WithShortcut("Mirror the stamp vertically", "Flip Vertical"), "tm-glyph-flip-v");
    flipV->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) {
        if (m_EditContext)
            m_EditContext->FlipStampVertical();
    });
    row->AddChild(std::move(flipV));

    auto rotate = MakeIconButton("TilePaletteRotate", WithShortcut("Rotate the stamp 90 degrees clockwise", "Rotate Stamp"),
                                 "tm-glyph-rotate");
    rotate->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) {
        if (m_EditContext)
            m_EditContext->RotateStampClockwise();
    });
    row->AddChild(std::move(rotate));

    auto scatter = std::make_unique<Checkbox>();
    scatter->SetId("TilePaletteScatter");
    scatter->AddClass("tp-scatter");
    scatter->SetText("Scatter");
    scatter->SetTooltip("Paint one weighted-random tile from the selection instead of its footprint");
    scatter->SetOnValueChanged([this](const bool& enabled) {
        if (m_EditContext)
            m_EditContext->SetRandomScatter(enabled);
    });
    m_ScatterToggle = scatter.get();
    row->AddChild(std::move(scatter));

    return row;
}

std::unique_ptr<UIElement> TilePalettePanel::BuildTerrainSection()
{
    auto section = std::make_unique<UIElement>();
    section->AddClass("tp-terrain-section");
    m_TerrainSection = section.get();

    auto list = std::make_unique<TilePaletteTerrainList>();
    list->SetOnTerrainChosen([this](int16 terrainSet, int16 terrain) {
        if (m_EditContext == nullptr)
            return;
        // Choosing a terrain is also how the Terrain tool gets armed: picking one
        // and then having to find the tool button would be a pointless second step.
        m_EditContext->SetTerrain(terrainSet, terrain);
        m_EditContext->SetTool(Editor::TileMapTool::Terrain);
        m_SeenEditVersion = m_EditContext->Version();
        if (m_OnToolActivated)
            m_OnToolActivated();
        SyncFromEditContext();
    });
    m_TerrainList = list.get();
    section->AddChild(std::move(list));

    return section;
}

namespace
{
constexpr const char* kNewTileMapStem = "Untitled";
constexpr const char* kNewTileMapExtension = ".tilemap";
constexpr const char* kDefaultLayerName = "Layer 1";
// A fresh map's cell size in world units. 1 keeps cell coordinates and world
// coordinates interchangeable, which is the least surprising starting point.
constexpr float32 kDefaultCellSize = 1.0f;
constexpr int32 kMaxUntitledSuffix = 1000;
} // namespace

GUID TilePalettePanel::CreateTileMapAsset()
{
    if (m_Context == nullptr || m_Context->Assets == nullptr || m_Context->AssetsRoot.empty())
    {
        SetHint("Open a project before creating a tilemap.");
        return GUID{};
    }

    std::filesystem::path path = m_Context->AssetsRoot / (std::string(kNewTileMapStem) + kNewTileMapExtension);
    for (int32 suffix = 1; std::filesystem::exists(path) && suffix < kMaxUntitledSuffix; ++suffix)
    {
        path = m_Context->AssetsRoot /
               (std::string(kNewTileMapStem) + std::to_string(suffix) + kNewTileMapExtension);
    }

    TileMap::TileMapAsset asset(GUID{}, path);
    asset.Data().CellWidth = kDefaultCellSize;
    asset.Data().CellHeight = kDefaultCellSize;
    asset.Data().AddLayer(kDefaultLayerName, TileMap::TileMapLayerType::Tiles);

    Vector<uint8> bytes;
    if (!asset.SaveToData(bytes))
    {
        SetHint("Failed to serialize the new tilemap.");
        return GUID{};
    }

    const auto temporary = FileSystem::MakeTemporarySiblingPath(path);
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    output.close();
    if (!output.good())
    {
        std::error_code removeError;
        std::filesystem::remove(temporary, removeError);
        SetHint("Failed to write the new tilemap.");
        return GUID{};
    }
    if (!FileSystem::PublishFile(temporary, path))
    {
        SetHint("Failed to write the new tilemap.");
        return GUID{};
    }

    if (!m_Context->Assets->GetRegistry().RegisterAsset(path))
    {
        SetHint("Created the tilemap but could not register it.");
        return GUID{};
    }
    const GUID guid = m_Context->Assets->ResolveAssetGuid(path);
    if (guid.IsNull())
    {
        SetHint("Created the tilemap but its GUID did not resolve.");
        return GUID{};
    }
    SetHint("Created " + path.filename().string() + " — assign it to an entity's Tile Map Layer.");
    return guid;
}

void TilePalettePanel::SetOnToolActivated(std::function<void()> callback)
{
    m_OnToolActivated = std::move(callback);
}

void TilePalettePanel::SetContext(const EditorContext* ctx)
{
    m_Context = ctx;
    if (m_LayerList != nullptr)
        m_LayerList->SetContext(ctx);
    if (m_TileSetField == nullptr || ctx == nullptr)
        return;
    if (ctx->Assets != nullptr)
        m_TileSetField->SetAssetRegistry(&ctx->Assets->GetRegistry());
    m_TileSetField->SetThumbnailProvider(ctx->Thumbnails);
}

void TilePalettePanel::SetUndoRedoService(Editor::UndoRedoService* undoRedo)
{
    if (m_LayerList != nullptr)
        m_LayerList->SetUndoRedoService(undoRedo);
}

void TilePalettePanel::SetEditContext(Editor::TileMapEditContext* editContext)
{
    m_EditContext = editContext;
    if (m_LayerList != nullptr)
        m_LayerList->SetEditContext(editContext);
    if (m_EditContext == nullptr)
        return;
    m_SeenEditVersion = m_EditContext->Version();
    if (m_TileSetField != nullptr)
        m_TileSetField->SetValue(m_EditContext->TileSet());
    ReloadTileSet(m_EditContext->TileSet());
    SyncFromEditContext();
}

std::unique_ptr<UIElement> TilePalettePanel::BuildIntGridSection()
{
    auto section = std::make_unique<UIElement>();
    section->AddClass("tp-terrain-section");
    m_IntGridSection = section.get();

    auto toggleRow = std::make_unique<UIElement>();
    toggleRow->AddClass("tp-row");
    auto tint = std::make_unique<Checkbox>();
    tint->SetId("TilePaletteIntGridTint");
    tint->AddClass("tp-scatter");
    tint->SetText("Tint IntGrid cells");
    tint->SetTooltip("Wash the Scene View cells with their IntGrid value colors");
    tint->SetOnValueChanged([this](const bool& checked)
                            {
                                if (m_EditContext == nullptr)
                                    return;
                                m_EditContext->SetShowIntGridTint(checked);
                                m_SeenEditVersion = m_EditContext->Version();
                            });
    m_IntGridTintToggle = tint.get();
    toggleRow->AddChild(std::move(tint));
    section->AddChild(std::move(toggleRow));

    auto list = std::make_unique<TilePaletteIntGridList>();
    list->SetOnValueChosen([this](uint8 value)
                           {
                               if (m_EditContext == nullptr)
                                   return;
                               // Choosing a value arms the tool too: picking one and
                               // then hunting for the tool button is a pointless step.
                               m_EditContext->SetIntGridValue(value);
                               m_EditContext->SetTool(Editor::TileMapTool::IntGrid);
                               m_SeenEditVersion = m_EditContext->Version();
                               if (m_OnToolActivated)
                                   m_OnToolActivated();
                               SyncFromEditContext();
                           });
    m_IntGridList = list.get();
    section->AddChild(std::move(list));

    return section;
}

void TilePalettePanel::RefreshIntGridValues()
{
    if (m_IntGridList == nullptr || m_EditContext == nullptr)
        return;
    auto* store = TileMapECS::TileMapRuntimeStore::TryGet();
    if (store == nullptr)
        return;

    const GUID mapGuid = m_EditContext->TargetMap();
    const uint32 layerId = m_EditContext->TargetLayer();
    // Cheap gate first: the value table only moves with the map's structure or
    // the target, so a quiet frame ends on two compares and no map lookup.
    const uint64 version = mapGuid.IsNull() ? 0 : store->StructureVersion(mapGuid);
    if (mapGuid == m_IntGridListMap && layerId == m_IntGridListLayer &&
        version == m_IntGridListVersion)
    {
        return;
    }
    m_IntGridListLayer = layerId;
    m_IntGridListMap = mapGuid;
    m_IntGridListVersion = version;

    const TileMap::TileMapData* data = mapGuid.IsNull() ? nullptr : store->GetMap(mapGuid);
    const TileMap::TileMapLayerData* layer = data ? data->FindLayer(layerId) : nullptr;
    // Painting a Tiles layer still offers the semantics it is dressed from, so
    // the chips do not vanish the moment the art layer is selected.
    const TileMap::TileMapLayerData* source =
        (layer && layer->Type == TileMap::TileMapLayerType::IntGrid)
            ? layer
            : (layer && data ? data->FindLayer(layer->Rules.SourceLayerId) : nullptr);

    static const std::vector<TileMap::IntGridValueDef> kNoValues;
    m_IntGridList->SetValues(source ? source->IntGridValues : kNoValues);
    GateEmptySections();
    m_IntGridList->SetSelection(m_EditContext->IntGridValue());
}

void TilePalettePanel::ApplySplitOrientation()
{
    if (m_SplitBody == nullptr || m_Splitter == nullptr)
        return;

    // Docked wide and short there is no room to stack: the atlas gets squeezed
    // and the layer list is clipped. Side by side gives both their own column.
    const float32 width = GetLayoutWidth();
    const float32 height = GetLayoutHeight();
    if (width <= 1.0f || height <= 1.0f)
        return;
    const bool horizontal = width > height;
    if (horizontal == m_SplitIsHorizontal)
        return;
    m_SplitIsHorizontal = horizontal;

    if (horizontal)
    {
        m_SplitBody->AddClass("horizontal");
        m_Splitter->RemoveClass("col");
        m_Splitter->AddClass("row");
    }
    else
    {
        m_SplitBody->RemoveClass("horizontal");
        m_Splitter->RemoveClass("row");
        m_Splitter->AddClass("col");
    }
}

void TilePalettePanel::GateEmptySections()
{
    // Terrain painting needs terrains in the tileset; IntGrid painting needs
    // values on the layer. Without them the section is a notice plus a dead
    // control, so it is hidden rather than explained.
    if (m_TerrainSection != nullptr)
    {
        const bool usable = m_TerrainList != nullptr && m_TerrainList->HasTerrains();
        if (usable)
            m_TerrainSection->RemoveClass("hidden");
        else
            m_TerrainSection->AddClass("hidden");
    }
    if (m_IntGridSection != nullptr)
    {
        const bool usable = m_IntGridList != nullptr && m_IntGridList->HasValues();
        if (usable)
            m_IntGridSection->RemoveClass("hidden");
        else
            m_IntGridSection->AddClass("hidden");
    }
    // Brushes need a tileset to belong to, nothing more: an empty list with the
    // Save button is how the first brush gets made.
    for (UIElement* section : {m_BrushSection, m_ReplaceSection, m_ApplyTileSetAllRow})
    {
        if (section == nullptr)
            continue;
        if (m_LoadedTileSet.IsNull())
            section->AddClass("hidden");
        else
            section->RemoveClass("hidden");
    }
}

void TilePalettePanel::AdoptTargetLayerTileSet()
{
    if (m_EditContext == nullptr || !m_EditContext->HasTargetLayer())
        return;
    auto* store = TileMapECS::TileMapRuntimeStore::TryGet();
    if (store == nullptr)
        return;
    TileMap::TileMapData* data = store->GetMap(m_EditContext->TargetMap());
    if (data == nullptr)
        return;
    const TileMap::TileMapLayerData* layer = data->FindLayer(m_EditContext->TargetLayer());
    if (layer == nullptr || layer->TileSetGuid.IsNull())
    {
        // A layer with no atlas of its own -- a freshly added one -- takes the
        // palette's, so adding a layer mid-session keeps painting where it was.
        ApplyTileSetToTargetLayer();
        return;
    }
    if (layer->TileSetGuid == m_EditContext->TileSet())
        return;

    // A painted layer is the authority on its own atlas: its cell ids index that
    // one. Retargeting pulls, so selecting a layer shows the tiles it is made of
    // instead of demanding you re-pick the tileset it already knows.
    const GUID adopted = layer->TileSetGuid;
    m_EditContext->SetTileSet(adopted);
    if (m_TileSetField != nullptr)
        m_TileSetField->SetValue(adopted);
    ReloadTileSet(adopted);
}

void TilePalettePanel::ApplyTileSetToTargetLayer()
{
    if (m_EditContext == nullptr || !m_EditContext->HasTargetLayer())
        return;
    auto* store = TileMapECS::TileMapRuntimeStore::TryGet();
    if (store == nullptr)
        return;
    TileMap::TileMapData* data = store->GetMap(m_EditContext->TargetMap());
    if (data == nullptr)
        return;
    // An empty palette means "nothing chosen", never "unbind this layer".
    // Writing the null through would leave every cell the layer has already
    // painted pointing at an atlas that no longer resolves.
    if (m_EditContext->TileSet().IsNull())
        return;
    TileMap::TileMapLayerData* layer = data->FindLayer(m_EditContext->TargetLayer());
    if (layer == nullptr || layer->TileSetGuid == m_EditContext->TileSet())
        return;
    // The layer's whole cell set now resolves through a different atlas, so the
    // change is worth an undo entry like every other layer edit.
    ECS::World* world = m_Context != nullptr ? m_Context->World : nullptr;
    Editor::UndoRedoService* undo = m_Context != nullptr ? m_Context->UndoRedo : nullptr;
    const GUID mapGuid = m_EditContext->TargetMap();
    if (world != nullptr && undo != nullptr)
    {
        const Editor::TileLayerEdits::LayerStateSnapshot before =
            Editor::TileLayerEdits::CaptureLayerState(*world, *store, mapGuid);
        if (Editor::TileLayerEdits::SetLayersTileSet(*store, mapGuid, {layer->Id},
                                                     m_EditContext->TileSet()))
        {
            undo->CommitAlreadyApplied(std::make_unique<Editor::TileLayerStateCommand>(
                "Assign Tile Set", before,
                Editor::TileLayerEdits::CaptureLayerState(*world, *store, mapGuid)));
        }
        return;
    }
    layer->TileSetGuid = m_EditContext->TileSet();
    store->MarkStructuralChange(mapGuid);
}

void TilePalettePanel::ApplyTileSetToAllLayers()
{
    if (m_EditContext == nullptr || m_EditContext->TileSet().IsNull())
        return;
    auto* store = TileMapECS::TileMapRuntimeStore::TryGet();
    ECS::World* world = m_Context != nullptr ? m_Context->World : nullptr;
    Editor::UndoRedoService* undo = m_Context != nullptr ? m_Context->UndoRedo : nullptr;
    const GUID mapGuid = m_EditContext->TargetMap();
    const TileMap::TileMapData* map = (store != nullptr && !mapGuid.IsNull()) ? store->GetMap(mapGuid) : nullptr;
    if (map == nullptr || world == nullptr)
    {
        SetHint("Select a tilemap entity first; the tileset applies to its map.");
        return;
    }
    std::vector<uint32> layers;
    for (const TileMap::TileMapLayerData& layer : map->Layers())
        layers.push_back(layer.Id);
    const Editor::TileLayerEdits::LayerStateSnapshot before =
        Editor::TileLayerEdits::CaptureLayerState(*world, *store, mapGuid);
    if (!Editor::TileLayerEdits::SetLayersTileSet(*store, mapGuid, layers, m_EditContext->TileSet()))
    {
        SetHint("Every tile layer already uses this tileset.");
        return;
    }
    if (undo != nullptr)
    {
        undo->CommitAlreadyApplied(std::make_unique<Editor::TileLayerStateCommand>(
            "Swap Tile Set", before,
            Editor::TileLayerEdits::CaptureLayerState(*world, *store, mapGuid)));
    }
    SetHint("Every tile layer now uses this tileset — Ctrl+Z restores them.");
}

void TilePalettePanel::OnTileSetChanged(const GUID& tileSet)
{
    if (m_EditContext != nullptr)
    {
        m_EditContext->SetTileSet(tileSet);
        m_SeenEditVersion = m_EditContext->Version();
    }
    ReloadTileSet(tileSet);
    ApplyTileSetToTargetLayer();
    SyncFromEditContext();
}

void TilePalettePanel::ReloadTileSet(const GUID& tileSet)
{
    m_LoadedTileSet = tileSet;
    if (m_Grid == nullptr)
        return;

    // The old atlas stays on screen until the new one is ready. Clearing up
    // front meant every layer switch drew twice -- grid empties, empty-state
    // label appears, then the new tiles land -- instead of changing once.
    auto clearGrid = [this]() {
        m_Grid->SetTileSet(GUID::Null(), 0, 0, 1, 1, {});
        ShowGridOrEmptyState(false);
        if (m_TerrainList != nullptr)
            m_TerrainList->SetTerrainSets({});
        m_Brushes.clear();
        if (m_BrushList != nullptr)
        {
            m_BrushList->SetAtlas(GUID::Null(), {});
            m_BrushList->SetBrushes({});
        }
        GateEmptySections();
    };

    if (tileSet.IsNull() || m_Context == nullptr || m_Context->Assets == nullptr)
    {
        clearGrid();
        return;
    }

    AssetMetadata metadata{};
    if (!m_Context->Assets->GetRegistry().TryGetAssetMetadata(tileSet, metadata) ||
        metadata.Path.empty())
    {
        SetHint("Tileset is not in the asset registry yet.");
        clearGrid();
        return;
    }

    TileMap::TileSetAsset asset(tileSet, metadata.Path);
    if (!asset.Load())
    {
        Logger::Log::Warning("TilePalettePanel: tileset {} failed to load", tileSet.ToString());
        SetHint("Tileset failed to load.");
        clearGrid();
        return;
    }

    const int32 columns = asset.Columns();
    const int32 rows = asset.Rows();
    if (columns <= 0 || rows <= 0 || asset.Tiles().empty())
    {
        SetHint("Tileset has no sliced tiles — set the tile size in the tileset inspector.");
        clearGrid();
        return;
    }

    // Grid slices assign ids row-major and 1-based, so (id - 1) is the cell index.
    std::vector<TilePaletteGrid::Cell> cells(static_cast<size_t>(columns) *
                                             static_cast<size_t>(rows));
    for (const TileMap::TileSetTile& tile : asset.Tiles())
    {
        if (tile.Id == 0)
            continue;
        const size_t index = static_cast<size_t>(tile.Id - 1);
        if (index >= cells.size())
            continue;
        cells[index] = TilePaletteGrid::Cell{tile.Id, tile.UV.U0, tile.UV.V0, tile.UV.U1,
                                             tile.UV.V1};
    }

    m_Grid->SetTileSet(asset.AtlasTexture(), columns, rows, asset.TileWidthPx(),
                       asset.TileHeightPx(), std::move(cells));
    ShowGridOrEmptyState(true);
    if (m_TerrainList != nullptr)
        m_TerrainList->SetTerrainSets(asset.TerrainSets());
    m_Brushes = asset.Brushes();
    if (m_BrushList != nullptr)
    {
        std::vector<TileMap::TileUVRect> uvs(static_cast<size_t>(columns) * static_cast<size_t>(rows));
        for (const TileMap::TileSetTile& tile : asset.Tiles())
        {
            if (tile.Id != 0 && tile.Id <= uvs.size())
                uvs[tile.Id - 1] = tile.UV;
        }
        m_BrushList->SetAtlas(asset.AtlasTexture(), std::move(uvs));
        m_BrushList->SetBrushes(m_Brushes);
    }
    GateEmptySections();
}

void TilePalettePanel::OnGridSelectionChanged(int32 column0, int32 row0, int32 column1, int32 row1)
{
    if (m_EditContext == nullptr || m_Grid == nullptr)
        return;

    Editor::TileStamp stamp;
    stamp.Width = column1 - column0 + 1;
    stamp.Height = row1 - row0 + 1;
    stamp.Cells.reserve(static_cast<size_t>(stamp.Width) * static_cast<size_t>(stamp.Height));
    // Row-major from the top-left — the same order the palette lays the tiles out.
    for (int32 row = row0; row <= row1; ++row)
    {
        for (int32 column = column0; column <= column1; ++column)
        {
            // A cell the group left out stays empty in the stamp: StampAt skips
            // empty stamp cells, so the group paints without a hole punched
            // around it.
            stamp.Cells.push_back(m_Grid->IsPicked(column, row)
                                      ? TileMap::MakeCell(m_Grid->TileIdAt(column, row))
                                      : TileMap::kEmptyCell);
        }
    }

    m_EditContext->SetStamp(std::move(stamp));
    m_SeenEditVersion = m_EditContext->Version();
    SyncFromEditContext();
}

void TilePalettePanel::SyncFromEditContext()
{
    if (m_EditContext == nullptr)
        return;

    for (const auto& [tool, button] : m_ToolButtons)
    {
        if (button == nullptr)
            continue;
        const bool armed = m_EditContext->Tool() == tool;
        if (armed == button->HasClass("selected"))
            continue;
        if (armed)
            button->AddClass("selected");
        else
            button->RemoveClass("selected");
    }

    if (m_ScatterToggle != nullptr &&
        m_ScatterToggle->IsChecked() != m_EditContext->RandomScatter())
    {
        m_ScatterToggle->SetValueWithoutNotify(m_EditContext->RandomScatter());
    }

    if (m_TerrainList != nullptr)
        m_TerrainList->SetSelection(m_EditContext->TerrainSet(), m_EditContext->Terrain());

    if (m_IntGridList != nullptr)
        m_IntGridList->SetSelection(m_EditContext->IntGridValue());
    if (m_IntGridTintToggle != nullptr &&
        m_IntGridTintToggle->IsChecked() != m_EditContext->ShowIntGridTint())
    {
        m_IntGridTintToggle->SetValueWithoutNotify(m_EditContext->ShowIntGridTint());
    }

    // The stamp can be replaced from outside the palette (the Picker tool), and a
    // flip/rotate rewrites it in place; neither maps back to an atlas rectangle,
    // so the highlight only survives while the stamp is still the one picked here.
    if (m_Grid != nullptr && m_EditContext->Stamp().IsEmpty())
        m_Grid->ClearSelection();

    if (m_BrushList != nullptr)
    {
        const Editor::TileStamp& stamp = m_EditContext->Stamp();
        size_t armed = m_Brushes.size();
        for (size_t i = 0; i < m_Brushes.size(); ++i)
        {
            const TileMap::TileBrushDef& brush = m_Brushes[i];
            if (brush.Width == stamp.Width && brush.Height == stamp.Height &&
                brush.Cells == stamp.Cells)
            {
                armed = i;
                break;
            }
        }
        m_BrushList->SetSelection(armed);
    }

    if (m_EditContext->TileSet().IsNull())
        SetHint("Assign a tileset to start painting.");
    else if (m_Grid != nullptr && !m_Grid->HasSelection())
        SetHint("Drag across the palette to pick the tiles a stroke stamps.");
    else if (!m_EditContext->HasTargetLayer())
        SetHint("Select a tilemap entity and activate a layer to paint into.");
    else if (!m_EditContext->HasPaintTarget())
        SetHint("No entity in the scene draws this layer, so it cannot be painted.");
    else
        SetHint(std::string());
}

void TilePalettePanel::ShowGridOrEmptyState(bool hasTiles)
{
    if (m_Grid != nullptr)
    {
        if (hasTiles)
            m_Grid->RemoveClass("hidden");
        else
            m_Grid->AddClass("hidden");
    }
    if (m_GridBar != nullptr)
    {
        if (hasTiles)
            m_GridBar->RemoveClass("hidden");
        else
            m_GridBar->AddClass("hidden");
    }
    if (m_EmptyStateLabel != nullptr)
    {
        if (hasTiles)
            m_EmptyStateLabel->AddClass("hidden");
        else
            m_EmptyStateLabel->RemoveClass("hidden");
    }
}

void TilePalettePanel::RefreshGridStatus()
{
    if (m_Grid == nullptr || m_GridStatusLabel == nullptr)
        return;

    std::string text;
    if (m_Grid->HasHover())
    {
        const int32 column = m_Grid->HoverColumn();
        const int32 row = m_Grid->HoverRow();
        const uint32 id = m_Grid->TileIdAt(column, row);
        text = id != 0 ? "Tile " + std::to_string(id) : std::string("Empty");
        text += "  |  column " + std::to_string(column) + ", row " + std::to_string(row);
        text += "  |  ";
    }
    char scale[16];
    std::snprintf(scale, sizeof(scale), "%.1fx", static_cast<double>(m_Grid->AtlasScale()));
    text += scale;

    if (text == m_GridStatus)
        return;
    m_GridStatus = text;
    m_GridStatusLabel->SetText(text);
}

void TilePalettePanel::SetHint(const std::string& text)
{
    if (m_HintLabel == nullptr || text == m_Hint)
        return;
    m_Hint = text;
    m_HintLabel->SetText(text);
    if (text.empty())
        m_HintLabel->AddClass("hidden");
    else
        m_HintLabel->RemoveClass("hidden");
}

namespace
{
constexpr uint32 kCmdBrushDelete = 0x7201;
constexpr const char* kBrushEditedHint = "Saved the stamp as a brush in the tileset.";
} // namespace

std::unique_ptr<UIElement> TilePalettePanel::BuildBrushSection()
{
    auto section = std::make_unique<UIElement>();
    section->AddClass("tp-terrain-section");
    section->AddClass("hidden");
    m_BrushSection = section.get();

    auto header = std::make_unique<UIElement>();
    header->AddClass("tp-row");
    header->AddChild(MakeLabel("tp-row-label", "Brushes"));
    auto save = std::make_unique<Button>();
    save->SetId("TilePaletteSaveBrush");
    save->SetText("Save Brush");
    save->AddClass("small");
    save->AddClass("secondary");
    save->SetTooltip("Store the current stamp in the tileset, so it can be armed with one click");
    save->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { SaveStampAsBrush(); });
    header->AddChild(std::move(save));
    section->AddChild(std::move(header));

    auto list = std::make_unique<TilePaletteBrushList>();
    list->SetOnBrushChosen([this](size_t index) { ChooseBrush(index); });
    list->SetOnBrushMenuRequested([this](size_t index, int x, int y) { ShowBrushMenu(index, x, y); });
    m_BrushList = list.get();
    section->AddChild(std::move(list));
    return section;
}

void TilePalettePanel::ChooseBrush(size_t index)
{
    if (m_EditContext == nullptr || index >= m_Brushes.size())
        return;
    const TileMap::TileBrushDef& brush = m_Brushes[index];
    Editor::TileStamp stamp;
    stamp.Width = brush.Width;
    stamp.Height = brush.Height;
    stamp.Cells = brush.Cells;
    m_EditContext->SetStamp(std::move(stamp));
    // A brush is a stamp, so it paints with the brush tool; arming it from the
    // marquee or eraser would silently do nothing.
    m_EditContext->SetTool(Editor::TileMapTool::Brush);
    m_SeenEditVersion = m_EditContext->Version();
    if (m_Grid != nullptr)
        m_Grid->ClearSelection();
    if (m_OnToolActivated)
        m_OnToolActivated();
    SyncFromEditContext();
}

void TilePalettePanel::SaveStampAsBrush()
{
    if (m_EditContext == nullptr || m_Context == nullptr || m_Context->Assets == nullptr)
        return;
    const Editor::TileStamp& stamp = m_EditContext->Stamp();
    if (stamp.IsEmpty())
    {
        SetHint("Pick tiles in the atlas first; the brush is the current stamp.");
        return;
    }
    if (m_LoadedTileSet.IsNull())
        return;
    AssetMetadata metadata{};
    if (!m_Context->Assets->GetRegistry().TryGetAssetMetadata(m_LoadedTileSet, metadata) ||
        metadata.Path.empty())
    {
        SetHint("Tileset is not in the asset registry yet.");
        return;
    }

    Editor::TileSetDocument document;
    if (document.OpenOrCreate(m_LoadedTileSet, metadata.Path) !=
            Editor::TileSetDocument::OpenOutcome::Opened ||
        document.Asset() == nullptr)
    {
        SetHint("Tileset failed to load; the brush was not saved.");
        return;
    }
    TileMap::TileBrushDef brush;
    brush.Name = "Brush " + std::to_string(document.Asset()->Brushes().size() + 1);
    brush.Width = stamp.Width;
    brush.Height = stamp.Height;
    brush.Cells = stamp.Cells;
    document.Asset()->Brushes().push_back(brush);
    if (document.Save() != Editor::TileSetDocument::SaveOutcome::Written)
    {
        SetHint("Failed to write the tileset; the brush was not saved.");
        return;
    }
    m_Brushes = document.Asset()->Brushes();
    if (m_BrushList != nullptr)
        m_BrushList->SetBrushes(m_Brushes);
    SyncFromEditContext();
    SetHint(kBrushEditedHint);
}

void TilePalettePanel::DeleteBrush(size_t index)
{
    if (m_Context == nullptr || m_Context->Assets == nullptr || m_LoadedTileSet.IsNull())
        return;
    AssetMetadata metadata{};
    if (!m_Context->Assets->GetRegistry().TryGetAssetMetadata(m_LoadedTileSet, metadata) ||
        metadata.Path.empty())
        return;
    Editor::TileSetDocument document;
    if (document.OpenOrCreate(m_LoadedTileSet, metadata.Path) !=
            Editor::TileSetDocument::OpenOutcome::Opened ||
        document.Asset() == nullptr)
        return;
    std::vector<TileMap::TileBrushDef>& brushes = document.Asset()->Brushes();
    if (index >= brushes.size())
        return;
    const std::string name = brushes[index].Name;
    brushes.erase(brushes.begin() + static_cast<std::ptrdiff_t>(index));
    if (document.Save() != Editor::TileSetDocument::SaveOutcome::Written)
    {
        SetHint("Failed to write the tileset; the brush was not deleted.");
        return;
    }
    m_Brushes = brushes;
    if (m_BrushList != nullptr)
        m_BrushList->SetBrushes(m_Brushes);
    SyncFromEditContext();
    SetHint("Deleted brush \"" + name + "\".");
}

void TilePalettePanel::ShowBrushMenu(size_t index, int x, int y)
{
    if (m_Context == nullptr || m_Context->MainWindow == nullptr || index >= m_Brushes.size())
        return;
    if (!m_BrushMenu)
    {
        m_BrushMenu = CreateContextMenu();
        if (!m_BrushMenu)
            return;
        m_BrushMenu->SetCommandHandler([this](uint32_t command) {
            if (command == kCmdBrushDelete)
                DeleteBrush(m_BrushMenuIndex);
        });
    }
    m_BrushMenuIndex = index;
    m_BrushMenu->Clear();
    ContextMenuBuilder builder;
    builder.AddItem("Delete Brush", kCmdBrushDelete, MenuItemFlag_None, 0, EditorIcons::kTrash);
    builder.Build(m_BrushMenu.get(), /*sortByPriorityThenPath*/ false);
    m_BrushMenu->Show(m_Context->MainWindow, x, y);
}

std::unique_ptr<UIElement> TilePalettePanel::BuildReplaceSection()
{
    auto section = std::make_unique<UIElement>();
    section->AddClass("tp-terrain-section");
    section->AddClass("hidden");
    m_ReplaceSection = section.get();

    // One row: "Replace <from> with <to>" and where to do it. Ids are the
    // palette's 1-based tile ids; 0 as the target clears the cells instead.
    auto row = std::make_unique<UIElement>();
    row->AddClass("tp-row");
    row->AddChild(MakeLabel("tp-row-label", "Replace tile"));
    auto from = std::make_unique<IntField>();
    from->SetId("TilePaletteReplaceFrom");
    from->AddClass("tp-replace-field");
    from->SetRange(1, static_cast<int>(TileMap::kMaxTileId));
    from->SetValueWithoutNotify(1);
    from->SetTooltip("Tile id to find (1-based, as the atlas numbers them)");
    m_ReplaceFromField = from.get();
    row->AddChild(std::move(from));
    row->AddChild(MakeLabel("tp-row-label", "with"));
    auto to = std::make_unique<IntField>();
    to->SetId("TilePaletteReplaceTo");
    to->AddClass("tp-replace-field");
    to->SetRange(0, static_cast<int>(TileMap::kMaxTileId));
    to->SetValueWithoutNotify(1);
    to->SetTooltip("Tile id to write; 0 clears the matching cells");
    m_ReplaceToField = to.get();
    row->AddChild(std::move(to));

    auto inLayer = std::make_unique<Button>();
    inLayer->SetId("TilePaletteReplaceInLayer");
    inLayer->SetText("In Layer");
    inLayer->AddClass("small");
    inLayer->AddClass("secondary");
    inLayer->SetTooltip("Replace every matching cell of the target layer");
    inLayer->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { ReplaceTile(false); });
    row->AddChild(std::move(inLayer));
    auto inMap = std::make_unique<Button>();
    inMap->SetId("TilePaletteReplaceInMap");
    inMap->SetText("In Map");
    inMap->AddClass("small");
    inMap->AddClass("secondary");
    inMap->SetTooltip("Replace every matching cell on every unlocked layer of the map");
    inMap->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { ReplaceTile(true); });
    row->AddChild(std::move(inMap));
    section->AddChild(std::move(row));
    return section;
}

void TilePalettePanel::ReplaceTile(bool wholeMap)
{
    if (m_EditContext == nullptr || m_ReplaceFromField == nullptr || m_ReplaceToField == nullptr)
        return;
    auto* store = TileMapECS::TileMapRuntimeStore::TryGet();
    Editor::UndoRedoService* undo = m_Context != nullptr ? m_Context->UndoRedo : nullptr;
    const GUID mapGuid = m_EditContext->TargetMap();
    if (store == nullptr || mapGuid.IsNull())
    {
        SetHint("Select a tilemap entity first; the replacement runs on its map.");
        return;
    }
    const TileMap::TileMapData* map = store->GetMap(mapGuid);
    if (map == nullptr)
        return;

    std::vector<uint32> layers;
    if (wholeMap)
    {
        for (const TileMap::TileMapLayerData& layer : map->Layers())
            layers.push_back(layer.Id);
    }
    else if (m_EditContext->TargetLayer() != 0)
    {
        layers.push_back(m_EditContext->TargetLayer());
    }
    if (layers.empty())
    {
        SetHint("Activate a layer to replace in, or use In Map.");
        return;
    }

    const uint32 fromTile = static_cast<uint32>(std::max(0, m_ReplaceFromField->GetValue()));
    const uint32 toTile = static_cast<uint32>(std::max(0, m_ReplaceToField->GetValue()));
    size_t cells = 0;
    std::unique_ptr<Editor::TileMapEditCommand> command =
        Editor::ReplaceTileEverywhere(*store, mapGuid, layers, fromTile, toTile, cells);
    if (!command)
    {
        SetHint("No cell holds tile " + std::to_string(fromTile) +
                (wholeMap ? " on any unlocked layer." : " on the target layer."));
        return;
    }
    if (undo != nullptr)
        undo->CommitAlreadyApplied(std::move(command));
    SetHint("Replaced " + std::to_string(cells) + " cell(s) of tile " + std::to_string(fromTile) +
            (toTile == 0 ? " with nothing." : " with tile " + std::to_string(toTile) + ".") +
            (undo != nullptr ? " Ctrl+Z restores them." : ""));
}

std::unique_ptr<UIElement> TilePalettePanel::BuildLayerSection()
{
    auto list = std::make_unique<TilePaletteLayerList>();
    list->SetOnLayerTargeted([this]() { OnLayerTargeted(); });
    list->SetOnLayerAdded([this]() { ApplyTileSetToTargetLayer(); });
    list->SetOnCreateMapRequested([this]() { (void)CreateTileMapAsset(); });
    list->SetOnHint([this](const std::string& text) { SetHint(text); });
    m_LayerList = list.get();
    return list;
}

void TilePalettePanel::SetSceneToolArmed(bool armed)
{
    if (armed == m_SceneToolArmed || m_ToolRow == nullptr)
        return;
    m_SceneToolArmed = armed;
    if (armed)
        m_ToolRow->RemoveClass("disarmed");
    else
        m_ToolRow->AddClass("disarmed");
}

void TilePalettePanel::OnLayerTargeted()
{
    if (m_EditContext == nullptr)
        return;
    m_FollowedTargetMap = m_EditContext->TargetMap();
    m_FollowedTargetLayer = m_EditContext->TargetLayer();
    AdoptTargetLayerTileSet();
    // In the same frame as the atlas swap. Left to the next update, the IntGrid
    // notice changed a frame later and its height change shoved the atlas and
    // everything under it, which reads as the text jumping before the panel
    // settles.
    RefreshIntGridValues();
    m_SeenEditVersion = m_EditContext->Version();
    SyncFromEditContext();
}

void TilePalettePanel::Update()
{
    if (m_EditContext == nullptr)
        return;

    const uint64 version = m_EditContext->Version();
    if (version != m_SeenEditVersion)
    {
        m_SeenEditVersion = version;
        if (m_EditContext->TileSet() != m_LoadedTileSet)
        {
            if (m_TileSetField != nullptr)
                m_TileSetField->SetValue(m_EditContext->TileSet());
            ReloadTileSet(m_EditContext->TileSet());
        }
        if (m_EditContext->TargetMap() != m_FollowedTargetMap ||
            m_EditContext->TargetLayer() != m_FollowedTargetLayer)
        {
            m_FollowedTargetMap = m_EditContext->TargetMap();
            m_FollowedTargetLayer = m_EditContext->TargetLayer();
            OnLayerTargeted();
        }
        SyncFromEditContext();
    }

    ApplySplitOrientation();
    if (m_LayerList != nullptr)
        m_LayerList->Update();
    RefreshIntGridValues();
    RefreshGridStatus();
}

} // namespace GameEngine
