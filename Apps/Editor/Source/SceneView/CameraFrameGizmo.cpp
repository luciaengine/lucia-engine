#include "SceneView/CameraFrameGizmo.h"

#include <algorithm>
#include <cmath>

#include "Engine/Rendering/CameraAspectRatio.h"
#include "Mathematics/Angle.h"
#include "Mathematics/Vector3.h"
#include "Types/Color.h"

namespace GameEngine::Editor::SceneTools
{

using GameEngine::Mathematics::Vector3;

namespace
{

// Blue frame color shared by the ortho rectangle and the perspective frame.
const Color kFrameColor(0.25f, 0.55f, 1.0f, 1.0f);
constexpr float kFrameThickness = 2.0f;

// Distance in front of a perspective camera at which the framing rectangle is
// drawn (clamped to the camera's near/far). Far enough to read clearly without
// dominating the view; the frustum has the same framing at any depth.
constexpr float kFrameGuideDistance = 4.0f;

// Resolve the projection aspect (width / height) for the framed area. For a
// Native preset the live Game View size isn't available to a Scene View gizmo,
// so we approximate with 16:9. Explicit presets use their fixed ratio and a
// Custom preset uses the camera's stored numerator/denominator.
float ResolveFrameAspect(const GameEngine::Components::Camera& params)
{
    using GameEngine::Engine::Renderer::CameraAspectPreset;
    using GameEngine::Engine::Renderer::GetCameraAspectPreset;
    using GameEngine::Engine::Renderer::GetCameraAspectPresetRatio;

    const CameraAspectPreset preset = GetCameraAspectPreset(params);
    if (preset == CameraAspectPreset::Custom)
    {
        const float w = std::max(params.CustomAspectWidth, 0.0001f);
        const float h = std::max(params.CustomAspectHeight, 0.0001f);
        return w / h;
    }
    return GetCameraAspectPresetRatio(preset);
}

} // namespace

void CameraFrameGizmo::SetCamera(bool hasCamera,
                                 const GameEngine::Components::Camera& params,
                                 const Mathematics::Matrix4x4& worldTransform)
{
    m_HasCamera      = hasCamera;
    m_Params         = params;
    m_WorldTransform = worldTransform;
}

void CameraFrameGizmo::Render(GizmoRenderContext& context)
{
    if (!m_HasCamera)
    {
        return;
    }

    // The world transform columns are the right, up and forward axes and the translation.
    const Mathematics::Matrix4x4& world = m_WorldTransform;
    const Vector3 right(world[0].x, world[0].y, world[0].z);
    const Vector3 up(world[1].x, world[1].y, world[1].z);
    const Vector3 forward(world[2].x, world[2].y, world[2].z);
    const Vector3 center(world[3].x, world[3].y, world[3].z);

    const float aspect = ResolveFrameAspect(m_Params);

    if (!m_Params.Perspective)
    {
        // Orthographic: the visible rect lies on the camera's right/up plane,
        // centered at the camera position.
        float frameHeight = m_Params.OrthographicSize;
        float frameWidth  = frameHeight * aspect;
        if (m_Params.PixelPerfect)
        {
            const float ppu = static_cast<float>(std::max(1u, m_Params.PixelPerfectPixelsPerUnit));
            frameHeight = static_cast<float>(m_Params.PixelPerfectReferenceHeight) / ppu;
            frameWidth  = static_cast<float>(m_Params.PixelPerfectReferenceWidth) / ppu;
        }

        const float halfW = frameWidth * 0.5f;
        const float halfH = frameHeight * 0.5f;

        auto corner = [&](float sx, float sy)
        {
            return center + right * sx * halfW + up * sy * halfH;
        };

        const Vector3 bl = corner(-1.0f, -1.0f);
        const Vector3 br = corner( 1.0f, -1.0f);
        const Vector3 tr = corner( 1.0f,  1.0f);
        const Vector3 tl = corner(-1.0f,  1.0f);

        GizmoLineBatch batch(context, kFrameColor, kFrameThickness);
        batch.AddLine(bl, br);
        batch.AddLine(br, tr);
        batch.AddLine(tr, tl);
        batch.AddLine(tl, bl);
        return;
    }

    // Perspective: draw a single rectangle for the camera's framing at a guide
    // distance in front of the camera. The full frustum's connecting edges read
    // as a noisy "cross"; one rectangle (matching the ortho case) is clearer.
    const float halfFovY = Mathematics::ToRadians(m_Params.FovY) * 0.5f;
    const float tanHalf  = std::tan(halfFovY);
    float dist = kFrameGuideDistance;
    if (dist < m_Params.NearZ) dist = m_Params.NearZ;
    if (dist > m_Params.FarZ)  dist = m_Params.FarZ;

    const float halfH = tanHalf * dist;
    const float squeeze = std::clamp(m_Params.AnamorphicSqueeze, 1.0f, 4.0f);
    const float halfW = halfH * aspect * squeeze;
    auto corner = [&](float sx, float sy)
    {
        return center + forward * dist + right * sx * halfW + up * sy * halfH;
    };

    const Vector3 bl = corner(-1.0f, -1.0f);
    const Vector3 br = corner( 1.0f, -1.0f);
    const Vector3 tr = corner( 1.0f,  1.0f);
    const Vector3 tl = corner(-1.0f,  1.0f);

    GizmoLineBatch batch(context, kFrameColor, kFrameThickness);
    batch.AddLine(bl, br);
    batch.AddLine(br, tr);
    batch.AddLine(tr, tl);
    batch.AddLine(tl, bl);
}

} // namespace GameEngine::Editor::SceneTools
