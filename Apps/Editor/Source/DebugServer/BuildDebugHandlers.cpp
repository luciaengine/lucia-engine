#include "DebugHandlerHelpers.h"

namespace GameEngine
{
void RegisterBuildDebugHandlers(EditorDebugServer& server, EditorApplication& app)
{
server.RegisterHandler("trigger_build", [&app](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        if (!app.m_PanelManager)
            return Editor::RefuseRequest("No panel manager");

        auto* buildPanel = app.m_PanelManager->FindFirstPanelOfType<BuildPanel>();
        if (!buildPanel)
            return Editor::RefuseRequest("BuildPanel not found. Open it via the toolbar first.");

        if (buildPanel->GetBuildStatus().running)
            return Editor::RefuseRequest("A build is already running");

        std::string platformName;
        if (ctx.params.contains("platformName"))
            platformName = ctx.params["platformName"].get<std::string>();
        else if (ctx.params.contains("platform"))
            platformName = ctx.params["platform"].get<std::string>();
        if (!platformName.empty() && !buildPanel->SetOnlyEnabledPlatform(platformName))
            return Editor::RefuseRequest("Unknown build platform: " + platformName);

        // This handler already runs on the main thread (FlushPendingRequests), where
        // PostAction has no UI dispatcher and gets dropped — so start the build
        // synchronously rather than via TriggerBuild()'s deferred PostAction.
        buildPanel->TriggerBuildImmediate();
        return json{{"ok", true}, {"platformName", platformName},
                    {"generation", buildPanel->GetBuildStatus().generation}, {"message", "Build requested"}}; });

server.RegisterHandler("get_build_status", [&app](const EditorDebugServer::RequestContext&) -> json
                           {
        auto* panel = app.m_PanelManager ? app.m_PanelManager->FindFirstPanelOfType<BuildPanel>() : nullptr;
        if (!panel)
            return Editor::RefuseRequest("BuildPanel not found. Open it via the toolbar first.");
        const auto status = panel->GetBuildStatus();
        return json{{"running", status.running}, {"succeeded", status.succeeded},
                    {"outputDirectory", status.outputDirectory.string()},
                    {"generation", status.generation}, {"cancelled", status.progress.cancelled},
                    {"progress", status.progress.progress}, {"message", status.progress.statusMessage},
                    {"errors", status.progress.errors}, {"warnings", status.progress.warnings}}; });

server.RegisterHandler("get_build_settings", [](const EditorDebugServer::RequestContext& /*ctx*/) -> json
                           {
        auto& engine = EngineCore::GetInstance();
        const auto& wsRoot = engine.GetWorkspaceRoot();
        const auto& assetRoot = engine.GetResolvedAssetRoot();

        json result = json::object();
        result["workspaceRoot"] = wsRoot.string();
        result["assetRoot"] = assetRoot.string();
        result["sdkPath"] = (PathUtils::GetExecutableDirectory() / "SDK").string();
        result["sdkLibDebugExists"] = std::filesystem::is_directory(PathUtils::GetExecutableDirectory() / "SDK" / "lib" / "Debug");
        result["sdkLibReleaseExists"] = std::filesystem::is_directory(PathUtils::GetExecutableDirectory() / "SDK" / "lib" / "Release");

        // Count files in asset root
        int assetFileCount = 0;
        std::error_code ec;
        if (std::filesystem::is_directory(assetRoot, ec))
        {
            for (const auto& entry : std::filesystem::recursive_directory_iterator(assetRoot, ec))
                if (entry.is_regular_file()) ++assetFileCount;
        }
        result["assetFileCount"] = assetFileCount;

        auto prefs = wsRoot.empty() ? Editor::OpenEditorPreferences() : Editor::OpenProjectSettings(wsRoot);
        std::string settingsError;
        prefs.Load(&settingsError);

        json buildSettings = json::object();
        buildSettings["settingsPath"] = prefs.GetFilePath().string();
        if (!settingsError.empty())
            buildSettings["settingsError"] = settingsError;

        std::string globalScenesRaw;
        prefs.TryGetString("build.globalScenes", globalScenesRaw);
        buildSettings["globalScenes"] = ParseDebugBuildSceneList(globalScenesRaw);

        if (const auto& rootJson = prefs.Json(); rootJson.is_object())
        {
            const auto itRendering = rootJson.find("rendering");
            if (itRendering != rootJson.end() && itRendering->is_object())
            {
                const auto itPipeline = itRendering->find("activeRenderPipeline");
                if (itPipeline != itRendering->end() && itPipeline->is_string())
                    buildSettings["activeRenderPipeline"] = itPipeline->get<std::string>();
            }
        }

        json platforms = json::array();
        for (const GameEngine::BuildPlatformInfo& info : GameEngine::GetBuildPlatforms())
        {
            const std::string platform(info.Name);
            json platformSettings = json::object();
            platformSettings["name"] = platform;
            if (info.DisplayName != info.Name)
                platformSettings["displayName"] = std::string(info.DisplayName);

            const std::string prefix = "build.platform." + platform + ".";
            bool useGlobalScenes = true;
            prefs.TryGetBool(prefix + "useGlobalScenes", useGlobalScenes);
            platformSettings["useGlobalScenes"] = useGlobalScenes;

            std::string scenesRaw;
            prefs.TryGetString(prefix + "scenes", scenesRaw);
            platformSettings["platformScenes"] = ParseDebugBuildSceneList(scenesRaw);
            platformSettings["effectiveScenes"] = useGlobalScenes
                ? ParseDebugBuildSceneList(globalScenesRaw)
                : ParseDebugBuildSceneList(scenesRaw);

            std::string outputDir;
            prefs.TryGetString(prefix + "outputDir", outputDir);
            platformSettings["outputDir"] = outputDir;

            std::string buildConfig;
            prefs.TryGetString(prefix + "buildConfig", buildConfig);
            platformSettings["buildConfig"] = buildConfig;
            // The configuration a build would actually use. The stored value
            // stays empty until someone edits the field, so reporting it alone
            // says nothing about what this editor would package.
            platformSettings["effectiveBuildConfig"] =
                buildConfig.empty() ? std::string(DefaultPlayerBuildConfig()) : buildConfig;

            platforms.push_back(std::move(platformSettings));
        }
        buildSettings["platforms"] = std::move(platforms);
        result["buildSettings"] = std::move(buildSettings);

        return result; });
}
}
