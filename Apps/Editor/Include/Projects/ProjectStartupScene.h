#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace GameEngine::Editor
{

class SettingsStore;

// Project settings key naming the scene the editor opens with the project; the
// value is asset-relative ("Scenes/Main.scene"), the form build.globalScenes uses.
inline constexpr const char* kProjectStartupSceneKey = "project.startupScene";

// What a project says about the scene it opens on, gathered by the caller.
struct ProjectStartupSceneCandidates
{
    // build.globalScenes, asset-relative, in build order.
    std::vector<std::string> GlobalBuildScenes;
    // project.startupScene, asset-relative; empty when unset.
    std::string StartupScene;
    // The editor's recent scenes, absolute, most recent first; only those inside
    // the project count as the project's last-opened scene.
    std::vector<std::filesystem::path> RecentScenes;
};

// The fix for loaded project settings that name the startup scene in a nested "project": { "startupScene" }
// object, which the editor does not read. Empty when the settings hold no such object.
std::string DescribeNestedProjectStartupScene(const SettingsStore& settings);

// The startup scene that loaded project settings name under kProjectStartupSceneKey; empty when unset.
// Logs DescribeNestedProjectStartupScene as a warning when it is not empty.
std::string ReadProjectStartupScene(const SettingsStore& settings);

// The absolute scene an asset-relative name refers to, when it is a scene file inside
// `assetsRoot`. Otherwise none, and a warning names `source` (the setting that holds the name).
std::optional<std::filesystem::path> ResolveNamedScene(const std::filesystem::path& assetsRoot,
                                                       const std::string& assetRelative, const char* source);

// The scene opening `projectRoot` loads: the first build scene, else the
// project's startupScene, else its last-opened scene, else none (the caller
// opens an empty scene). A named scene that is missing or outside the
// project's Assets folder is skipped with a warning and the next rule applies.
std::optional<std::filesystem::path> ResolveProjectStartupScene(
    const std::filesystem::path& projectRoot, const ProjectStartupSceneCandidates& candidates);

// The startup scene a freshly created project names, asset-relative: the
// startupScene of its game.config when that scene exists, else its only scene
// file. Empty when neither applies.
std::string FindNewProjectStartupScene(const std::filesystem::path& projectRoot);

} // namespace GameEngine::Editor
