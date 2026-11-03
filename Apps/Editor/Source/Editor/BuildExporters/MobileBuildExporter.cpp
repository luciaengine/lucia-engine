#include "Editor/BuildExporters/MobileBuildExporter.h"

#include "Editor/Registries/BuildExporterRegistry.h"
#include "Editor/EditorPaths.h"
#include "Core/Application.h"
#include "Core/StandardPaths.h"
#include "Editor/Settings/BuildSettingsStore.h"
#include "Editor/Settings/EditorSettingsRegistry.h"
#include "Editor/Settings/SettingsStore.h"
#include "Engine/Build/BuildPipeline.h"
#include "Engine/Build/MobileExportRunner.h"
#include "Logger/Logger.h"

#include <chrono>
#include <string>
#include <utility>

namespace GameEngine::Editor
{
namespace
{
std::string ReadSetting(const SettingsStore& store, const std::string& platform,
                        const std::string& leaf, const std::string& fallback = {})
{
    std::string value;
    return store.TryGetString(BuildPlatformPrefKey(platform, leaf), value) && !value.empty()
        ? value : fallback;
}

bool RunMobileBuild(const BuildSettings& settings, BuildPipeline& pipeline,
                    const BuildExporterDescriptor::ProgressCallback& progress)
{
    auto store = OpenProjectSettings(settings.projectRoot);
    std::string error;
    if (!store.Load(&error))
    {
        progress(BuildProgress{BuildProgress::Stage::Failed, 0.0f, error});
        return false;
    }
    const std::string& platform = settings.platformName;
    MobileExportInputs inputs;
    inputs.BuildTool = GetEditorGlobalPaths().installAssetsRoot / "Tools/Mobile/build_player.py";
    inputs.EngineRoot = ReadSetting(store, platform, "engineRoot", GE_MOBILE_ENGINE_SOURCE_ROOT);
    inputs.BuildCache = StandardPaths::UserCacheRoot("OpenEngine") / "MobilePlayers";
#if defined(_WIN32)
    inputs.HostGlslc = GetEditorGlobalPaths().installAssetsRoot / "Tools/glslc.exe";
#else
    inputs.HostGlslc = GetEditorGlobalPaths().installAssetsRoot / "Tools/glslc";
#endif
    inputs.ApplicationIdentifier = ReadSetting(store, platform, "applicationIdentifier");
    inputs.AndroidSDK = ReadSetting(store, platform, "androidSDK");
    inputs.AndroidNDK = ReadSetting(store, platform, "androidNDK");
    inputs.SigningTeam = ReadSetting(store, platform, "signingTeam");
    inputs.DeviceUDID = ReadSetting(store, platform, "deviceUDID");
    inputs.Version = ReadSetting(store, platform, "version", "1.0");
    progress(BuildProgress{BuildProgress::Stage::PreparingProject, 0.05f, "Building " + platform + "..."});
    std::filesystem::path contentRoot;
    if (!pipeline.PrepareMobileContent(settings, contentRoot, error))
    {
        progress(BuildProgress{BuildProgress::Stage::Failed, 0.0f, error, {}, {}, pipeline.IsCancelled()});
        return false;
    }
    auto nextProgress = std::chrono::steady_clock::time_point{};
    const bool ok = RunMobileBuildTool(settings, pipeline, inputs, contentRoot,
        [&progress, &nextProgress](const std::string& line) {
            Logger::Log::Info("Build: {}", line);
            const auto now = std::chrono::steady_clock::now();
            if (now >= nextProgress)
            {
                progress(BuildProgress{BuildProgress::Stage::CompilingPlayer, 0.5f, line});
                nextProgress = now + std::chrono::milliseconds(100);
            }
        }, error);
    progress(BuildProgress{ok ? BuildProgress::Stage::Complete : BuildProgress::Stage::Failed,
                           ok ? 1.0f : 0.0f,
                           ok ? platform + " build complete: " + settings.outputDirectory.string() : error,
                           {}, {}, pipeline.IsCancelled()});
    return ok;
}

SettingsFieldDescriptor MakeField(const std::string& platform, const std::string& leaf,
                                  const std::string& label, const std::string& tooltip,
                                  const std::string& fallback = {})
{
    const std::string key = BuildPlatformPrefKey(platform, leaf);
    SettingsFieldDescriptor field;
    field.Label = label;
    field.Tooltip = tooltip;
    SettingsFieldDescriptor::StringField control;
    control.DefaultValue = fallback;
    control.Get = [key, fallback] { return LoadBuildStringSetting(key, fallback); };
    control.Set = [key](const std::string& value) { SaveBuildStringSetting(key, value); };
    field.Control = std::move(control);
    return field;
}

void RegisterPlatform(const std::string& platform)
{
    SettingsCategoryDescriptor category;
    category.CategoryId = "build." + platform + ".mobile";
    category.ParentCategoryId = "build." + platform;
    category.Title = "Mobile build";
    category.Group = SettingsCategoryGroup::ProjectSettings;
    category.Description = "Build the engine Player with the native Android or iOS toolchain. "
                           "No project launcher is needed. iOS requires macOS and Xcode.";
    category.DescriptionAlwaysVisible = true;
    category.SearchKeywords = "mobile android ios sdk ndk signing player";
    category.Fields.push_back(MakeField(platform, "engineRoot", "Engine checkout", "Engine source used to compile the mobile Player.", GE_MOBILE_ENGINE_SOURCE_ROOT));
    category.Fields.push_back(MakeField(platform, "applicationIdentifier", "Application identifier", "Unique reverse-domain identifier. Empty uses a stable project-specific identifier."));
    if (platform == "Android")
    {
        category.Fields.push_back(MakeField(platform, "androidSDK", "Android SDK", "Installed Android SDK directory."));
        category.Fields.push_back(MakeField(platform, "androidNDK", "Android NDK", "Installed Android NDK directory."));
    }
    else
    {
        category.Fields.push_back(MakeField(platform, "signingTeam", "Signing team", "Apple development team ID. Leave empty for an unsigned build."));
        category.Fields.push_back(MakeField(platform, "deviceUDID", "Device UDID", "Device registered for development signing. Pair with Signing team."));
    }
    EditorSettingsRegistry::Get().RegisterCategory(std::move(category));
    BuildExporterDescriptor exporter;
    exporter.ExporterId = "mobile." + platform;
    exporter.DisplayName = platform + " build";
    exporter.HandlesPlatform = [platform](std::string_view name) { return name == platform; };
    exporter.Run = &RunMobileBuild;
    BuildExporterRegistry::Get().Register(std::move(exporter));
}
}

void RegisterMobileBuildExporters()
{
    RegisterPlatform("Android");
    RegisterPlatform("iOS");
}
}
