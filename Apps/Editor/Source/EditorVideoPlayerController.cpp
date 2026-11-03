#include "EditorVideoPlayerController.h"
#include "Panels/VideoPlayerModal.h"
#include "Assets/AssetManager.h"
#include "Core/Engine.h"
#include "UI/UIElement.h"
#include "Rendering/Core/Device.h"
#include "UI/UIManager.h"

#include <memory>

namespace GameEngine {

void EditorVideoPlayerController::Setup(UIElement* root, Rendering::IDevice* device, UIManager* uiManager)
{
    if (!root) return;
    auto modal = std::make_unique<VideoPlayerModal>();
    modal->SetDevice(device);
    modal->SetUIManager(uiManager);
    m_Modal = modal.get();
    root->AddChild(std::move(modal));
}

void EditorVideoPlayerController::Open(const std::filesystem::path& videoPath, const std::string& title)
{
    if (m_Modal)
    {
        m_Modal->SetOnClose({});
        if (UIElement* area = m_Modal->FindById("VideoPlayerVideoArea"))
            area->RemoveClass("engine-info-video-area");
        m_Modal->Open(videoPath.string(), title);
    }
}

void EditorVideoPlayerController::OpenIntro()
{
    if (!m_Modal)
        return;
    auto& assets = EngineCore::GetInstance().GetAssetManager();
    const auto path = assets.ResolveAssetPath("Branding/LuciaFlameLoop.mp4", "editor");
    m_Modal->SetOnClose([modal = m_Modal]()
    {
        UIManager* manager = modal->GetOwnerManager();
        if (manager && !manager->IsBeingDestroyed())
        {
            if (UIElement* root = manager->GetRootElement())
                manager->FocusElement(root->FindById("TopToolbarRight2"));
        }
    });
    if (UIElement* area = m_Modal->FindById("VideoPlayerVideoArea"))
        area->AddClass("engine-info-video-area");
    m_Modal->Open(path.string(), "Lucia Engine " GE_ENGINE_VERSION, true);
}

bool EditorVideoPlayerController::IsOpen() const
{
    return m_Modal && m_Modal->IsOpen();
}

void EditorVideoPlayerController::Close()
{
    if (m_Modal)
        m_Modal->Close();
}

void EditorVideoPlayerController::Update()
{
    if (m_Modal)
        m_Modal->Update();
}

void EditorVideoPlayerController::TickRenderRG(Rendering::RenderGraph::RGFrame& frame)
{
    if (m_Modal && m_Modal->IsOpen())
        m_Modal->TickRenderRG(frame);
}

} // namespace GameEngine
