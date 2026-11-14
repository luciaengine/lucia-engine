// Editor-module entry: registers the Localization panel, with its icon, when the localization
// editor library loads (static init, the single-TU pattern the other package editor modules
// use). The panel opens from the Window menu and the toolbar's panel list, like every other
// panel. The library links the EditorSDK import lib, so the registrations land in the host's
// registries.

#include "LocalizationPanel.h"
#include "Editor/Registries/EditorPanelRegistry.h"

#include <memory>

namespace
{

std::unique_ptr<GameEngine::UIElement> MakeLocalizationPanel()
{
    return std::make_unique<GameEngine::Editor::LocalizationPanel>();
}

struct LocalizationEditorModuleRegistrar
{
    LocalizationEditorModuleRegistrar()
    {
        using GameEngine::Editor::LocalizationPanel;
        // The tab icon class lives in the editor's cascade: the dock tab and the panel menus
        // are outside the panel's own stylesheet.
        GameEngine::Editor::EditorPanelRegistry::Get().RegisterEditorStyleSheet(
            {LocalizationPanel::kAssetSourceAlias, LocalizationPanel::kChromeStyleAssetPath});
        GameEngine::Editor::EditorPanelDescriptor panel;
        panel.PanelId = LocalizationPanel::kPanelId;
        panel.Title = "Localization";
        panel.TabIconClass = LocalizationPanel::kTabIconClass;
        panel.AssetSourceAlias = LocalizationPanel::kAssetSourceAlias;
        panel.LayoutAssetPath = LocalizationPanel::kLayoutAssetPath;
        panel.StyleAssetPath = LocalizationPanel::kStyleAssetPath;
        // The key list and the entry need a wide, tall leaf: the panel first opens as a tab
        // beside the Scene View.
        panel.DefaultDockAnchorPanelId = "SceneView";
        panel.Factory = &MakeLocalizationPanel;
        GameEngine::Editor::EditorPanelRegistry::Get().RegisterPanel(std::move(panel));
    }
};

LocalizationEditorModuleRegistrar s_LocalizationEditorModuleRegistrar;

} // namespace
