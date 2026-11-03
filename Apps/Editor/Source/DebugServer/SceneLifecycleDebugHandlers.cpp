#include "DebugHandlerHelpers.h"

namespace GameEngine
{
void RegisterSceneLifecycleDebugHandlers(EditorDebugServer& server, EditorApplication& app)
{
server.RegisterHandler("open_scene", [&app](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        if (!app.m_SceneEditor)
            return Editor::RefuseRequest("Scene editor is not available");

        const std::string pathStr = ctx.params.value("path", "");
        if (pathStr.empty())
            return Editor::RefuseRequest("Missing path");

        const bool additive = ctx.params.value("additive", false);
        const std::string policyStr = ctx.params.value("restorePolicy", "prompt");
        auto policy = Editor::SceneEditorController::OpenRestorePolicy::Prompt;
        if (policyStr == "discard")
            policy = Editor::SceneEditorController::OpenRestorePolicy::Discard;
        else if (policyStr == "restore")
            policy = Editor::SceneEditorController::OpenRestorePolicy::Restore;
        else if (policyStr != "prompt")
            return Editor::RefuseRequest("restorePolicy must be prompt, discard, or restore");

        app.m_SceneEditor->RequestOpenScene(std::filesystem::path(pathStr), additive, policy);
        return json{{"queued", true}, {"path", pathStr}, {"additive", additive},
                    {"restorePolicy", policyStr}}; });

server.RegisterHandler("respond_modal", [&app](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        if (!app.m_SceneEditor)
            return Editor::RefuseRequest("Scene editor is not available");
        const std::string choice = ctx.params.value("choice", "");
        if (choice.empty())
            return Editor::RefuseRequest("Provide choice (save|dontSave|cancel|restore|discard|confirm|ok)");

        const std::string kind = app.m_SceneEditor->GetActiveModalKind();
        std::string error;
        if (!app.m_SceneEditor->RespondToActiveModal(choice, &error))
            return Editor::RefuseRequest(error);
        return json{{"ok", true}, {"modal", kind}, {"choice", choice}}; });

server.RegisterHandler("save_scene", [&app](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        if (!app.m_SceneEditor)
            return Editor::RefuseRequest("Scene editor is not available");

        const std::string pathStr = ctx.params.value("path", "");
        // A degraded scene will not overwrite its own source without this. Automation is the easiest
        // way to destroy such a file silently — no modal stands in the way — so refusing is the
        // default and consent has to be typed.
        const bool force = ctx.params.value("force", false);
        const Editor::DegradedSavePolicy degradedPolicy =
            force ? Editor::DegradedSavePolicy::SaveAnyway : Editor::DegradedSavePolicy::Refuse;
        std::string error;

        if (pathStr.empty())
        {
            // Deliberately NOT the UI-replay command: it calls SaveActiveScene with the default
            // policy and carries no way to pass one, so force= could not reach the guard through it.
            if (!app.m_SceneEditor->SaveActiveScene(&error, degradedPolicy))
                return Editor::RefuseRequest(error.empty() ? "save failed" : error);
            const auto saved = app.m_SceneEditor->GetActiveScenePath();
            return json{{"ok", true}, {"path", saved ? saved->string() : std::string{}}};
        }

        std::filesystem::path scenePath(pathStr);
        if (scenePath.is_relative())
        {
            const auto& assetRoot = EngineCore::GetInstance().GetResolvedAssetRoot();
            if (!assetRoot.empty())
                scenePath = assetRoot / scenePath;
        }
        std::string ext = scenePath.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (ext != ".scene")
            scenePath.replace_extension(".scene");
        scenePath = scenePath.lexically_normal();

        if (!app.m_SceneEditor->SaveActiveSceneAs(scenePath, &error, degradedPolicy))
            return Editor::RefuseRequest(error.empty() ? "save-as failed" : error);
        return json{{"ok", true}, {"path", scenePath.string()}}; });

server.RegisterHandler("set_play_mode", [&app](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        auto* playMode = app.m_PlayMode.get();
        if (!playMode)
            return Editor::RefuseRequest("Play mode is not available");

        const Editor::PlayModeRequest request(ctx.params, playMode->GetState());
        if (!request.Error().empty())
            return Editor::RefuseRequest(request.Error(),
                                         json{{"playMode", Editor::PlayModeStateToString(playMode->GetState())}});

        std::optional<ViewActivation> activation;
        if (request.ActivateGameView())
        {
            if (!app.m_Docking)
                return Editor::RefuseRequest("cannot activate the Game View; the main editor window is not available",
                                             json{{"playMode", Editor::PlayModeStateToString(playMode->GetState())}});

            activation = ActivateDockedView(app, *app.m_Docking, EditorPanelIds::GameView,
                                            /*wantsViewportUiFocus=*/true);
            if (!activation->ok)
                return Editor::RefuseRequest("cannot activate the Game View; the main editor window is not available",
                                             json{{"playMode", Editor::PlayModeStateToString(playMode->GetState())}});
        }

        std::string error;
        if (!app.InvokeUiReplayCommand(request.CommandId(), &error))
            return Editor::RefuseRequest(error.empty() ? std::string("play mode transition failed") : error,
                                         json{{"playMode", Editor::PlayModeStateToString(playMode->GetState())}});

        const Editor::PlayModeState after = playMode->GetState();
        json result{{"playMode", Editor::PlayModeStateToString(after)}};

        // Present only when activation was requested: a caller that did not ask
        // to move its layout has no view state to reconcile.
        if (activation)
        {
            result["gameViewActive"] = activation->active;
            result["gameViewViewportFocused"] = activation->viewportFocused;
        }

        // EnterPlayMode refuses without a world, or while a deferred scene build is
        // still resolving entities, and says so only to the log. Report it rather
        // than answering ok with the editor still sitting in Edit.
        if (request.IsEnter() && after == Editor::PlayModeState::Edit)
            return Editor::RefuseRequest("play mode did not start; no world is open, the scene is still loading, "
                                         "or the pre-play snapshot failed — see the editor log",
                                         std::move(result));

        return result; });
}
}
