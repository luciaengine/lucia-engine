#include "DebugHandlerHelpers.h"

namespace GameEngine
{
void RegisterEditorStateDebugHandlers(EditorDebugServer& server,
                                      EditorApplication& app,
                                      Logger::RingBufferSink* ringBufferSink)
{
server.RegisterHandler("get_editor_state", [&app](const EditorDebugServer::RequestContext& /*ctx*/) -> json
                           {
        json result;

        // Play mode
        auto* playMode = app.m_PlayMode.get();
        if (playMode)
            result["playMode"] = Editor::PlayModeStateToString(playMode->GetState());
        else
            result["playMode"] = "editing";

        if (app.m_SceneEditor)
        {
            if (const auto activeScene = app.m_SceneEditor->GetActiveScenePath())
                result["scenePath"] = activeScene->string();
            else
                result["scenePath"] = "";
            result["sceneDirty"] = app.m_SceneEditor->IsSceneDirty();
            // Blocked-on-modal visibility: scene modals (save changes / restore
            // backup / save as / revert scene) silently park pending opens until
            // answered, so agents need to see them without a screenshot. Answer via
            // respond_modal or avoid them with open_scene restorePolicy. The
            // loadFailed notice parks nothing but blocks execute_command until
            // respond_modal ok dismisses it.
            const std::string modalKind = app.m_SceneEditor->GetActiveModalKind();
            result["modal"] = json{{"open", !modalKind.empty()},
                                   {"kind", modalKind.empty() ? json() : json(modalKind)}};

            // A failed open is otherwise invisible here: open_scene only reports
            // that it QUEUED the request, and an entityCount taken afterwards
            // looks perfectly plausible while the world holds a partial load. A
            // non-null sceneLoadError means the world is not the scene anyone
            // asked for; worldCleared distinguishes "your previous scene is gone"
            // from "the open document survived".
            if (const auto& failure = app.m_SceneEditor->GetLastSceneLoadFailure())
            {
                result["sceneLoadError"] = json{
                    {"path", failure->DocumentPath.string()},
                    {"message", failure->Message},
                    {"file", failure->ErrorFile.string()},
                    {"line", failure->ErrorLine},
                    {"worldCleared", failure->WorldCleared},
                    {"entitiesInWorld", failure->EntitiesInWorld}};
            }
            else
            {
                result["sceneLoadError"] = json();
            }

            // Kept SEPARATE from sceneLoadError on purpose. sceneLoadError means "the world is not
            // the scene you asked for, do not trust scenePath or entityCount". A degraded scene IS
            // the scene you asked for and both of those ARE trustworthy — it is merely missing the
            // assignments this build could not read. Folding the two together would make the
            // documented sceneLoadError contract false for every degraded open.
            //
            // Both records, because they answer different questions and reporting only one made
            // this surface contradict the window title. BuildSceneDegradedReport owns that
            // reconciliation and the shape it produces.
            result["sceneDegraded"] =
                BuildSceneDegradedReport(app.m_SceneEditor->GetLastSceneLoadDegraded(),
                                         app.m_SceneEditor->GetOutstandingSceneLoadDegraded());
        }
        else
        {
            result["scenePath"] = "";
            result["sceneLoadError"] = json();
            result["sceneDegraded"] = json();
        }

        // Content readiness the pixels cannot report. Non-zero means some
        // material is still sampling a bindless default instead of its authored
        // texture, so anything measured from the viewport right now is a
        // transient state — poll to zero before drawing a visual conclusion.
        result["pendingMaterialTextureBinds"] = PendingMaterialTextureBinds();
        // The same question for UI chrome, which the material counter cannot
        // answer: non-zero means that many background images (icons, panel art)
        // have a load or upload in flight and are drawing nothing where they
        // will draw. Zero is a settled UI frame.
        UIManager* chromeUi =
            (!app.m_Windows.empty() && app.m_Windows[0]) ? app.m_Windows[0]->ui.get() : nullptr;
        result["pendingUiBackgroundImages"] =
            chromeUi ? chromeUi->PendingBackgroundImageCount() : 0u;

        // Selected entity — read from the InspectorPanel which displays the
        // currently selected entity across all selection sources.
        auto* inspector = FindPanel<InspectorPanel>(app.m_PanelStorage);
        ECS::EntityHandle selectedEntity = inspector ? inspector->GetInspectedEntity() : ECS::EntityHandle{};
        if (selectedEntity.IsValid())
        {
            auto* world = EngineCore::GetInstance().GetPrimaryWorld();
            result["selectedEntity"] = {
                {"id", selectedEntity.id},
                {"name", EntityNameFromWorld(world, selectedEntity)}
            };
        }
        else
        {
            result["selectedEntity"] = nullptr;
        }

        // Window count
        result["windowCount"] = static_cast<int>(app.m_Windows.size());
        if (!app.m_Windows.empty() && app.m_Windows[0] && app.m_Windows[0]->window)
        {
            int wx = 0;
            int wy = 0;
            int ww = 0;
            int wh = 0;
            app.m_Windows[0]->window->GetPosition(wx, wy);
            app.m_Windows[0]->window->GetWindowSize(ww, wh);
            result["mainWindow"] = {
                {"x", wx},
                {"y", wy},
                {"width", ww},
                {"height", wh}
            };
            if (app.m_Windows[0]->ui)
            {
                result["focusedElementId"] = app.m_Windows[0]->ui->GetFocusedElementId();
                result["hoveredElement"] = app.m_Windows[0]->ui->GetHoveredElementDebugName();
                result["mouseCaptured"] = app.m_Windows[0]->ui->IsMouseCaptured();
                result["captureElementId"] = app.m_Windows[0]->ui->GetCaptureId();
            }
        }

        // Entity count
        auto* world = EngineCore::GetInstance().GetPrimaryWorld();
        result["entityCount"] = world ? static_cast<uint64_t>(world->GetEntityCount()) : 0;

        // Session provenance: who this editor is, so an agent can answer "may I touch
        // it?" from one query instead of the process table. Every field is stamped at
        // launch; an unknown field is null, never a guess. `branchAtLaunch` is the
        // branch this binary was BUILT from — the tree may have been switched since,
        // which is exactly why `worktreePath` is the identity that cannot drift.
        const auto& session = app.m_SessionDescriptor;
        auto optionalString = [](const std::optional<std::string>& value) -> json
        { return value.has_value() ? json(*value) : json(nullptr); };
        result["session"] = {
            {"label", optionalString(session.SessionLabel)},
            {"worktreePath", session.WorktreePath.has_value() ? json(session.WorktreePath->string()) : json(nullptr)},
            {"worktreeName", optionalString(session.WorktreeName)},
            {"branchAtLaunch", optionalString(session.BranchAtLaunch)},
            {"buildConfig", optionalString(session.BuildConfig)}
        };

        return result; });

server.RegisterHandler("move_window", [&app](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        const int windowIndex = ctx.params.value("windowIndex", 0);
        if (windowIndex < 0 || windowIndex >= static_cast<int>(app.m_Windows.size()))
            return Editor::RefuseRequest("windowIndex out of range");

        auto* wndCtx = app.m_Windows[static_cast<size_t>(windowIndex)].get();
        if (!wndCtx || !wndCtx->window)
            return Editor::RefuseRequest("window unavailable");

        int x = 0;
        int y = 0;
        int width = 0;
        int height = 0;
        wndCtx->window->GetPosition(x, y);
        wndCtx->window->GetWindowSize(width, height);

        bool resizeRequested = ctx.params.contains("width") || ctx.params.contains("height");

        // Optional "monitor" index: place the window on that display's work area.
        // Defaults size to fill the target monitor unless the caller overrides width/height.
        if (ctx.params.contains("monitor"))
        {
            const int monitorIndex = ctx.params.value("monitor", 0);
            const auto monitors = Platform::EnumerateMonitors();
            const auto it = std::find_if(monitors.begin(), monitors.end(),
                                         [monitorIndex](const Platform::MonitorInfo& m)
                                         { return m.index == monitorIndex; });
            if (it == monitors.end())
                return Editor::RefuseRequest("monitor index out of range", json{{"monitorCount", monitors.size()}});
            x = it->workX;
            y = it->workY;
            if (!resizeRequested)
            {
                width = it->workWidth;
                height = it->workHeight;
                resizeRequested = true;
            }
        }

        x = ctx.params.value("x", x);
        y = ctx.params.value("y", y);
        width = ctx.params.value("width", width);
        height = ctx.params.value("height", height);

        wndCtx->window->SetPosition(x, y);
        if (resizeRequested && width > 0 && height > 0)
            wndCtx->window->SetWindowSize(width, height);
        wndCtx->window->Focus();

        int outX = 0;
        int outY = 0;
        int outW = 0;
        int outH = 0;
        wndCtx->window->GetPosition(outX, outY);
        wndCtx->window->GetWindowSize(outW, outH);
        return json{{"ok", true},
                    {"windowIndex", windowIndex},
                    {"x", outX},
                    {"y", outY},
                    {"width", outW},
                    {"height", outH}}; });

server.RegisterHandler("get_log", [ringBufferSink](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        if (!ringBufferSink)
            return Editor::RefuseRequest("Log ring buffer sink not available");

        size_t count = ctx.params.value("count", 100);
        size_t offset = ctx.params.value("offset", 0);
        std::string minLevelStr = ctx.params.value("minLevel", "info");
        std::string substring = ctx.params.value("filter", "");
        Logger::LogLevel minLevel = Logger::StringToLogLevel(Logger::String(minLevelStr));

        // Returns the most recent `count` matches (the tail of the log),
        // oldest-first within that window. `offset` pages further back.
        auto messages = ringBufferSink->GetMessages(minLevel, count, Logger::String(substring), offset);

        json result = json::array();
        for (auto& msg : messages)
        {
            result.push_back({
                {"level", Logger::LogLevelToString(msg.Level)},
                {"message", msg.Message},
            });
        }
        return result; });

server.RegisterHandler("focus_view", [&app](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        const Editor::ViewFocusRequest request(ctx.params);
        if (!request.Error().empty())
            return Editor::RefuseRequest(request.Error());

        if (!app.m_Docking)
            return Editor::RefuseRequest("Main editor window is not available");

        const std::string panelId = request.PanelId();
        const ViewActivation activation =
            ActivateDockedView(app, *app.m_Docking, panelId, request.WantsViewportUiFocus());
        if (!activation.ok)
            return Editor::RefuseRequest("Main editor window is not available");

        return json{{"panelId", panelId},
                    {"active", activation.active},
                    {"viewportFocused", activation.viewportFocused},
                    {"focusedElementId", activation.focusedElementId}}; });

server.RegisterHandler("set_scroll", [&app](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        if (app.m_Windows.empty())
            return Editor::RefuseRequest("No editor window");
        const std::string elementId = ctx.params.value("elementId", std::string());
        if (elementId.empty())
            return Editor::RefuseRequest("set_scroll requires 'elementId'");
        auto* wndCtx = app.m_Windows[0].get();
        if (!wndCtx->ui)
            return Editor::RefuseRequest("No UIManager");
        UIElement* root = wndCtx->ui->GetRootElement();
        if (!root)
            return Editor::RefuseRequest("No root element");
        UIElement* el = root->FindById(elementId);
        // Docked panel content is owned by the docking model and may be
        // mounted through a portal rather than beneath the window root.
        // Match get_panel_tree's reach so MCP can drive virtualized panel
        // controls such as assets-grid.
        if (!el && app.m_Docking)
        {
            for (const auto& [_, panel] : app.m_Docking->GetPanels())
            {
                if (panel && (el = panel->FindById(elementId)))
                    break;
            }
        }
        if (!el)
            return Editor::RefuseRequest("Element not found: " + elementId);
        // Find a ScrollView at or below the targeted element. Most virtualized
        // controls (ListView/TreeView/GridView) own a ScrollView descendant.
        std::function<ScrollView*(UIElement*)> findScroll = [&](UIElement* node) -> ScrollView*
        {
            if (auto* sv = dynamic_cast<ScrollView*>(node))
                return sv;
            for (auto& child : node->GetChildren())
                if (auto* found = findScroll(child.get()))
                    return found;
            return nullptr;
        };
        ScrollView* sv = findScroll(el);
        if (!sv)
            return Editor::RefuseRequest("No ScrollView at or below target");
        if (ctx.params.contains("x"))
            sv->SetScrollX(ctx.params.value("x", 0.0f));
        if (ctx.params.contains("y"))
            sv->SetScrollY(ctx.params.value("y", 0.0f));
        return json{{"ok", true},
                    {"scrollX", sv->GetScrollX()},
                    {"scrollY", sv->GetScrollY()},
                    {"contentHeight", sv->GetContentHeight()},
                    {"viewportHeight", sv->GetViewportHeight()}}; });

server.RegisterHandler("shutdown", [&app](const EditorDebugServer::RequestContext& /*ctx*/) -> json
                           {
        app.RequestExit();
        return json{{"ok", true}}; });

server.RegisterHandler("undock_panel", [&app](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        const std::string panelId = ctx.params.value("panelId", "");
        if (panelId.empty())
            return Editor::RefuseRequest("panelId required");
        app.QueueUndockPanel(panelId);
        return json{{"ok", true}, {"panelId", panelId}}; });

server.RegisterHandler("execute_command", [&app](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        // Map well-known string names to command IDs. Play-mode transitions are not
        // here: set_play_mode owns them, because it reports the resulting state and
        // refuses an illegal transition instead of answering ok to a no-op.
        static const std::unordered_map<std::string, uint32_t> kNamedCommands = {
            {"create_empty",          0x4101u},
            {"create_cube",           0x4110u},
            {"create_sphere",         0x4111u},
            {"create_capsule",        0x4112u},
            {"create_plane",          0x4113u},
            {"create_camera",         0x4120u},
            {"create_light_dir",      0x4130u},
            {"create_light_point",    0x4131u},
            {"create_light_spot",     0x4132u},
            {"create_light_ambient",  0x4133u},
            // Every entry of the Create/Terrain menu, so automation can reach the planet
            // presets by name; create_terrain is the small planar default.
            {"create_terrain",        0x4140u},
            {"create_terrain_large",  0x4147u},
            {"create_planet_5km",     0x4148u},
            {"create_planet_50km",    0x4149u},
            {"create_particle_emitter", 0x4142u},
            {"assets_view_grid",      0xF0A10101u},
            {"assets_view_list",      0xF0A10102u},
            {"reimport_lods",         0xF0B10001u},
            {"bake_hlod",             0xF0B10002u},
            {"save_scene",            0xF0B10003u},
        };

        uint32_t commandId = 0;

        if (ctx.params.contains("commandId"))
        {
            commandId = ctx.params["commandId"].get<uint32_t>();
        }
        else if (ctx.params.contains("name"))
        {
            std::string name = ctx.params["name"].get<std::string>();
            auto it = kNamedCommands.find(name);
            if (it == kNamedCommands.end())
                return Editor::RefuseRequest("Unknown command name: " + name);
            commandId = it->second;
        }
        else
        {
            return Editor::RefuseRequest("Provide commandId (uint32) or name (string)");
        }

        // A visible scene modal parks pending opens/saves; dispatching editor
        // commands underneath it acts on half-committed document state, and
        // several command flows raise their own prompts on top. Reject with
        // the modal kind so the caller answers it first (respond_modal /
        // open_scene restorePolicy).
        if (app.m_SceneEditor)
        {
            const std::string modalKind = app.m_SceneEditor->GetActiveModalKind();
            if (!modalKind.empty())
                return Editor::RefuseRequest("Editor is blocked on the '" + modalKind
                           + "' modal — answer it (respond_modal) before execute_command");
        }

        std::string error;
        bool ok = app.InvokeUiReplayCommand(commandId, &error);
        if (!ok)
            return Editor::RefuseRequest(error.empty() ? "Command failed" : error);

        return json{{"ok", true}, {"commandId", commandId}}; });
}
}
