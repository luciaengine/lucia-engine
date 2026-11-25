#include "MediaWorkspacePanel.h"

#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Core/Engine.h"
#include "Logger/Logger.h"
#include "UI/Controls/Label.h"

#include <filesystem>
#include <utility>

#if defined(__APPLE__)
#include <dlfcn.h>
#endif

namespace GameEngine::Editor
{
struct MediaWorkspacePanel::Workspace
{
    using Create = void* (*)(const char*);
    using Place = void (*)(void*, void*, double, double, double, double);
    using Destroy = void (*)(void*);

    void* Image = nullptr;
    void* View = nullptr;
    Place PlaceView = nullptr;
    Destroy DestroyView = nullptr;
    void* HostedWindow = nullptr;

    ~Workspace()
    {
        if (View)
            DestroyView(View);
#if defined(__APPLE__)
        if (Image)
            dlclose(Image);
#endif
    }
};

MediaWorkspacePanel::MediaWorkspacePanel(std::string name)
    : AssetBoundDockPanel(name, kAssetSourceAlias, kLayoutAssetPath, kStyleAssetPath),
      m_Name(std::move(name))
{
}

MediaWorkspacePanel::~MediaWorkspacePanel() = default;

void MediaWorkspacePanel::SetStatus(const std::string& status)
{
    if (auto* label = dynamic_cast<Label*>(FindById(HashStringId("WorkspaceStatus"))))
        label->SetText(status);
}

void MediaWorkspacePanel::LoadWorkspace()
{
    m_LoadAttempted = true;
#if defined(__APPLE__)
    if (m_Name == "Compositor")
    {
        if (__builtin_available(macOS 26.0, *)) {}
        else
        {
            SetStatus("Compositor requires macOS 26 or later.");
            return;
        }
    }
    auto* assets = EngineCore::GetInstance().TryGetAssetManager();
    const std::filesystem::path root = assets ? assets->GetRegistry().GetSourceRoot(kAssetSourceAlias)
                                              : std::filesystem::path{};
    if (root.empty())
    {
        SetStatus("The Media Workspaces package is unavailable.");
        return;
    }

    const auto bundle = root.parent_path() / "Host" / (m_Name + ".app");
    const auto executable = bundle / "Contents" / "MacOS" / m_Name;
    auto workspace = std::make_unique<Workspace>();
    workspace->Image = dlopen(executable.c_str(), RTLD_NOW | RTLD_LOCAL | RTLD_NODELETE);
    if (!workspace->Image)
    {
        if (const char* detail = dlerror())
            Logger::Log::Warning("{} workspace: {}", m_Name, detail);
        SetStatus("This build needs the optional macOS Media Workspaces dependency.");
        return;
    }

    const auto create = reinterpret_cast<Workspace::Create>(dlsym(workspace->Image, "LuciaCreateMediaWorkspace"));
    workspace->PlaceView = reinterpret_cast<Workspace::Place>(dlsym(workspace->Image, "LuciaPlaceMediaWorkspace"));
    workspace->DestroyView = reinterpret_cast<Workspace::Destroy>(dlsym(workspace->Image, "LuciaDestroyMediaWorkspace"));
    if (!create || !workspace->PlaceView || !workspace->DestroyView)
    {
        SetStatus("The workspace host is incompatible with this Editor build.");
        return;
    }
    workspace->View = create(bundle.c_str());
    if (!workspace->View)
    {
        SetStatus("The workspace could not start.");
        return;
    }
    m_Workspace = std::move(workspace);
    SetStatus("");
#else
    SetStatus("This workspace requires macOS.");
#endif
}

void MediaWorkspacePanel::SyncHost(const NativePanelHost& host)
{
    if (m_Mounted && IsLayoutBound() && !m_LoadAttempted)
        LoadWorkspace();
    if (!m_Workspace)
        return;

    auto* bounds = FindById(HashStringId("WorkspaceHost"));
    if (!m_Mounted || !bounds || bounds->GetLayoutWidth() <= 0.0f || bounds->GetLayoutHeight() <= 0.0f)
    {
        m_Workspace->PlaceView(m_Workspace->View, nullptr, 0, 0, 0, 0);
        return;
    }
    const double scale = host.UiScale;
    m_Workspace->PlaceView(m_Workspace->View, host.Window,
                           bounds->GetLayoutX() * scale, bounds->GetLayoutY() * scale,
                           bounds->GetLayoutWidth() * scale, bounds->GetLayoutHeight() * scale);
    if (host.Window && m_Workspace->HostedWindow != host.Window)
    {
        m_Workspace->HostedWindow = host.Window;
        Logger::Log::Info("Editor mounted the {} native workspace.", m_Name);
    }
}

void MediaWorkspacePanel::OnMountVisibilityChanged(bool visible)
{
    m_Mounted = visible;
    if (!visible && m_Workspace)
        m_Workspace->PlaceView(m_Workspace->View, nullptr, 0, 0, 0, 0);
}
}
