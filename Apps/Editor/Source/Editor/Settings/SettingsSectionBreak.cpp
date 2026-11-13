#include "Editor/Settings/SettingsSectionBreak.h"

#include "Editor/Settings/EditorSettingsRegistry.h"
#include "Inspectors/InspectorDragHelpers.h"
#include "Logger/Logger.h"
#include "UI/ControlLayoutLoader.h"
#include "UI/Controls/CollapsibleInfoCard.h"
#include "UI/Controls/Label.h"

#include <memory>
#include <string>
#include <utility>

namespace GameEngine::Editor
{
namespace
{

constexpr const char* kLayoutAssetPath = "UI/controls/SettingsSectionBreak/SettingsSectionBreak.uxml";
constexpr const char* kStyleAssetPath = "UI/controls/SettingsSectionBreak/SettingsSectionBreak.css";
constexpr const char* kHeaderClass = "settings-section-break-header";
constexpr const char* kDescriptionClass = "settings-section-break-description";
constexpr const char* kHiddenClass = "hidden";

} // namespace

SettingsSectionBreak::SettingsSectionBreak(const SettingsFieldDescriptor& field)
{
    AddClass("settings-section-break");
    RequestSubtreeStyleAssetPath(kStyleAssetPath, "editor");

    // The parsed body is added whole: a settings page can be built inside an event dispatch, where
    // TakeChild refuses to move children.
    std::unique_ptr<UIElement> body = ControlLayoutLoader::Load(kLayoutAssetPath, {});
    if (!body)
        return;
    auto* header = dynamic_cast<Label*>(InspectorDrag::FindChildByClass(body.get(), kHeaderClass));
    auto* description = dynamic_cast<EditorUI::CollapsibleInfoCard*>(
        InspectorDrag::FindChildByClass(body.get(), kDescriptionClass));
    if (!header || !description)
    {
        Logger::Log::Error("SettingsSectionBreak: {} needs a Label with the class {} and a CollapsibleInfoCard with "
                           "the class {}.",
                           kLayoutAssetPath, kHeaderClass, kDescriptionClass);
        return;
    }

    header->SetText(field.SectionHeader);
    if (field.SectionHeader.empty())
        header->AddClass(kHiddenClass);
    description->SetText(field.SectionDescription);
    if (field.SectionDescription.empty())
        description->AddClass(kHiddenClass);
    AddChild(std::move(body));
}

} // namespace GameEngine::Editor
