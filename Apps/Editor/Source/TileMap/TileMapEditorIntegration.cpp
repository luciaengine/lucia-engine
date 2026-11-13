#include "TileMap/TileMapEditorIntegration.h"

#include "Editor/Registries/EditorMenuRegistry.h"
#include "Editor/Settings/EditorSettingsRegistry.h"
#include "Editor/Registries/EditorPanelRegistry.h"
#include "Panels/ConfirmActionModal.h"
#include "Panels/TileRulesEditorPanel.h"
#include "Editor/Settings/TilePaletteSettings.h"
#include "EditorContext.h"
#include "EditorPanelManager.h"
#include "Panels/TilePalettePanel.h"
#include "Rendering/Sorting/RenderSortingEditor.h"
#include "Panels/TileSetEditorPanel.h"
#include "SceneView/TileMapPaintTool.h"
#include "SceneView/TileMapPaintToolStripEntry.h"
#include "SceneView/SceneViewController.h"
#include "TileMap/TileMapSourceImport.h"
#include "UI/UIManager.h"

#include "Logger/Logger.h"
#include "Mathematics/MatrixOps.h"

#include "Components/TileMap/TileMapLayerRef.h"

namespace GameEngine
{
namespace Editor
{

namespace
{
/** The Tile Palette's page in Settings, registered by the feature that owns it. */
void RegisterTilePaletteSettings()
{
    SettingsCategoryDescriptor category;
    category.CategoryId = "tilePalette";
    category.Title = "Tile Palette";
    category.Group = SettingsCategoryGroup::UserSettings;
    category.TreeRowClass = "tile-palette-row";
    category.SearchKeywords = "tile palette atlas tilemap tileset background";

    SettingsFieldDescriptor background;
    background.Label = "Atlas Background";
    background.Tooltip = "The color behind the tileset atlas in the Tile Palette.";
    background.SearchKeywords = "tile palette atlas background color colour";
    SettingsFieldDescriptor::ColorField colorField;
    colorField.DefaultArgb = TilePaletteSettings::kDefaultAtlasBackgroundArgb;
    colorField.Get = []() { return TilePaletteSettings::Get().GetAtlasBackgroundArgb(); };
    colorField.Set = [](uint32_t argb) { TilePaletteSettings::Get().SetAtlasBackgroundArgb(argb); };
    background.Control = colorField;
    category.Fields.push_back(std::move(background));

    EditorSettingsRegistry::Get().RegisterCategory(std::move(category));
}
void RegisterTileRulesEditor(const std::shared_ptr<TileMapEditContext>& editContext,
                             EditorPanelManager* panels)
{
    EditorPanelDescriptor descriptor;
    descriptor.PanelId = "TileRulesEditor";
    descriptor.Title = "Tile Rules";
    descriptor.TabIconClass = "dock-tile-rules-icon";
    descriptor.DefaultDockAnchorPanelId = "TilePalette";
    descriptor.Factory = [editContext, panels]() {
        return std::make_unique<TileRulesEditorPanel>(editContext, panels);
    };
    EditorPanelRegistry::Get().RegisterPanel(std::move(descriptor));
}
} // namespace

void TileMapEditorIntegration::Attach(const Services& services)
{
    m_Context = services.Context;
    m_Scene = services.Scene;
    m_UI = services.UI;
    RegisterTilePaletteSettings();
    RegisterTileRulesEditor(m_EditContext, services.Panels);
    SceneViewToolStripRegistry::Get().Register(MakeTileMapPaintToolStripEntry(m_EditContext));
    RegisterRenderSortingComponent(ECS::GetComponentTypeId<Components::TileMapLayerRef>());

    EditorAssetFileMenuItemDescriptor importItem;
    importItem.Path = "Import as Tilemap";
    importItem.Priority = -500;
    importItem.AppliesTo = [](const std::filesystem::path& file) { return IsTileMapSourceFile(file); };
    importItem.Action = [this](const std::filesystem::path& file) { ImportSource(file, /*overwrite*/ false); };
    EditorMenuRegistry::Get().RegisterAssetFileMenuItem(std::move(importItem));

    if (TileSetEditorPanel* panel = services.TileSetEditor)
    {
        if (services.Context)
            panel->SetContext(services.Context);
        if (services.Panels)
            services.Panels->RegisterUpdateCallback(
                panel, [panel](const Editor::PanelUpdateContext&) { panel->Update(); });
    }

    if (TilePalettePanel* panel = services.Palette)
    {
        if (services.Context)
            panel->SetContext(services.Context);
        panel->SetEditContext(m_EditContext.get());
        panel->SetUndoRedoService(services.UndoRedo);
        SceneViewController* scene = services.Scene;
        panel->SetOnToolActivated(
            [scene]()
            {
                if (scene)
                    scene->SetActiveRegisteredTool(kTileMapPaintToolId);
            });
        if (services.Panels)
        {
            // Registered as always-updating: the selection sync must run while
            // the palette is hidden, or the paint tool would follow a stale
            // target. The panel's own refresh keeps the manager's visibility
            // gate, applied here by the same layout-extent test it uses.
            services.Panels->RegisterUpdateCallback(
                panel,
                [this, panel](const Editor::PanelUpdateContext&)
                {
                    const bool visible = panel->GetLayoutWidth() > 0.0f && panel->GetLayoutHeight() > 0.0f;
                    SyncTargetToSelection(visible);
                    SyncGridToTarget();
                    // The tool row shows the edit context's tool; the Scene View
                    // decides which tool gets the pointer. The row dims while the
                    // two disagree, so an armed-looking brush never lies.
                    panel->SetSceneToolArmed(
                        m_Scene != nullptr &&
                        m_Scene->GetActiveRegisteredToolId() == kTileMapPaintToolId);
                    if (visible)
                        panel->Update();
                },
                /*alwaysUpdate=*/true);
        }
    }


}

ConfirmActionModal* TileMapEditorIntegration::EnsureModal(const char* id)
{
    UIElement* root = m_UI ? m_UI->GetRootElement() : nullptr;
    if (!root)
        return nullptr;
    // Resolve through the current root so a layout replacement cannot leave a stale pointer.
    if (UIElement* existing = root->FindById(id))
        return dynamic_cast<ConfirmActionModal*>(existing);
    auto created = std::make_unique<ConfirmActionModal>();
    auto* modal = created.get();
    modal->SetId(id);
    root->AddChild(std::move(created));
    return modal;
}

void TileMapEditorIntegration::ImportSource(const std::filesystem::path& source, bool overwrite)
{
    const std::u8string sourceText = source.filename().u8string();
    const std::string sourceName(reinterpret_cast<const char*>(sourceText.data()), sourceText.size());
    const auto fileName = [](const std::filesystem::path& path) {
        const std::u8string text = path.filename().u8string();
        return std::string(reinterpret_cast<const char*>(text.data()), text.size());
    };

    std::string error;
    TileMapImportSummary summary;
    const bool imported = ImportTileMapSource(source, overwrite, error, &summary);
    if (!imported && !summary.ExistingOutputs.empty())
    {
        ConfirmActionModal* confirm = EnsureModal("tilemap-import-confirm");
        if (!confirm)
        {
            // A confirmation that cannot be shown is a reason to stop, not to overwrite.
            Logger::Log::Error("Import as Tilemap: {}", error);
            return;
        }
        std::string files;
        for (const std::filesystem::path& path : summary.ExistingOutputs)
            files += "\n  " + fileName(path);
        m_PendingImport = source;
        confirm->SetOnConfirm([this]() { ImportSource(m_PendingImport, /*overwrite*/ true); });
        confirm->SetOnCancel([sourceName]() {
            Logger::Log::Info("Import as Tilemap: '{}' canceled; nothing was written", sourceName);
        });
        confirm->Show("Replace Imported Files?",
                      "Importing " + sourceName + " again replaces:" + files +
                          "\n\nPainting, layers and IntGrid values added to a .tilemap after its import are lost.",
                      "Replace");
        return;
    }

    if (!imported)
        Logger::Log::Error("Import as Tilemap failed: {}", error);
    ConfirmActionModal* notice = EnsureModal("tilemap-import-notice");
    if (!notice)
        return;
    notice->SetOnConfirm({});
    notice->SetOnCancel({});
    ConfirmActionModal::Notice text;
    text.DismissLabel = "OK";
    if (imported)
    {
        text.Title = "Tilemap Imported";
        text.Message = sourceName + " imported as " + std::to_string(summary.TileSetPaths.size()) + " tile set(s) and " +
                       std::to_string(summary.MapPaths.size()) + " tile map(s) beside it.";
    }
    else
    {
        text.Title = "Tilemap Import Failed";
        text.Message = error;
        text.Closing = summary.TileSetPaths.empty() && summary.MapPaths.empty()
                           ? "No native tile-map outputs were written."
                           : "Some outputs were written before the error. Review them before retrying.";
    }
    if (!summary.Warnings.empty())
    {
        text.Detail = std::to_string(summary.Warnings.size()) + " warning(s): ";
        for (size_t i = 0; i < summary.Warnings.size(); ++i)
            text.Detail += (i == 0 ? "" : "; ") + summary.Warnings[i];
    }
    notice->ShowNotice(text);
}

void TileMapEditorIntegration::SyncGridToTarget()
{
    if (m_Scene == nullptr)
        return;
    SceneTools::TileMapPaintTool* tool = GetTileMapPaintTool(*m_Scene);
    const SceneTools::TileMapPaintTarget target =
        tool != nullptr ? tool->ResolveTarget() : SceneTools::TileMapPaintTarget{};
    if (!target.Valid)
    {
        m_Scene->ClearGrid2DCells();
        return;
    }
    // The grid shader is axis-aligned, so only the map's origin carries over;
    // a rotated map keeps its cells but the lines stay upright.
    const Mathematics::Matrix4x4 localToWorld = Mathematics::Inverse(target.WorldToLocal);
    const auto& m = localToWorld.GetGLM();
    SceneViewController::Grid2DCells cells;
    cells.OriginX = m[3][0];
    cells.OriginY = m[3][1];
    cells.Width = target.CellWidth;
    cells.Height = target.CellHeight;
    cells.Valid = true;
    m_Scene->SetGrid2DCells(cells);
}

void TileMapEditorIntegration::SyncTargetToSelection(bool paletteVisible)
{
    if (m_Scene == nullptr)
        return;
    const std::vector<ECS::EntityHandle>& selected = m_Scene->GetSelectedEntities();
    const ECS::EntityHandle back = selected.empty() ? ECS::EntityHandle{} : selected.back();
    // A palette that just came to the front counts as a change too: the layer
    // selected while it was hidden should arm the brush now, not on the next click.
    if (m_SyncedOnce && selected.size() == m_SyncedSelectionCount &&
        back == m_SyncedSelectionBack && paletteVisible == m_SyncedPaletteVisible)
        return;
    m_SyncedOnce = true;
    m_SyncedSelectionCount = selected.size();
    m_SyncedSelectionBack = back;
    m_SyncedPaletteVisible = paletteVisible;

    ECS::World* world = m_Context != nullptr ? m_Context->World : nullptr;
    if (world == nullptr)
    {
        m_EditContext->ClearTarget();
        return;
    }

    // The most recently selected entity that draws a layer wins, so a
    // multi-selection still paints where the user clicked last.
    for (auto it = selected.rbegin(); it != selected.rend(); ++it)
    {
        const auto* layerRef = world->GetComponent<Components::TileMapLayerRef>(*it);
        if (layerRef == nullptr || layerRef->LayerId == 0 || layerRef->TileMap.IsNull())
            continue;
        m_EditContext->SetTarget(*it, layerRef->TileMap.ToGuid(), layerRef->LayerId);
        if (paletteVisible)
            m_Scene->SetActiveRegisteredTool(kTileMapPaintToolId);
        return;
    }
    m_EditContext->ClearTarget();
    // The user moved on to something that is not a tile layer: hand the Scene
    // View back to selection so the next click picks instead of painting.
    if (!selected.empty() &&
        m_Scene->GetActiveRegisteredToolId() == kTileMapPaintToolId)
    {
        m_Scene->SetActiveTool(SceneViewController::ToolKind::Selection);
    }
}

} // namespace Editor
} // namespace GameEngine
