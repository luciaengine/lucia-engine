#include "Engine/Build/MobileExportRunner.h"

#include "Engine/Build/BuildHost.h"
#include "Engine/Build/PythonBuildTool.h"
#include "Core/Application.h"
#include "Engine/Build/BuildPipeline.h"
#include "AssetCore/GUID.h"
#include "Platform/Process.h"
#include "Types/PathUtils.h"

#include <fstream>
#include <system_error>

#include <nlohmann/json.hpp>

namespace GameEngine
{
namespace
{
namespace fs = std::filesystem;

fs::path ResolvePath(const fs::path& path, const fs::path& project)
{
    return fs::weakly_canonical(path.is_absolute() ? path : project / path);
}

bool ValidateArtifact(const fs::path& output, const nlohmann::json& receipt,
                      const std::string& requestId, const BuildSettings& settings)
{
    if (receipt.value("requestId", std::string{}) != requestId
        || receipt.value("platform", std::string{}) != settings.platformName
        || receipt.value("configuration", std::string{}) != settings.buildConfiguration)
        return false;
    const fs::path relative = receipt.value("artifact", std::string{});
    if (relative.empty() || relative.is_absolute())
        return false;
    const fs::path artifact = fs::weakly_canonical(output / relative);
    if (RelativePathUnderRoot(artifact, output).empty())
        return false;
    if (settings.platformName == "Android" || artifact.extension() == ".ipa")
        return artifact.extension() == (settings.platformName == "Android" ? ".apk" : ".ipa")
            && fs::is_regular_file(artifact) && fs::file_size(artifact) > 0;
    return artifact.extension() == ".app" && fs::is_directory(artifact)
        && fs::is_regular_file(artifact / "Info.plist");
}
}

bool RunMobileBuildTool(const BuildSettings& settings, BuildPipeline& pipeline,
                     const MobileExportInputs& inputs, const std::filesystem::path& contentRoot,
                     const std::function<void(const std::string&)>& onLine, std::string& error)
{
    error.clear();
    if (pipeline.IsCancelled())
    {
        error = "Mobile build cancelled.";
        return false;
    }
    if (settings.platformName != "Android" && settings.platformName != "iOS")
    {
        error = "Select Android or iOS for a mobile build.";
        return false;
    }
    if (settings.platformName == "iOS" && CurrentBuildHost() != BuildHost::Mac)
    {
        error = "Build iOS on a Mac with Xcode installed.";
        return false;
    }
    try
    {
        if (settings.projectRoot.empty() || settings.outputDirectory.empty() || settings.scenes.empty())
            throw std::runtime_error("Open a project, choose an output folder, and add a build scene.");
        const fs::path project = fs::canonical(settings.projectRoot);
        const fs::path output = ResolvePath(settings.outputDirectory, project);
        if (output == output.root_path() || output == project
            || !RelativePathUnderRoot(project, output).empty())
            throw std::runtime_error("Choose a mobile output folder outside the project root and its parents.");
        for (const char* authored : {"Assets", "Packages", "Platforms", "Managed", "UserScripts", ".Editor", ".git"})
        {
            const fs::path source = fs::weakly_canonical(project / authored);
            if (output == source || !RelativePathUnderRoot(output, source).empty())
                throw std::runtime_error("Choose a mobile output folder outside authored project folders.");
        }
        const fs::path buildTool = inputs.BuildTool;
        if (!fs::is_regular_file(buildTool))
            throw std::runtime_error("The installed engine mobile build tool is missing. Rebuild or reinstall the Editor.");
        if (!fs::is_regular_file(inputs.EngineRoot / "CMakeLists.txt"))
            throw std::runtime_error("Set Engine checkout in Build > " + settings.platformName + " > Mobile build to the engine source checkout.");
        fs::create_directories(output);
        const fs::path resultPath = output / "mobile-build-result.json";
        fs::remove(resultPath);
        const std::string requestId = GUID::Generate().ToString();
        const fs::path requestPath = output / "mobile-build-request.json";
        const fs::path configPath = fs::canonical(contentRoot / "game.config");
        if (fs::is_symlink(requestPath))
            throw std::runtime_error("Remove the symbolic link at the mobile build request path.");
        std::string artifactName = PathUtils::SanitizeForFolderName(settings.playerConfig.gameName);
        while (!artifactName.empty() && artifactName.front() == '.') artifactName.erase(artifactName.begin());
        if (artifactName.empty()) artifactName = "Game";
        const nlohmann::json request = {
            {"schemaVersion", 1}, {"requestId", requestId}, {"platform", settings.platformName},
            {"projectRoot", project.string()}, {"outputDirectory", output.string()},
            {"configuration", settings.buildConfiguration}, {"scenes", settings.scenes},
            {"gameConfigPath", configPath.string()}, {"contentRoot", fs::canonical(contentRoot).string()},
            {"iconsRoot", (output / ".mobile-icons/icons").string()},
            {"nativeSourcesPath", (output / "mobile-native-sources.json").string()},
            {"buildCache", inputs.BuildCache.string()}, {"hostGlslc", inputs.HostGlslc.string()},
            {"artifactName", artifactName}, {"applicationIdentifier", inputs.ApplicationIdentifier}, {"version", inputs.Version},
            {"resultPath", resultPath.string()},
            {"toolchain", {{"engineRoot", inputs.EngineRoot.string()},
                           {"androidSDK", inputs.AndroidSDK.string()}, {"androidNDK", inputs.AndroidNDK.string()},
                           {"signingTeam", inputs.SigningTeam}, {"deviceUDID", inputs.DeviceUDID}}}};
        std::ofstream stream(requestPath);
        stream << request.dump(2) << '\n';
        stream.close();
        if (!stream)
            throw std::runtime_error("Could not write the mobile build request.");
        ProcessLaunch launch;
        launch.Executable = FindPythonBuildExecutable().string();
        if (launch.Executable.empty()) throw std::runtime_error("Install Python 3 to run the engine mobile build tools.");
        launch.Arguments = {buildTool.string(), "--request", requestPath.string()};
        launch.WorkingDirectory = project;
        const auto result = pipeline.RunProcess(launch, onLine);
        if (result.cancelled || pipeline.IsCancelled())
            throw std::runtime_error("Mobile build cancelled.");
        if (result.exitCode != 0)
            throw std::runtime_error("Engine mobile build failed (exit " + std::to_string(result.exitCode)
                                     + "). " + result.stderrTail);
        std::ifstream receiptStream(resultPath);
        nlohmann::json receipt;
        receiptStream >> receipt;
        if (!ValidateArtifact(output, receipt, requestId, settings))
            throw std::runtime_error("Engine mobile build did not produce a matching APK, IPA, or app bundle receipt.");
        return true;
    }
    catch (const std::exception& exception)
    {
        error = std::string("Mobile build failed: ") + exception.what();
        return false;
    }
}
}
