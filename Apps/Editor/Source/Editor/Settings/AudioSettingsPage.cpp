#include "Editor/Settings/AudioSettingsPage.h"

#include "Editor/Settings/EditorSettingsRegistry.h"

#include <utility>

namespace GameEngine::Editor
{
namespace
{
// Read by the audio asset inspector each time it is built.
constexpr const char* kPrefKeyAutoPlayOnSelection = "audio.autoPlayOnSelection";
} // namespace

void RegisterAudioSettingsCategory()
{
    SettingsCategoryDescriptor audio;
    audio.CategoryId = "audio";
    audio.Title = "Audio Settings";
    audio.Group = SettingsCategoryGroup::ProjectSettings;
    audio.TreeRowClass = "audio-settings-row";
    audio.SearchKeywords = "audio sound volume";

    SettingsFieldDescriptor autoPlay;
    autoPlay.Label = "Auto-play On Select";
    autoPlay.Tooltip = "Auto-play audio when file is selected";
    autoPlay.SearchKeywords = "audio auto play preview selection";
    autoPlay.PrefKey = kPrefKeyAutoPlayOnSelection;
    SettingsFieldDescriptor::ToggleField toggle;
    toggle.DefaultValue = true;
    autoPlay.Control = std::move(toggle);
    audio.Fields.push_back(std::move(autoPlay));

    EditorSettingsRegistry::Get().RegisterCategory(std::move(audio));
}

} // namespace GameEngine::Editor
