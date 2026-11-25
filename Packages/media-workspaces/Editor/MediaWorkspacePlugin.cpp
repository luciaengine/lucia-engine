#include "MediaWorkspacePanel.h"

#include "Editor/Registries/EditorPanelRegistry.h"

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

namespace GameEngine::Editor
{
namespace
{
class MediaWorkspacePlugin final : public IEditorPlugin
{
  public:
    const Plugins::PluginDescriptor& GetDescriptor() const override
    {
        static const Plugins::PluginDescriptor descriptor{
            "media-workspaces", "Media Workspaces", "0.1.0", true};
        return descriptor;
    }

    void OnEditorAttach(const EditorHostServices& host) override
    {
        m_NativeHostForPanel = host.NativeHostForPanel;
        RegisterPanel("Editroom");
        RegisterPanel("Compositor");
    }

    void OnEditorUpdate() override
    {
        std::erase_if(m_Panels, [](const auto& panel) { return !panel.Get(); });
        for (const auto& reference : m_Panels)
        {
            if (auto* panel = reference.Get())
                panel->SyncHost(m_NativeHostForPanel ? m_NativeHostForPanel(panel->GetOwnerManager())
                                                       : NativePanelHost{});
        }
    }

    void OnEditorShutdown() override
    {
        for (const auto& reference : m_Panels)
            if (auto* panel = reference.Get())
                panel->SyncHost({});
        m_NativeHostForPanel = {};
        m_Panels.clear();
    }

  private:
    void RegisterPanel(const std::string& name)
    {
        EditorPanelDescriptor panel;
        panel.PanelId = name;
        panel.Title = name;
        panel.AssetSourceAlias = MediaWorkspacePanel::kAssetSourceAlias;
        panel.LayoutAssetPath = MediaWorkspacePanel::kLayoutAssetPath;
        panel.StyleAssetPath = MediaWorkspacePanel::kStyleAssetPath;
        panel.DefaultDockAnchorPanelId = "SceneView";
        panel.Factory = [this, name] {
            auto element = std::make_unique<MediaWorkspacePanel>(name);
            m_Panels.push_back(UIElement::MakeWeakRef(element.get()));
            return element;
        };
        EditorPanelRegistry::Get().RegisterPanel(std::move(panel));
    }

    std::vector<UIElement::WeakRef<MediaWorkspacePanel>> m_Panels;
    std::function<NativePanelHost(UIManager*)> m_NativeHostForPanel;
};

MediaWorkspacePlugin s_MediaWorkspacePlugin;
struct RegisterMediaWorkspacePlugin
{
    RegisterMediaWorkspacePlugin() { EditorPluginRegistry::Get().RegisterPlugin(s_MediaWorkspacePlugin); }
};
RegisterMediaWorkspacePlugin s_RegisterMediaWorkspacePlugin;
}
}
