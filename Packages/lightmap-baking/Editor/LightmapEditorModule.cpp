#include "Editor/Registries/EditorMenuRegistry.h"
#include "Editor/Registries/EditorPanelRegistry.h"
#include "Editor/Registries/EditorPluginRegistry.h"
#include "Panel/LightmapBakingPanel.h"
#include "Service/LightmapBakeService.h"
#include "UI/UIElement.h"

#include <algorithm>
#include <nlohmann/json.hpp>

namespace GameEngine::Editor
{
namespace
{
class LightmapEditorPlugin final : public IEditorPlugin
{
public:
    const Plugins::PluginDescriptor& GetDescriptor() const override
    {
        // The package resolver owns opt-in. This module is loaded only after
        // the project's package configuration enables lightmap-baking.
        static const Plugins::PluginDescriptor descriptor{
            "lightmap-baking", "Lightmap Baking", "0.1.0", true};
        return descriptor;
    }

    void OnEditorAttach(const EditorHostServices& host) override
    {
        m_Host = host;
        m_Service = std::make_shared<LightmapBakeService>(host.ChangeNotifications, host.CanWriteScene);
        for (auto* existing : m_Panels)
        {
            existing->SetServices(m_Service, m_Host.SelectedEntityIds, m_Host.CanWriteScene);
            existing->SetListedInPanelMenus(true);
            existing->SetEnabled(true);
        }
        EditorPanelDescriptor panel;
        panel.PanelId = "LightmapBaking";
        panel.Title = "Lighting Bake";
        panel.Factory = [this]() -> std::unique_ptr<UIElement> {
            if (!m_Service)
                return {};
            auto panel = std::make_unique<LightmapBakingPanel>();
            auto* raw = panel.get();
            panel->SetServices(m_Service, m_Host.SelectedEntityIds, m_Host.CanWriteScene);
            panel->OnDestroyed = [this, raw] { std::erase(m_Panels, raw); };
            m_Panels.push_back(raw);
            return panel;
        };
        EditorPanelRegistry::Get().RegisterPanel(std::move(panel));
        EditorMenuRegistry::Get().RegisterMenuItem({"Tools/Lighting Bake", 0, [this] {
            if (m_Service)
                EditorPanelRegistry::Get().OpenPanel("LightmapBaking");
        }});
    }

    void OnEditorUpdate() override
    {
        if (!m_Service)
            return;
        m_Service->Poll();
        for (auto* panel : m_Panels)
            panel->Update();
    }

    void OnSceneInvalidated() override
    {
        if (m_Service)
            m_Service->InvalidateScene();
    }

    void OnEditorShutdown() override
    {
        if (m_Service)
            m_Service->Shutdown();
        for (auto* panel : m_Panels)
        {
            panel->SetServices({}, {}, {});
            panel->SetListedInPanelMenus(false);
            panel->SetEnabled(false);
        }
        m_Service.reset();
        m_Host = {};
    }

    bool TryInvokeCommand(std::string_view method, const nlohmann::json& parameters,
                          nlohmann::json& result) override
    {
        if (!m_Service)
            return false;
        if (method == "bake_lightmaps")
            result = m_Service->Start(parameters);
        else if (method == "get_lightmap_bake")
            result = m_Service->Poll();
        else if (method == "cancel_lightmap_bake")
        {
            m_Service->Cancel();
            result = {{"ok", true}};
        }
        else
            return false;
        return true;
    }

private:
    EditorHostServices m_Host;
    std::shared_ptr<LightmapBakeService> m_Service;
    std::vector<LightmapBakingPanel*> m_Panels;
};

LightmapEditorPlugin s_Plugin;
struct Registrar
{
    Registrar() { EditorPluginRegistry::Get().RegisterPlugin(s_Plugin); }
} s_Registrar;
}
}
