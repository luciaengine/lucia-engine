#pragma once

#include "Components/Transform.h"
#include "Mathematics/Vector2.h"
#include "Mathematics/Vector3.h"
#include <memory>

namespace GameEngine {
class GameUIHost;
class Label;
class RuntimeFrameReadback;
namespace Input { class InputSystem; }
namespace Platform { class Window; }

/// Opt-in Player scene inspection; never changes a normal game's input or camera.
class PlayerDemoControls
{
public:
    PlayerDemoControls();
    ~PlayerDemoControls();
    void Update(Input::InputSystem& input, Platform::Window& window, float deltaTime);
    void Render(RuntimeFrameReadback& readback, float deltaTime);
private:
    void CreateOverlay(RuntimeFrameReadback& readback);
    std::unique_ptr<GameUIHost> m_Overlay;
    Label* m_Statistics = nullptr;
    Components::Transform m_InitialPose;
    Mathematics::Vector3 m_Target;
    Mathematics::Vector2 m_PreviousPointer;
    float m_Yaw = 0.0f;
    float m_Pitch = 0.0f;
    float m_Distance = 10.0f;
    float m_Elapsed = 0.0f;
    unsigned m_Frames = 0;
    bool m_HasPose = false;
    bool m_Dragging = false;
};
} // namespace GameEngine
