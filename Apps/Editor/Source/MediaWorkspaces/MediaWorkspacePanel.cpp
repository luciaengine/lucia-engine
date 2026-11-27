#include "MediaWorkspacePanel.h"

#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Core/ApplicationPaths.h"
#include "Core/Engine.h"
#include "Engine/Rendering/RenderServices.h"
#include "Logger/Logger.h"
#include "Types/PathUtils.h"
#include "UI/Controls/Label.h"

#include <filesystem>
#include <utility>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__) || defined(__linux__)
#include <dlfcn.h>
#endif

namespace GameEngine::Editor
{
struct MediaWorkspacePanel::Workspace
{
    using Preview = void (*)(const char*, const uint8_t*, uint32_t, uint32_t, size_t);
    using Create = void* (*)(const char*, const char*, Preview);
    using Place = void (*)(void*, void*, double, double, double, double);
    using Destroy = void (*)(void*);
    using CloseReply = void (*)(void*, int32_t);
    using Confirm = void (*)(void*, void*, CloseReply);
    using Pump = void (*)();

    void* Image = nullptr;
    void* View = nullptr;
    Place PlaceView = nullptr;
    Destroy DestroyView = nullptr;
    Confirm ConfirmViewClose = nullptr;
    Pump PumpEvents = nullptr;
    void* HostedWindow = nullptr;

    static void UpdateTexturePreview(const char* path, const uint8_t* rgba, uint32_t width,
                                     uint32_t height, size_t size)
    {
        auto& engine = EngineCore::GetInstance();
        auto* assets = engine.TryGetAssetManager();
        auto* renderServices = engine.GetRenderServices();
        if (!path || !assets || !renderServices)
            return;

        const auto root = assets->GetRegistry().GetSourceRoot("project");
        const auto source = PathFromUtf8(path);
        auto relative = RelativePathUnderRoot(source, root);
        if (relative.empty() && !root.empty())
        {
            std::error_code error;
            const auto canonicalRoot = std::filesystem::canonical(root, error);
            if (!error)
                relative = RelativePathUnderRoot(source, canonicalRoot);
        }
        if (relative.empty())
            return;

        const auto guid = assets->ResolveAssetGuid(relative, "project");
        renderServices->Textures().QueueEditorTexturePreview(guid, width, height, {rgba, size});
    }

    bool Load(const std::filesystem::path& executable)
    {
#if defined(_WIN32)
        Image = LoadLibraryExW(executable.c_str(), nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!Image)
        {
            const auto error = GetLastError();
            Logger::Log::Warning("Media workspace {} failed to load (Windows error {}).",
                                 PathToUtf8(executable), error);
        }
#elif defined(__APPLE__) || defined(__linux__)
        Image = dlopen(executable.c_str(), RTLD_NOW | RTLD_LOCAL | RTLD_NODELETE);
        if (!Image)
        {
            const char* error = dlerror();
            Logger::Log::Warning("Media workspace {} failed to load: {}", PathToUtf8(executable),
                                 error ? error : "unknown error");
        }
#endif
        return Image != nullptr;
    }

    template <typename Function>
    Function Symbol(const char* name)
    {
#if defined(_WIN32)
        return reinterpret_cast<Function>(GetProcAddress(static_cast<HMODULE>(Image), name));
#elif defined(__APPLE__) || defined(__linux__)
        return reinterpret_cast<Function>(dlsym(Image, name));
#else
        return nullptr;
#endif
    }

