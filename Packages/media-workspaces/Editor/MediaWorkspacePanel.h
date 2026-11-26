#pragma once

#include "Editor/Registries/EditorPluginRegistry.h"
#include "UI/AssetBoundDockPanel.h"

#include <memory>
#include <string>

namespace GameEngine::Editor
{
class MediaWorkspacePanel final : public AssetBoundDockPanel
{
  public:
    static constexpr const char* kAssetSourceAlias = "media-workspaces";
    static constexpr const char* kLayoutAssetPath = "Editor/UI/panels/MediaWorkspacePanel.uxml";
    static constexpr const char* kStyleAssetPath = "Editor/UI/panels/MediaWorkspacePanel.css";

    explicit MediaWorkspacePanel(std::string name);
    ~MediaWorkspacePanel() override;

    bool HasCloseConfirmation() const;
    void ConfirmClose(std::function<void(bool)> done);

    void SyncHost(const NativePanelHost& host);
    void OnMountVisibilityChanged(bool visible) override;

  private:
    struct Workspace;

    void SetStatus(const std::string& status);
    void LoadWorkspace();

    std::string m_Name;
    std::unique_ptr<Workspace> m_Workspace;
    bool m_Mounted = false;
    bool m_LoadAttempted = false;
};
}
