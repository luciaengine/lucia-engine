#include "SceneView/MeasureTool.h"

#include "Mathematics/Ray.h"
#include "Platform/Window.h"
#include "SceneViewController.h"
#include "UI/Controls/Widgets/SceneViewMeasureOverlay.h"

namespace GameEngine::Editor::SceneTools
{

MeasureTool::MeasureTool(SceneViewController& owner)
    : m_Owner(owner)
{
}

void MeasureTool::OnDeactivated()
{
    m_DragActive = false;
}

void MeasureTool::OnPointerEvent(const ScenePointerEvent& event)
{
    switch (event.phase)
    {
    case PointerPhase::Down:
        if (event.button != PointerButton::Left || !m_Overlay)
            return;
        m_DragActive = true;
        m_DragIs2D = m_Owner.Is2DMode();
        m_DragStartWorld = PointFromPointer(event);
        m_DragEndWorld = m_DragStartWorld;
        m_Overlay->BeginMeasure(event.viewX, event.viewY, m_DragStartWorld, m_DragIs2D, event.shift);
        return;
    case PointerPhase::Move:
        if (!m_DragActive || !m_Overlay)
            return;
        m_DragEndWorld = PointFromPointer(event);
        m_Overlay->UpdateMeasure(event.viewX, event.viewY, m_DragEndWorld, event.shift);
        return;
    case PointerPhase::Up:
        if (!m_DragActive || event.button != PointerButton::Left)
            return;
        m_DragEndWorld = PointFromPointer(event);
        if (m_Overlay)
            m_Overlay->UpdateMeasure(event.viewX, event.viewY, m_DragEndWorld, event.shift);
        FinishDrag(/*reportMeasured=*/true);
        return;
    }
}

void MeasureTool::FinishDragIfReleased()
{
    if (m_DragActive && !Platform::Window::IsLeftMouseButtonDown())
        FinishDrag(/*reportMeasured=*/true);
}

std::array<float, 3> MeasureTool::PointFromPointer(const ScenePointerEvent& event) const
{
    std::array<float, 3> out{event.ray.origin.x, event.ray.origin.y, event.ray.origin.z};
    if (m_Owner.Is2DMode() || m_Owner.IsOrthographic())
        return out;

    const auto pose = m_Owner.GetCameraPose();
    const Mathematics::Vector3 planePoint = event.cameraPos + event.cameraForward * pose.Distance;

    float t = 0.0f;
    Mathematics::Vector3 hit;
    if (Mathematics::IntersectRayPlane(event.ray, planePoint, event.cameraForward, t, hit))
        out = {hit.x, hit.y, hit.z};
    return out;
}

void MeasureTool::FinishDrag(bool reportMeasured)
{
    if (!m_DragActive)
        return;
    m_DragActive = false;
    if (m_Overlay)
        m_Overlay->EndMeasure();
    if (reportMeasured && m_OnMeasured)
        m_OnMeasured(m_DragStartWorld, m_DragEndWorld, m_DragIs2D);
}

} // namespace GameEngine::Editor::SceneTools
