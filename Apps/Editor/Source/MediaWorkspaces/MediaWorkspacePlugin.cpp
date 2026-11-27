#include "Editor/MediaWorkspaces/MediaWorkspacePlugin.h"

#include "MediaWorkspacePanel.h"

#include "Assets/AssetRegistry.h"

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
    MediaWorkspacePlugin(const char* id, const char* title)
        : m_Descriptor{id, title, "0.1.0", true}
    {
    }

    const Plugins::PluginDescriptor& GetDescriptor() const override { return m_Descriptor; }

    void OnEditorAttach(const EditorHostServices& host) override
    {
        m_NativeHostForPanel = host.NativeHostForPanel;
        RegisterPanel();
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

    bool HasCloseConfirmation() const override
    {
        return std::any_of(m_Panels.begin(), m_Panels.end(), [](const auto& reference) {
            const auto* panel = reference.Get();
            return panel && panel->HasCloseConfirmation();
        });
    }

    void ConfirmClose(std::function<void(bool)> done) override
    {
        for (auto it = m_Panels.rbegin(); it != m_Panels.rend(); ++it)
        {
            if (!it->Get() || !it->Get()->HasCloseConfirmation())
                continue;
            done = [reference = *it, next = std::move(done)](bool approved) {
                if (auto* panel = reference.Get(); approved && panel)
                    panel->ConfirmClose(next);
                else
                    next(approved);
            };
        }
        done(true);
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
    void RegisterPanel()
    {
        const std::string name = m_Descriptor.DisplayName;
        EditorPanelDescriptor panel;
        panel.PanelId = name;
        panel.Title = name;
        panel.AssetSourceAlias = kAssetSourceAliasEditor;
        panel.LayoutAssetPath = MediaWorkspacePanel::kLayoutAssetPath;
        panel.StyleAssetPath = MediaWorkspacePanel::kStyleAssetPath;
        panel.DefaultDockAnchorPanelId = "SceneView";
        panel.Factory = [this, name] {
            auto element = std::make_unique<MediaWorkspacePanel>(name, m_Descriptor.Id);
            m_Panels.push_back(UIElement::MakeWeakRef(element.get()));
            return element;
        };
        EditorPanelRegistry::Get().RegisterPanel(std::move(panel));
    }

    Plugins::PluginDescriptor m_Descriptor;
    std::vector<UIElement::WeakRef<MediaWorkspacePanel>> m_Panels;
    std::function<NativePanelHost(UIManager*)> m_NativeHostForPanel;
};

} // namespace

IEditorPlugin& GetMediaWorkspaceEditorPlugin(MediaWorkspaceKind kind)
{
    static MediaWorkspacePlugin editroom{"editroom", "Editroom"};
    static MediaWorkspacePlugin compositor{"compositor", "Compositor"};
    return kind == MediaWorkspaceKind::Editroom ? editroom : compositor;
}
}
