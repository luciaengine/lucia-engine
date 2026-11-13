#include "Editor/Settings/BuildSettingsStore.h"

#include "Core/Engine.h"
#include "Editor/Settings/SettingsStore.h"
#include "Projects/ProjectStartupScene.h"
#include "Types/StringUtils.h"

namespace GameEngine::Editor
{
namespace
{

constexpr const char* kGlobalBuildScenesPrefKey = "build.globalScenes";

SettingsStore OpenBuildSettingsStore()
{
    const auto& root = EngineCore::GetInstance().GetWorkspaceRoot();
    if (!root.empty())
        return OpenProjectSettings(root);
    return OpenEditorPreferences();
}

// One newline-separated scene list, trimmed of blank and whitespace-only lines
// so a stray trailing newline never becomes an empty scene entry.
std::vector<std::string> ParseSceneList(const std::string& raw)
{
    std::vector<std::string> out;
    for (std::string_view line : Split(raw, '\n'))
    {
        line = TrimWhitespaceView(line);
        if (!line.empty())
            out.emplace_back(line);
    }
    return out;
}

std::string JoinSceneList(const std::vector<std::string>& scenes)
{
    std::string raw;
    for (size_t i = 0; i < scenes.size(); ++i)
    {
        if (i)
            raw += '\n';
        raw += scenes[i];
    }
    return raw;
}

std::vector<std::string> LoadSceneList(const std::string& prefKey)
{
    const std::string raw = LoadBuildStringSetting(prefKey);
    if (raw.empty())
        return {};
    return ParseSceneList(raw);
}
} // namespace

std::string LoadBuildStringSetting(const std::string& prefKey, const std::string& defaultValue)
{
    auto prefs = OpenBuildSettingsStore();
    std::string err;
    prefs.Load(&err);
    std::string value;
    if (!prefs.TryGetString(prefKey, value) || value.empty())
        return defaultValue;
    return value;
}

void SaveBuildStringSetting(const std::string& prefKey, const std::string& value)
{
    auto prefs = OpenBuildSettingsStore();
    std::string err;
    prefs.Load(&err);
    prefs.SetString(prefKey, value);
    prefs.Save(&err);
}

bool LoadBuildBoolSetting(const std::string& prefKey, bool defaultValue)
{
    auto prefs = OpenBuildSettingsStore();
    std::string err;
    prefs.Load(&err);
    bool value = defaultValue;
    prefs.TryGetBool(prefKey, value);
    return value;
}

void SaveBuildBoolSetting(const std::string& prefKey, bool value)
{
    auto prefs = OpenBuildSettingsStore();
    std::string err;
    prefs.Load(&err);
    prefs.SetBool(prefKey, value);
    prefs.Save(&err);
}

std::string BuildPlatformPrefKey(const std::string& platformName, const std::string& leaf)
{
    return "build.platform." + platformName + "." + leaf;
}

std::vector<std::string> LoadBuildScenes(const std::string& platformName)
{
    return LoadSceneList(BuildPlatformPrefKey(platformName, "scenes"));
}

void SaveBuildScenes(const std::string& platformName, const std::vector<std::string>& scenes)
{
    SaveBuildStringSetting(BuildPlatformPrefKey(platformName, "scenes"), JoinSceneList(scenes));
}

std::vector<std::string> LoadGlobalBuildScenes()
{
    return LoadSceneList(kGlobalBuildScenesPrefKey);
}

void SaveGlobalBuildScenes(const std::vector<std::string>& scenes)
{
    SaveBuildStringSetting(kGlobalBuildScenesPrefKey, JoinSceneList(scenes));
}

bool LoadBuildPlatformUseGlobalScenes(const std::string& platformName)
{
    // Default on: a fresh project ships every platform from the global list.
    return LoadBuildBoolSetting(BuildPlatformPrefKey(platformName, "useGlobalScenes"), true);
}

std::vector<std::string> LoadEffectiveBuildScenes(const std::string& platformName)
{
    if (LoadBuildPlatformUseGlobalScenes(platformName))
    {
        std::vector<std::string> globalScenes = LoadGlobalBuildScenes();
        if (!globalScenes.empty())
            return globalScenes;
    }
    std::vector<std::string> platformScenes = LoadBuildScenes(platformName);
    if (!platformScenes.empty())
        return platformScenes;
    const std::filesystem::path& projectRoot = EngineCore::GetInstance().GetWorkspaceRoot();
    SettingsStore settings = OpenBuildSettingsStore();
    std::string err;
    settings.Load(&err);
    std::string startupScene = ReadProjectStartupScene(settings);
    if (projectRoot.empty() || !ResolveNamedScene(projectRoot / "Assets", startupScene, kProjectStartupSceneKey))
        return {};
    return {std::move(startupScene)};
}

std::string LoadBuildGlobalIcon()
{
    return LoadBuildStringSetting(kGlobalBuildIconPrefKey);
}

std::string LoadBuildPlatformIcon(const std::string& platformName)
{
    return LoadBuildStringSetting(BuildPlatformPrefKey(platformName, "icon"));
}

bool LoadBuildPlatformUseGlobalIcon(const std::string& platformName)
{
    return LoadBuildBoolSetting(BuildPlatformPrefKey(platformName, "useGlobalIcon"), true);
}

} // namespace GameEngine::Editor
