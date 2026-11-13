#include "UI/Controls/EditorBuildStatus.h"

#include "Editor/Application/BuildConfigStatus.h"
#include "Editor/Application/EditorVersion.h"
#include "UI/ControlLayoutLoader.h"
#include "UI/Controls/Label.h"
#include "UI/Registration/ElementRegistration.h"

#include <array>
#include <memory>
#include <string_view>

namespace GameEngine
{
namespace
{
constexpr const char* kLayoutAssetPath = "UI/controls/EditorBuildStatus/EditorBuildStatus.uxml";
constexpr const char* kStyleAssetPath = "UI/controls/EditorBuildStatus/EditorBuildStatus.css";
constexpr std::string_view kConfigurationId = "BuildConfigStatus";
constexpr std::string_view kVersionId = "EditorVersionStatus";
} // namespace

EditorBuildStatus::EditorBuildStatus()
{
    RequestSubtreeStyleAssetPath(kStyleAssetPath, "editor");
    constexpr std::array<std::string_view, 2> requiredIds{kConfigurationId, kVersionId};
    std::unique_ptr<UIElement> layout = Editor::ControlLayoutLoader::Load(kLayoutAssetPath, requiredIds);
    if (!layout)
        return;
    if (auto* configuration = layout->FindById<Label>(HashStringId(kConfigurationId)).Get())
        configuration->SetText(Editor::BuildConfigStatusText());
    if (auto* version = layout->FindById<Label>(HashStringId(kVersionId)).Get())
        version->SetText(Editor::EditorVersionText());
    AddChild(std::move(layout));
}
} // namespace GameEngine

namespace RegisterWidgets
{
static auto s_EditorBuildStatus = GameEngine::UIRegistration::RegisterWithFactory<GameEngine::EditorBuildStatus>(
    "EditorBuildStatus", []() { return std::make_unique<GameEngine::EditorBuildStatus>(); });
}
