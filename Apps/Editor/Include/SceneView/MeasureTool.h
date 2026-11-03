#pragma once

#include "SceneView/SceneViewTools.h"

#include <array>
#include <functional>
#include <utility>

namespace GameEngine
{

class SceneViewController;
class SceneViewMeasureOverlay;

namespace Editor::SceneTools
{

// Measure (SceneViewController::ToolKind::Measure): a left-button drag in the
// view measures from the press point to the pointer, drawn by the viewport's
// SceneViewMeasureOverlay. In a perspective view the points lie on the plane
// through the orbit pivot facing the camera; in 2D and orthographic views they
// are the pointer ray origin. The Scene View panel turns Measure on and off for
// every viewport at once and hands each tool its viewport's overlay.
class MeasureTool final : public ISceneTool
{
  public:
    // A completed drag: world start and end, and whether it was measured in 2D.
    using MeasuredCallback =
        std::function<void(const std::array<float, 3>& start, const std::array<float, 3>& end, bool is2D)>;

    explicit MeasureTool(SceneViewController& owner);

    const char* GetName() const override { return "MeasureTool"; }
    // Drops a drag in progress without reporting it.
    void OnDeactivated() override;
    // Expects view coordinates clamped to the viewport, with the ray built from
    // them: a drag keeps measuring while the pointer is outside the view.
    void OnPointerEvent(const ScenePointerEvent& event) override;
    void OnKeyEvent(const SceneKeyEvent& /*event*/) override {}
    void GatherGizmos(GizmoCollector& /*collector*/) override {}

    // The overlay of the viewport this tool's controller renders; null
    // disables measuring.
    void SetOverlay(SceneViewMeasureOverlay* overlay) { m_Overlay = overlay; }
    void SetOnMeasured(MeasuredCallback callback) { m_OnMeasured = std::move(callback); }

    bool IsDragging() const { return m_DragActive; }
    // Completes a drag whose button was released where no pointer-up reached
    // the viewport.
    void FinishDragIfReleased();
    // Ends a drag without reporting it: the overlay drops the line in progress.
    void AbandonDrag() { FinishDrag(/*reportMeasured=*/false); }

  private:
    std::array<float, 3> PointFromPointer(const ScenePointerEvent& event) const;
    void FinishDrag(bool reportMeasured);

    SceneViewController& m_Owner;
    SceneViewMeasureOverlay* m_Overlay = nullptr; // not owned
    MeasuredCallback m_OnMeasured;

    bool m_DragActive = false;
    bool m_DragIs2D = false;
    std::array<float, 3> m_DragStartWorld{0.0f, 0.0f, 0.0f};
    std::array<float, 3> m_DragEndWorld{0.0f, 0.0f, 0.0f};
};

} // namespace Editor::SceneTools
} // namespace GameEngine
