#pragma once

#include "Components/Rendering/Camera.h"
#include "Mathematics/Matrix4x4.h"
#include "SceneView/SceneViewGizmos.h"

namespace GameEngine::Editor::SceneTools
{

// Scene View gizmo that overlays the active game camera's visible frame as a
// blue line box. In orthographic / 2D mode it draws the framed rectangle; in
// perspective mode it draws the camera frustum outline. The host feeds the
// active camera each frame via SetCamera(); the gizmo draws from the stored
// copy so it never needs ECS world access of its own.
class CameraFrameGizmo : public IGizmo
{
public:
    CameraFrameGizmo() = default;

    // worldTransform holds right, up and forward in columns 0, 1 and 2 and the
    // translation in column 3. When hasCamera is false the gizmo draws nothing.
    void SetCamera(bool hasCamera,
                   const GameEngine::Components::Camera& params,
                   const Mathematics::Matrix4x4& worldTransform);

    void Render(GizmoRenderContext& context) override;

private:
    bool                       m_HasCamera{false};
    GameEngine::Components::Camera m_Params{};
    Mathematics::Matrix4x4     m_WorldTransform{};
};

} // namespace GameEngine::Editor::SceneTools
