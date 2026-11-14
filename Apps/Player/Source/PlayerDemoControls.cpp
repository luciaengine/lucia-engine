#include "PlayerDemoControls.h"

#include "Core/Engine.h"
#include "ECS/ECSTemplates.h"
#include "Engine/GameUI/GameUIHost.h"
#include "Engine/Hosting/RuntimeFrameReadback.h"
#include "Engine/Rendering/Camera.h"
#include "Input/InputSystem.h"
#include "Mathematics/Quaternion.h"
#include "Platform/Window.h"
#include "UI/Controls/Label.h"
#include "Logger/Logger.h"
#include "UI/UIScaleSettings.h"
#include "UI/UIManager.h"
#include "UI/UITargetSpace.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace GameEngine {
namespace {
constexpr float kOrbitSensitivity = 4.0f;
constexpr float kControlsTop = 0.80f;
constexpr float kInitialDistance = 10.0f;

} // namespace

PlayerDemoControls::PlayerDemoControls() = default;
PlayerDemoControls::~PlayerDemoControls() = default;

void PlayerDemoControls::Update(Input::InputSystem& input, Platform::Window& window, float deltaTime)
{
    auto* world = EngineCore::GetInstance().GetPrimaryWorld();
    if (!world) return;
    const auto camera = Engine::Renderer::FindActiveCamera(*world);
    if (!camera) return;
    const auto* transform = world->GetComponent<Components::Transform>(camera->entity);
    if (!transform) return;
    if (!m_HasPose)
    {
        m_InitialPose = *transform;
        const auto rotation = transform->GetRotation();
        const auto forward = rotation.Rotate({0.0f, 0.0f, 1.0f});
        m_Yaw = std::atan2(forward.x, forward.z);
        m_Pitch = -std::asin(std::clamp(forward.y, -1.0f, 1.0f));
        m_Target = transform->GetPosition() + forward * kInitialDistance;
        m_HasPose = true;
    }
    int width = 0, height = 0;
    window.GetWindowSize(width, height);
    if (width <= 0 || height <= 0) return;
    const auto pointer = input.GetMousePosition();
    const bool down = input.IsMouseButtonDown(0);
    const bool pressed = input.WasMouseButtonPressed(0);
    if (pressed)
    {
        m_Dragging = pointer.y < height * kControlsTop;
        m_PreviousPointer = pointer;
    }
    bool changed = false;
    if (down && m_Dragging)
    {
        const auto delta = pointer - m_PreviousPointer;
        m_Yaw += delta.x / width * kOrbitSensitivity;
        m_Pitch = std::clamp(m_Pitch + delta.y / height * kOrbitSensitivity, -1.45f, 1.45f);
        changed = delta.x != 0.0f || delta.y != 0.0f;
    }
    else if (down && pointer.y >= height * kControlsTop)
    {
        if (pointer.x < width / 3.0f)
            m_Distance = std::max(0.5f, m_Distance * std::exp(-std::min(deltaTime, 0.1f)));
        else if (pointer.x >= width * 2.0f / 3.0f)
            m_Distance = std::min(500.0f, m_Distance * std::exp(std::min(deltaTime, 0.1f)));
        else if (pressed)
        {
            world->SetComponentDirect(camera->entity, m_InitialPose);
            m_Distance = kInitialDistance;
            m_HasPose = false;
            m_Dragging = false;
            return;
        }
        changed = true;
    }
    m_PreviousPointer = pointer;
    if (!down) m_Dragging = false;
    if (!changed) return;
    const auto rotation = Mathematics::Quaternion::FromAxisAngle({0.0f, 1.0f, 0.0f}, m_Yaw) *
                          Mathematics::Quaternion::FromAxisAngle({1.0f, 0.0f, 0.0f}, m_Pitch);
    const auto forward = rotation.Rotate({0.0f, 0.0f, 1.0f});
    world->SetComponentDirect(camera->entity, Components::Transform::FromTRS(
        m_Target - forward * m_Distance, rotation, {1.0f, 1.0f, 1.0f}));
}

void PlayerDemoControls::CreateOverlay(RuntimeFrameReadback& readback)
{
    auto& engine = EngineCore::GetInstance();
    m_Overlay = GameUIHost::CreateForHost(&readback.GetDevice(), &engine.GetAssetManager(),
        &engine.GetJobSystem(), engine.GetResolvedAssetRoot(), true);
    auto* ui = m_Overlay->GetUIManager();
    const auto directory = engine.GetResolvedAssetRoot().parent_path() / "PlayerDemo";
    if (!ui->LoadLayoutFromFile((directory / "Overlay.xml").string()) ||
        !ui->AttachStyleFromFile((directory / "Overlay.css").string()))
    {
        Logger::Log::Error("Player demo: packaged overlay is missing at '{}'", directory.string());
        return;
    }
    UI::UIScaleSettings scale;
    scale.mode = UI::UIScaleMode::Fit;
    ui->SetScaleSettings(scale);
    m_Statistics = dynamic_cast<Label*>(ui->GetRootElement()->FindById("demo-statistics"_sid));
}

void PlayerDemoControls::Render(RuntimeFrameReadback& readback, float deltaTime)
{
    const auto& composite = readback.GetComposite();
    if (!composite.PresentSrc.IsValid()) return;
    if (!m_Overlay) CreateOverlay(readback);
    if (!m_Statistics) return;
    m_Elapsed += deltaTime;
    ++m_Frames;
    if (m_Elapsed >= 0.5f)
    {
        char text[80];
        std::snprintf(text, sizeof(text), "%.1f FPS  |  %.1f ms", m_Frames / m_Elapsed,
                      m_Elapsed * 1000.0f / m_Frames);
        m_Statistics->SetText(text);
        m_Elapsed = 0.0f;
        m_Frames = 0;
    }
    uint32_t width = 0, height = 0;
    readback.GetDevice().GetSwapchainSize(width, height);
    auto* ui = m_Overlay->GetUIManager();
    ui->SetLayoutSizeOverride(width, height);
    ui->Update(std::min(deltaTime, 0.1f), false);
    ui->RenderRG(readback.GetFrame(), composite.PresentSrc,
        UI::UITargetSpace::ForPipelineOutput(composite.PresentSpace, readback.GetDevice().GetActiveHdrOutputMode()), Rendering::RenderGraph::RGLoadOp::Load);
}
} // namespace GameEngine