    ~Workspace()
    {
        if (View)
            DestroyView(View);
#if defined(_WIN32)
        if (Image)
            FreeLibrary(static_cast<HMODULE>(Image));
#elif defined(__APPLE__) || defined(__linux__)
        if (Image)
            dlclose(Image);
#endif
    }
};

MediaWorkspacePanel::MediaWorkspacePanel(std::string name, std::string packageId)
    : AssetBoundDockPanel(name, std::string(kAssetSourceAliasEditor), kLayoutAssetPath, kStyleAssetPath),
      m_Name(std::move(name)), m_PackageId(std::move(packageId))
{
}

MediaWorkspacePanel::~MediaWorkspacePanel() = default;

bool MediaWorkspacePanel::HasCloseConfirmation() const
{
    return m_Workspace && m_Workspace->ConfirmViewClose;
}

void MediaWorkspacePanel::ConfirmClose(std::function<void(bool)> done)
{
    if (!HasCloseConfirmation())
    {
        done(true);
        return;
    }

    auto reply = std::make_unique<std::function<void(bool)>>(std::move(done));
    m_Workspace->ConfirmViewClose(m_Workspace->View, reply.release(), [](void* context, int32_t approved) {
        std::unique_ptr<std::function<void(bool)>> completion(static_cast<std::function<void(bool)>*>(context));
        (*completion)(approved != 0);
    });
}

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
#endif
    auto* assets = EngineCore::GetInstance().TryGetAssetManager();
    if (!assets)
    {
        SetStatus("The project assets are unavailable.");
        return;
    }

    const auto hostRoot = ApplicationPaths::GetExecutableDirectory() / "Packages" / m_PackageId / "Host";
#if defined(__APPLE__)
    const auto bundle = hostRoot / (m_Name + ".app");
    const auto executable = bundle / "Contents" / "MacOS" / m_Name;
#else
    const auto bundle = hostRoot / m_Name;
#if defined(_WIN32)
    const auto executable = bundle / "LuciaMediaWorkspace.dll";
#else
    const auto executable = bundle / "libLuciaMediaWorkspace.so";
#endif
#endif
    auto workspace = std::make_unique<Workspace>();
    if (!workspace->Load(executable))
    {
        SetStatus("This build does not include the " + m_Name + " workspace host.");
        return;
    }

    const auto create = workspace->Symbol<Workspace::Create>("LuciaCreateProjectMediaWorkspace");
    workspace->PlaceView = workspace->Symbol<Workspace::Place>("LuciaPlaceMediaWorkspace");
    workspace->DestroyView = workspace->Symbol<Workspace::Destroy>("LuciaDestroyMediaWorkspace");
    workspace->ConfirmViewClose = workspace->Symbol<Workspace::Confirm>("LuciaConfirmMediaWorkspaceClose");
    workspace->PumpEvents = workspace->Symbol<Workspace::Pump>("LuciaPumpMediaWorkspaceEvents");
    if (!create || !workspace->PlaceView || !workspace->DestroyView)
    {
        SetStatus("The workspace host is incompatible with this Editor build.");
        return;
    }
    const auto projectAssets = assets->GetRegistry().GetSourceRoot("project");
    workspace->View = create(PathToUtf8(bundle).c_str(), PathToUtf8(projectAssets).c_str(),
                             Workspace::UpdateTexturePreview);
    if (!workspace->View)
    {
        SetStatus("The workspace could not start.");
        return;
    }
#if defined(_WIN32)
    // Qt keeps process-wide state after the last dock closes.
    HMODULE pinned = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                       static_cast<LPCWSTR>(workspace->Image), &pinned);
#endif
    m_Workspace = std::move(workspace);
    SetStatus("");
}

void MediaWorkspacePanel::SyncHost(const NativePanelHost& host)
{
    if (m_Mounted && IsLayoutBound() && host.Window && !m_LoadAttempted)
        LoadWorkspace();
    if (!m_Workspace)
        return;

    auto* bounds = FindById(HashStringId("WorkspaceHost"));
    if (!m_Mounted || !bounds || bounds->GetLayoutWidth() <= 0.0f || bounds->GetLayoutHeight() <= 0.0f)
    {
        m_Workspace->PlaceView(m_Workspace->View, nullptr, 0, 0, 0, 0);
        if (m_Workspace->PumpEvents)
            m_Workspace->PumpEvents();
        return;
    }
    const double scale = host.UiScale;
    m_Workspace->PlaceView(m_Workspace->View, host.Window,
                           bounds->GetLayoutX() * scale, bounds->GetLayoutY() * scale,
                           bounds->GetLayoutWidth() * scale, bounds->GetLayoutHeight() * scale);
    if (m_Workspace->PumpEvents)
        m_Workspace->PumpEvents();
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
