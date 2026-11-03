#include "SceneView/GizmoSettingsWiring.h"

#include "SceneView/TransformTool.h"
#include "SceneViewController.h"
#include "Editor/Settings/GizmoSettingsPage.h"
#include "Editor/Settings/GridSettingsPage.h"
#include "Editor/Settings/SettingsStore.h"

namespace GameEngine
{
namespace Editor
{
namespace SceneTools
{

void GizmoSettingsWiring::WireGizmoSettings(::GameEngine::SceneViewController* sceneController)
{
    if (!sceneController)
    {
        return;
    }

    RegisterGizmoSettingsCategory([sceneController]() { return sceneController->GetTransformTool(); });
    RegisterGridSettingsCategory([sceneController](float size) { sceneController->SetGridSnapSize(size); });

    // Apply saved grid + gizmo preferences at startup.
    {
        auto prefs = ::GameEngine::Editor::OpenEditorPreferences();
        std::string err;
        prefs.Load(&err);
        double opacity = 0.3;
        double snapSize = 0.5;
        prefs.TryGetDouble("grid.opacity", opacity);
        prefs.TryGetDouble("grid.snapSize", snapSize);
        sceneController->SetGridOpacity(static_cast<float>(opacity));
        sceneController->SetGridSnapSize(static_cast<float>(snapSize));
    }
    if (TransformTool* tool = sceneController->GetTransformTool())
        ApplySavedGizmoSettings(*tool);
}

} // namespace SceneTools
} // namespace Editor
} // namespace GameEngine
