#pragma once

#include <functional>

namespace GameEngine
{
struct InspectorContext;

// Registers the default Camera component inspector with InspectorRegistry.
void RegisterCameraInspector();

// The pixel-perfect camera's Pixels Per Unit rows: the Auto Pixels Per Unit toggle (one undo step on
// every selected camera), then the resolved value (read-only) or the typed value. `requestRebuild`
// swaps the second row after a toggle.
void AddCameraPixelsPerUnitRows(const InspectorContext& ctx, std::function<void()> requestRebuild);

} // namespace GameEngine
