#include "DebugHandlerHelpers.h"
#include "SceneLifecycleDebugHandlers.h"
#include "EditorStateDebugHandlers.h"
#include "CrashDebugHandlers.h"
#include "ShadowDebugHandlers.h"
#include "GraphDebugHandlers.h"
#include "ModelSpawnDebugHandlers.h"
#include "CameraDebugHandlers.h"
#include "SceneEntityDebugHandlers.h"
#include "RenderStatsDebugHandlers.h"
#include "CaptureDebugHandlers.h"
#include "DisplayOutputDebugHandlers.h"
#include "RenderGraphDebugHandlers.h"
#include "ProfilerDebugHandlers.h"
#include "UndoDebugHandlers.h"
#include "MovieRecordDebugHandlers.h"
#include "MeshLodDebugHandlers.h"
#include "UiAutomationDebugHandlers.h"
#include "SceneToolDebugHandlers.h"
#include "AssetDebugHandlers.h"
#include "BuildDebugHandlers.h"
#include "MemoryDebugHandlers.h"
#include "JobSystem/JobSystemDebugHandlers.h"

namespace GameEngine
{
void RegisterDebugHandlers(EditorDebugServer& server,
                           EditorApplication& app,
                           Logger::RingBufferSink* ringBufferSink)
{
    RegisterSceneLifecycleDebugHandlers(server, app);
    RegisterEditorStateDebugHandlers(server, app, ringBufferSink);
    RegisterCrashDebugHandlers(server);
    RegisterShadowDebugHandlers(server, app);
    RegisterGraphDebugHandlers(server, app);
    RegisterModelSpawnDebugHandlers(server, app);
    RegisterCameraDebugHandlers(server, app);
    RegisterSceneEntityDebugHandlers(server, app);
    RegisterRenderStatsDebugHandlers(server, app);
    RegisterCaptureDebugHandlers(server, app);
    RegisterDisplayOutputDebugHandlers(server, app);
    RegisterRenderGraphDebugHandlers(server, app);
    RegisterProfilerDebugHandlers(server, app);
    RegisterUndoDebugHandlers(server, app);
    RegisterMovieRecordDebugHandlers(server, app);
    RegisterMeshLodDebugHandlers(server);
    RegisterUiAutomationDebugHandlers(server, app);
    RegisterSceneToolDebugHandlers(server, app);
    RegisterAssetDebugHandlers(server, app);
    RegisterBuildDebugHandlers(server, app);
    RegisterMemoryDebugHandlers(server, app);
    RegisterJobSystemDebugHandlers(server);
    // 1. get_editor_state


    // 1b. move_window
    // Moves/resizes an editor window through the engine's own platform layer.
    // Useful for monitor/HDR testing on macOS where external Apple Events may
    // be denied by TCC.


    // 2. get_log


    // 3. get_scene_hierarchy


    // open_scene
    //   path:          required scene path.
    //   additive:      optional (default false).
    //   restorePolicy: optional "prompt" (default) | "discard" | "restore" —
    //                  headless answer for the modals an open can raise. The
    //                  dirty-scene prompt resolves as Don't Save for both
    //                  non-prompt values; the crash-recovery prompt discards or
    //                  restores the autosave staging. With "prompt", a raised
    //                  modal silently blocks the open — poll get_editor_state
    //                  (modal field) and answer via respond_modal.
    // A failed open raises the loadFailed notice whatever the policy, because
    // the user at the editor needs it as much as the caller; the caller reads
    // the same failure from get_editor_state.sceneLoadError and dismisses the
    // notice with respond_modal ok.


    // respond_modal
    // Answers the currently-visible scene modal (the ones that block scene
    // opens/saves) by choice name, equivalent to clicking its button:
    //   saveChanges   -> save | dontSave | cancel
    //   restoreBackup -> restore | discard
    //   saveAs        -> cancel
    //   revertScene   -> confirm | cancel
    //   loadFailed    -> ok
    // get_editor_state reports which modal is up under "modal".


    // save_scene — synchronous save of the active scene document.
    //   path present -> Save As to that path (project-relative resolves under the
    //                   asset root, .scene enforced), adopting it as the document path.
    //   path absent  -> Save to the current scene path (fails if the document is untitled).
    // Routes through SceneDocumentManager::Save / SaveAs, so hierarchy-UI capture +
    // MarkClean happen exactly as the Ctrl+S / File menu path does.


    // set_play_mode — drive the editor's Play / Pause / Stop actions.
    //   action: "enter" | "exit" | "pause" | "resume".
    //   activateGameView: optional bool, default false.
    // Routed through the same UI-replay commands the toolbar buttons use, so the
    // play-fullscreen teardown and toolbar state stay in sync with the transition.
    // Each action is legal from exactly one state and refused from any other, and
    // the response always carries the resulting "playMode" so a caller never has
    // to trust its own intent.
    //
    // activateGameView raises the Game View through the same path focus_view
    // takes, before the transition: the Game View's composite is what publishes
    // the gameplay UI host, so a HUD needs that tab up to have anywhere to mount.
    // No main window means the view can never come up, so that refuses the whole
    // transition instead of entering play with the caller believing it did; the
    // reached-the-dock-model case reports "gameViewActive" read back instead.
    //
    // Nothing is saved on the caller's behalf. Entering snapshots the world and
    // exiting restores that snapshot, so runtime mutations are discarded — see the
    // tool description for the full contract.


    server.RegisterHandler("dock_panel", [&app, &server](const EditorDebugServer::RequestContext& ctx) -> json
    {
        Editor::DockPanelRequest request(ctx.params);
        if (!request.Error().empty())
            return Editor::RefuseRequest(request.Error());
        if (!app.m_PanelManager || app.m_Windows.empty() || !app.m_Docking)
            return Editor::RefuseRequest("Main editor window is not available");
        DockingManager& docking = *app.m_Docking;
        auto* window = app.m_Windows[0].get();
        UIElement* root = window->ui ? window->ui->GetRootElement() : nullptr;
        auto* dockspace = root ? dynamic_cast<DockspaceElement*>(root->FindById("dock")) : nullptr;
        if (!dockspace)
            return Editor::RefuseRequest("Main editor dockspace is not available");
        if (!request.Apply(docking, [&app, window](const std::string& panel, const std::string& anchor) {
                app.m_PanelManager->ShowOrActivatePanel(window, panel, anchor);
            }))
            return Editor::RefuseRequest(request.Error(), request.Report(docking));
        dockspace->RequestRebuildFromModel();
        server.EnqueueDeferredResponse(
            ctx.id, [&docking, request = std::move(request), framesToSkip = 1](json& result) mutable {
                if (framesToSkip-- > 0)
                    return false;
                result = request.Report(docking);
                if (!result["docked"].get<bool>())
                    result = Editor::RefuseRequest("panel left the dock tree before layout readback", result);
                return true;
            });
        return EditorDebugServer::DeferredMarker();
    });

    // focus_view — bring the Scene View or Game View tab to the front of its dock
    // group in the main window and give that view the input focus it expects.
    //   view: "scene" | "game".
    // "active" and "focusedElementId" are read back from the dock model and the UI
    // manager after the fact, never assumed from the request.


    // 4. get_entity_components


    // 4b. terrain-specific diagnostics + sculpt stroke driver
    RegisterTerrainDebugHandlers(server, app);

    // 4c. renderer scale-bench spawner (Tools/Benchmarks/Rendering/perf-bench)
    RegisterBenchSceneHandlers(server);

    // 4d. physics world queries (the collision oracle for automated checks)
    RegisterPhysicsDebugHandlers(server);
    RegisterTileMapDebugHandlers(server, app);

    // 4e. composed-ground queries (the ground oracle for placement checks)
    RegisterGroundQueryDebugHandlers(server);

    // 4e2. the sky's per-frame inputs (the readback for comparing two runs input by input)
    RegisterSkyDebugHandlers(server);

    // 4e3. the Parallax steps debug view (height samples per pixel of each relief march)
    RegisterParallaxDebugHandlers(server);

    // 4e3b. world mark-ups: the agent's side of the visual conversation
    RegisterMarkupDebugHandlers(server, app);

    // 4e4. folder thumbnail bakes (the Assets panel's Generate Thumbnails)
    RegisterThumbnailDebugHandlers(server, app.m_ThumbnailProvider);

    // 4f. GPU vendor, which graphics tools are attached to the live device, and
    //     Nsight Graphics capture triggering
    RegisterGpuToolingHandlers(server, app);

    // 4g. an assistant session's binding and its permission for actions outside the
    //     editor, answered by the gate that claims them
    RegisterAssistantSessionHandlers(server);

    // 5. query_ecs


    // 6. get_ui_tree


    // get_panel_tree — resolve a docked panel by name and return its UI subtree.
    // Unlike get_ui_tree with rootId, this finds the actual panel content regardless
    // of where it sits in the dockspace layout (bypasses dock-config declarations).


    // 7. click_element
    // Simulates a click by pressing on this frame and releasing on the next.
    // UIManager dispatches each transition on arrival, so a same-frame
    // press+release would also land; scheduling the release explicitly keeps the
    // injected gesture deterministic — handlers observe a held button for
    // exactly one frame, which is what a drag-arming control needs to see.


    // move_pointer
    // Moves the UI pointer without pressing a button so MCP clients can
    // inspect hover behavior and capture stable visual evidence. The move
    // enters the window's input routing and nothing else: hover, tooltips and
    // scene-view hover all read the routed pointer, and the operating-system
    // cursor stays wherever the person using the machine has it.


    // ---- Context-menu automation -------------------------------------------
    // Native popup menus (Win32 TrackPopupMenuEx / NSMenu) run a nested OS
    // message loop that freezes the main loop — and this debug server — until
    // the menu closes, so menu flows previously required OS-level input.
    // Instead of pumping IPC from inside the nested loop (handlers would run
    // against a dead RGFrame capture context and re-entrant UI state), menus
    // are driven programmatically: open_context_menu arms a one-shot
    // interceptor and performs the click; the InterceptableContextMenu
    // decorator (which CreateContextMenu wraps around every editor menu)
    // hands over the state-resolved item tree plus a copy of the command
    // callback instead of opening the popup; invoke_menu_item runs the chosen
    // command. Contract: the capture stays valid until the next
    // open_context_menu or invoke_menu_item; invoke promptly — callbacks
    // capture their owning panels, so a scene or layout change between
    // capture and invoke acts on stale context (same risk class as
    // click_element on a rebuilt UI). A command that itself opens another
    // menu reverts to the popup (interceptor is one-shot) and will stall IPC
    // as before.
    // open_context_menu
    // Right-clicks (default) the target and returns the context menu's item
    // tree instead of opening the OS popup. Targeting mirrors click_element,
    // plus a pooled-row mode for virtualized lists (TableView/ListView), whose
    // recycled row widgets have no per-logical-row element ids:
    //   elementId | x,y      : click location, or
    //   tableId + rowIndex   : pooled list automation id (the ListView, its
    //                          TableView, or a container with exactly one list)
    //                          + logical provider row index. The row is scrolled
    //                          into view and its live cell is clicked exactly
    //                          like a user right-click.
    //   button               : "right" (default) or "left" (toolbar dropdowns).
    //   windowIndex          : optional.
    // Response: {items:[{path, commandId|submenu|separator, enabled, checked?, icon?, color?}],
    //            x, y, generation, row?:{index, tableId}}. The row field is
    //            also reported when plain x/y targeting lands on a pooled row.
    //            Follow with invoke_menu_item.


    // get_context_menu
    // Returns the currently captured menu (from the last open_context_menu),
    // or {open:false} when none is pending.


    // invoke_menu_item
    // Invokes a command on the captured menu through a copy of the menu's
    // command callback — equivalent to the user clicking that row in the
    // popup. The capture is consumed on success.
    //   commandId : integer from open_context_menu, or
    //   path      : exact item path ("Create/Entity") or unambiguous leaf title.


    // set_text_field
    // Sets a TextField's value by element id. send_key cannot type punctuation
    // (paths, URLs), so text-entry flows (Package Manager add/relocate/re-pin,
    // future search/rename automation) are driven by setting the field value
    // directly — the field's click handlers read GetValue() exactly as they
    // would after real typing.


    // input_text
    // Types a string through the UI system's real text-input path: one
    // UIManager::OnChar per codepoint — the same entry point the platform
    // layer feeds from WM_CHAR / glfwSetCharCallback — so any focused editable
    // (TextField, float/int field editors, search fields) receives it exactly
    // as if typed, including the per-keystroke value-changed callbacks that
    // set_text_field bypasses. Punctuation and non-ASCII are fine (UTF-8).
    //   text:      required UTF-8 string. Control characters are rejected —
    //              use commit:true for Enter, send_key for Tab/Escape.
    //   elementId: optional element to focus first (same focus path as
    //              clicking it). Omitted: current focus.
    //   clear:     select-all before typing (default false), so the text
    //              replaces the current content. Needed for free-text fields,
    //              which keep their caret on a mouse-equivalent focus; value
    //              fields select-all on focus and replace either way.
    //   commit:    press Enter after the text (default false) — commits
    //              numeric field editors and fires Enter-bound callbacks.
    // Focus-in, select-all, chars, and the Enter press all queue as pending
    // UI inputs and dispatch in order inside the next UIManager::Update, so a
    // single call reproduces the full click-and-type interaction.


    // send_key
    // Injects a keyboard event through WindowInputRouter, the path a real key
    // takes: the window's global shortcut interception (onKeyPre), then the
    // chrome UI, then a live play surface's HUD and gameplay, then the editor's
    // own actions. So an injected Ctrl+W closes what a typed Ctrl+W closes and
    // an injected W walks the character a typed W walks — which is the whole
    // point of driving the editor over this protocol. Text-entry behaviour that
    // needs no chain (Tab traversal, Enter commit, Escape cancel) is unaffected:
    // the focused element still answers first.
    //   key:    a name (Tab, Enter, Return, NumpadEnter, Escape, Space,
    //           Backspace, Delete, Home, End, Up, Down, Left, Right, A-Z, 0-9)
    //           or a raw GLFW keycode integer.
    //   mods:   optional shift|control|alt|super as a string, array of strings,
    //           or an integer GLFW mod mask.
    //   action: "tap" (default; press now + release next frame), "press",
    //           "release", or "repeat" (OS key-repeat edge, e.g. held Tab).
    //   count:  tap only — number of taps to issue (e.g. Tab x3). Default 1.


    // send_text — inject UTF-8 text into the currently focused UI control.
    // This mirrors the platform character callback and complements send_key,
    // which is intentionally limited to physical key press/release events.


    // Open the global palette directly, naming the intent rather than relying on
    // a key binding staying bound to it.


    // simulate_mouse_drag
    // Drives a press-move-release drag across frames so controls that arm on
    // press, begin dragging on later move, and commit on release see the same
    // event sequence as a real mouse gesture. button defaults to left; middle
    // and right match click_element. holdBeforeReleaseFrames keeps the button
    // down at the end so a concurrent screenshot can observe in-flight drag.


    // perform_drop
    // Drops asset files at a point in a window, through that window's drag-and-drop manager
    // (PerformAssetFileDrop): the drop target under the point runs its own CanDrop and
    // PerformDrop, exactly as for a drop made with the mouse, without a pointer gesture to
    // start the drag. Files outside the asset sources are refused, never moved in.
    server.RegisterHandler("perform_drop", [&app](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        const int windowIndex = ctx.params.value("windowIndex", 0);
        if (windowIndex < 0 || windowIndex >= static_cast<int>(app.m_Windows.size()))
            return Editor::RefuseRequest("windowIndex out of range");
        auto* wndCtx = app.m_Windows[static_cast<size_t>(windowIndex)].get();
        if (!wndCtx || !wndCtx->ui || !wndCtx->ui->GetRootElement())
            return Editor::RefuseRequest("No UI manager available");

        if (!ctx.params.contains("paths") || !ctx.params["paths"].is_array() || ctx.params["paths"].empty())
            return Editor::RefuseRequest("Provide paths: an array of asset paths, absolute or project-relative");
        const AssetManager& assetManager = EngineCore::GetInstance().GetAssetManager();
        std::vector<std::filesystem::path> paths;
        for (const json& entry : ctx.params["paths"])
        {
            if (!entry.is_string())
                return Editor::RefuseRequest("paths must hold strings");
            const std::string requested = entry.get<std::string>();
            const std::filesystem::path resolved = Editor::ResolveOpenAssetPath(assetManager, requested);
            if (resolved.empty())
                return Editor::RefuseRequest("Cannot resolve asset path: " + requested);
            paths.push_back(resolved);
        }
        std::vector<std::filesystem::path> sourceRoots;
        for (const AssetSourceDesc& source : assetManager.GetRegisteredSources())
            sourceRoots.push_back(source.Root);

        float x = 0.0f;
        float y = 0.0f;
        if (ctx.params.contains("elementId"))
        {
            if (!ctx.params["elementId"].is_string())
                return Editor::RefuseRequest("elementId must be a string: an element id from get_ui_tree");
            json targetError;
            if (!Editor::ResolveElementPointerTarget(wndCtx->ui->GetRootElement(),
                                                     ctx.params["elementId"].get<std::string>(), x, y, targetError))
                return targetError;
        }
        else if (ctx.params.contains("x") && ctx.params.contains("y"))
        {
            json coordError;
            if (!Editor::ReadPointerCoords(ctx.params, "x", "y", x, y, coordError))
                return coordError;
        }
        else
        {
            return Editor::RefuseRequest("Provide elementId or x/y");
        }

        const Editor::DropOutcome outcome = Editor::PerformAssetFileDrop(*wndCtx->ui, paths, sourceRoots, x, y);
        if (!outcome.Refusal.empty())
            return Editor::RefuseRequest(outcome.Refusal);
        json pathsOut = json::array();
        for (const auto& path : paths)
            pathsOut.push_back(path.lexically_normal().string());
        // "accepted": the target took the payload and its PerformDrop ran; PerformDrop reports
        // no result, so whether the drop did what it should is read from its effect.
        return json{{"accepted", outcome.Accepted}, {"reason", outcome.Reason},
                    {"target", outcome.TargetElement}, {"x", x}, {"y", y}, {"paths", pathsOut}}; });

    // simulate_double_click_drag
    // First click down/up, then a second press on the same spot (timeline double-click
    // insert) while held, move, and release — matches create-key-then-drag in one gesture.


    // 8. create_entity


    // 8b. spawn_model — instantiate a ModelAsset (FBX/glTF) into the scene
    // and optionally attach an AnimationClip to its skinned entities. Wraps
    // ModelEntityFactory so AI agents can drive the same flow as a manual
    // FBX drop into the scene view. Uses the deferred response pattern so
    // async asset loads don't deadlock the IPC main-thread.


    // 8c. spawn_humanoid_test_pair — synchronous, main-thread spawn of a Model
    // entity (with optional AnimationClip wired into AnimatorRef on every
    // skinned submesh). Bypasses the deferred-response path of `spawn_model`
    // which times out before first-time FBX import completes. Loads assets
    // synchronously via LoadAssetAsync(...).get() (same pattern used by
    // ModelRenderSetup / RenderServices). Intended for autonomous test
    // scaffolding (humanoid retargeting scenarios).


    // 8d. export_model_materials — invoke Editor::ExportModelMaterials on an
    // already-spawned model. Test scaffolding for the provenance integration
    // test. The drag-drop UI path
    // in SceneViewPanel / HierarchyPanel is what normally calls
    // ExportModelMaterials, but those paths require UI events to fire — this
    // RPC lets a test harness run the same flow over IPC.
    //
    // Caller is expected to have spawned the model first (e.g. via
    // spawn_humanoid_test_pair) and passes the resulting submesh entity IDs
    // plus the model's asset-relative path.


    // 8e. enumerate_provenance — read all rows from the v6 provenance table
    // on the project source's SQLite cache. Test scaffolding for the
    // provenance integration test; lets the harness verify the importer
    // call sites wrote the expected rows.


    // 9. set_component


    // set_entity_enabled — switch an entity on or off exactly like the Hierarchy panel's icon:
    // the entity's own state (Editor::CommitEntityEnabledToggle, one undo step), announced as a
    // world-structure change. Its descendants keep their own state and follow through the
    // hierarchy pass on the next update. {"entityId": id, "enabled": bool}.


    // 10. delete_entity — routes through the editor undo system exactly like the
    // Hierarchy / Scene-View delete key (DeleteEntitiesCommand over the collected
    // subtree, preserve-handle). This lands on the undo stack so a subsequent
    // `undo` / `redo` reproduces the real user path (needed for the camera-delete
    // → undo repro; a plain DestroyEntityImmediate could not be undone).


    // 10b. undo / redo — drive the real editor undo stack, identical to Ctrl+Z /
    // Ctrl+Y and the toolbar buttons (EditorApplication::Undo/Redo → UndoRedoService).
    // Runs on the main thread via FlushPendingRequests (same phase as keyboard input).




    // 10c. get_undo_stack — dump the undo/redo stacks for inspection. Each entry
    // reports its command type (DeleteEntitiesCommand vs WorldSnapshotCommand vs …),
    // a human label, and (undo side) a wall-clock timestamp. `cursor` is the number
    // of applied commands (the boundary between the undo and redo stacks).


    // 11. select_entity


    // 12. get_validation_stats — exact per-VUID validation-layer telemetry from the
    // device's ValidationStatsStore. Immune to log-ring eviction and log thinning.
    // Agent workflow: {reset:true} -> run phase -> read -> assert delta == 0.


    // 12b. get_render_stats


    // get_ui_clip_stats
    // Live persistent clip-slot usage per UI window (used = currently
    // allocated, total = allocator high-water). Clip slots share a 16-bit
    // index space with the clip SSBO; this tracks pressure from
    // overflow:hidden containers and overflowing-text self-clips.


    // get_ui_profile_history
    // Returns the full UpdateProfileFrame ring buffer (last ~120 frames) for
    // each UI window. Intended for offline spike analysis — a polling client
    // can locate the exact mutation frame by scanning for max(totalMs) and
    // inspect its full counter set without relying on burst-sampling tricks.
    //
    // Response:
    //   { windows: [ { frames: [ <UpdateProfileFrame fields>, ... ] } ] }
    //
    // Note: requires GE_UI_UPDATE_PROFILE=1 at launch (no frames otherwise).


    // 13. take_screenshot
    // Screenshot via async GPU readback.
    //
    // Targets (parameter "target"):
    //   "window"   — full editor window (UI + scene viewport composited). Default.
    //   "viewport" — scene viewport texture only (legacy behavior).
    //   "panel"    — full window readback cropped to the dock panel rect ("panelId").
    //   "element"  — full window readback cropped to a UI element rect ("elementId").
    //   "rect"           — full window readback cropped to a custom rect ("x","y","w","h").
    //   "asset_preview"  — live video preview texture from Asset View (direct GPU readback).
    //
    // For window/panel/element/rect targets we readback the editor's presentation target.
    // asset_preview bypasses the UI composite and reads the video player's GPU texture.
    //
    // The readback is declared into the RenderGraph frame at the right point — after all
    // normal passes are declared but before Execute. We use EnqueueRenderCallback()
    // which fires from the frame driver pre-Execute on the pure RenderGraph path. A deferred
    // poll then checks the ticket's TryGet() each frame until the GPU copy completes.


    // Shadow/draw-stream readback diagnostics: returns compact hashes only.
    // Intended for backend-specific flicker investigations where screenshots
    // show instability but CPU-side culling/cascade data is stable.


    // Programmatic scroll: drives a ScrollView's scroll offset directly.
    // Useful for debug repros of virtualization regressions (e.g. take_screenshot
    // after scrolling). Resolves the target by elementId (must reference a
    // ScrollView or an ancestor whose first ScrollView descendant is targeted).


    // 14. shutdown
    // Gracefully exits the editor. Useful to release PDB locks before rebuilds.


    // Undocks a panel into a floating OS window — exercises the multi-window
    // render-target path from the test harness.


    // 15. execute_command
    // Dispatches a named editor command by its numeric ID or well-known name.
    // This reuses the UI replay command infrastructure.


    // Batch-import ProFlares-authored lens flare content (TexturePacker atlas
    // .txt + PNG pairs, flare .txt docs) from an external folder into the open
    // project as engine-native .flareatlas/.lensflare assets. First (and so far
    // only) caller of LensFlareImport::ImportFolder — the same entry point a
    // future asset-browser menu action will call.












    // Select by logical name instead of a visible tree-row element. This remains
    // deterministic when the Smart Folders section is collapsed or off-screen.






    // Movie recording automation: drives GameViewController's recording
    // directly (the panel UI flow is interaction-heavy and its embedded
    // variant swallows failure statuses). action=start|stop|status.
    // The debug recording path bypasses MovieRecorderPanel's normal start/stop
    // callbacks, so retain the requested path and publish it when stopping.



    // 16. set_scene_tool
    // Switch the active scene view tool.


    // 16b. set_terrain_brush — drive brush radius/strength/mode from a stroke harness.
    // Params (all optional): radius (world m), strength, mode ("raise"|"lower"|"paint").


    // 16c. set_gizmos_visibility — show or hide the scene view's gizmo overlays so a
    // screenshot shows the scene rather than the editor's light rings and transform
    // handles.
    // Params (all optional bools): "all" is the master gate — "light", "transform" and
    // "markups" only take effect while it is true. An omitted group keeps its current state, so
    // a request with no params reports the state without changing it.
    // Writes every controller in the main window: the render coordinator copies this
    // state from the active viewport slot onto the others each frame, so writing only
    // the main-slot controller would be reverted on the next frame in quad view.


    // 17. get_scene_tool
    // Query the currently active scene view tool.


    // ── Render Graph introspection ──────────────────────────────────────────────
    //
    // These read the MAIN window's live RGFrame at request time (see the
    // MainRgFrame helpers above). The frame holds the previous frame's compiled
    // graph — a stable snapshot, since the next BeginFrame (which clears it)
    // runs later in the loop. Per-pass GPU timings are present only while
    // profiling is armed (Visual Profiler / Render Graph panel Profile button).
    // 16. get_render_graph_overview


    // 17. get_render_graph_passes


    // 18. get_render_graph_resources


    // 19. get_render_graph_pass_detail


    // 20. get_render_graph_dependencies


    // Probe diagnostics are available without opening an inspector, allowing
    // capture publication, work budgets and static-edit refreshes to be tested.
    server.RegisterHandler("get_reflection_probe_state", [](const EditorDebugServer::RequestContext&) -> json {
        using Ibl = Engine::Renderer::ImageBasedLightingFeature;
        using Source = Engine::Renderer::SceneReflectionProbeEnvironmentSource;
        auto& engine = EngineCore::GetInstance();
        auto* world = engine.GetPrimaryWorld();
        auto* services = engine.GetRenderServices();
        auto* ibl = services ? services->GetFeature<Ibl>() : nullptr;
        json probes = json::array();
        if (world && ibl)
            for (uint32_t i = 0; i < Ibl::kMaxLocalProbes; ++i)
            {
                const auto* capture = ibl->GetLocalProbeCapture(world->GetWorldId(), i);
                const auto* desc = ibl->GetLocalProbeDesc(world->GetWorldId(), i);
                if (!capture || !desc) continue;
                const auto* source = static_cast<const Source*>(capture->GetEnvironmentSource());
                const auto d = source->GetDiagnostics();
                probes.push_back({
                    {"slot", i}, {"entityId", desc->SourceKey & 0xffffffffu},
                    {"sourceKey", std::to_string(desc->SourceKey)}, {"priority", desc->Priority},
                    {"ready", capture->IsInitialized() && capture->GetLastBakedDigest() != 0},
                    {"baking", source->IsBaking()}, {"needsRefresh", source->NeedsBake()},
                    {"stage", d.Stage}, {"step", d.Step}, {"completedBakes", d.CompletedBakes},
                    {"ageSeconds", d.AgeSeconds}, {"waitSeconds", d.WaitSeconds},
                    {"capturePrepareCpuMs", d.CaptureRecordMs}, {"filterRecordCpuMs", d.FilterRecordMs},
                    {"lastFaceGpuSpanMs", d.LastCaptureGpuSpanMs}, {"lastFilterGpuSpanMs", d.LastFilterGpuSpanMs},
                    {"gpuSpansShared", d.GpuSpansShared},
                    {"inputDigest", std::to_string(source->InputDigest())},
                    {"bakedDigest", std::to_string(capture->GetLastBakedDigest())},
                    {"captureResolution", capture->GetEnvCaptureSize()}, {"prefilterMaxMip", capture->GetPrefilterMaxMip()},
                    {"capturePosition", {source->GetCapturedPosition()[0], source->GetCapturedPosition()[1], source->GetCapturedPosition()[2]}},
                    {"requestedPosition", {desc->Position[0], desc->Position[1], desc->Position[2]}},
                    {"influenceShape", desc->InfluenceShape}, {"boxProjection", desc->BoxProjection}});
            }
        return json{{"worldId", world ? world->GetWorldId() : 0}, {"capacity", Ibl::kMaxLocalProbes},
            {"globalWorkUnitsPerFrame", 1}, {"workUnit", "one capture face or one filtering dispatch"}, {"probes", probes}};
    });
    server.RegisterHandler("rebake_reflection_probes", [](const EditorDebugServer::RequestContext& ctx) -> json {
        using Ibl = Engine::Renderer::ImageBasedLightingFeature;
        auto& engine = EngineCore::GetInstance();
        auto* world = engine.GetPrimaryWorld();
        auto* services = engine.GetRenderServices();
        auto* ibl = services ? services->GetFeature<Ibl>() : nullptr;
        if (!world || !ibl) return json{{"queued", 0}};
        const uint64_t entityId = ctx.params.value("entityId", uint64_t(0));
        uint32_t count = 0;
        for (uint32_t i = 0; i < Ibl::kMaxLocalProbes; ++i)
            if (const auto* desc = ibl->GetLocalProbeDesc(world->GetWorldId(), i);
                desc && (entityId == 0 || (desc->SourceKey & 0xffffffffu) == entityId))
            {
                ibl->RequestLocalProbeRebake(world->GetWorldId(), desc->SourceKey);
                ++count;
            }
        return json{{"queued", count}};
    });

    // 21. get_render_graph_validation — the immediate-mode graph has no separate
    // validation layer; surface culled passes (declared but never run) and their
    // reason as warnings, which is the actionable signal here.


    // 22. get_camera — read the selected view's camera and projection.


    // 23. set_camera — set the editor scene view camera pose.
    // Accepts any combination of: position [x,y,z], yawDeg, pitchDeg, distance.
    // Omitted fields keep their current values.




    // 24. look_at — point the camera at a world position or entity from a
    // given direction and distance. Computes yaw/pitch from the direction
    // vector and positions the camera at target - dir * distance. For
    // entities the target is the subtree bounds union and, unless the caller
    // passes an explicit distance, the distance is fitted to those bounds.


    // 25. move_camera — smoothly animate the camera to a target pose.
    // Same parameters as set_camera, plus optional durationSec (default 0.7).


    // 26. capture_resource
    // Capture a named render-graph resource — a texture as a PNG, a buffer as
    // bytes — from the live immediate-mode frame.
    //
    // The readback is declared into the frame at the same seam take_screenshot
    // uses: RunRenderCallbacks fires after every normal pass is declared and
    // before Execute, so the whole frame's resource table is visible and the
    // capture reads each resource's FINAL contents for the frame.
    //
    // Correctness rests on the declared CopySrc read, not on pass ordering: the
    // RAW edge from the last writer is what sequences the copy after the
    // producer, and PreventCulling (inside the readback helper) keeps a producer
    // alive that nothing else consumes — capturing a resource is what makes it
    // live, so an otherwise-culled target still realizes and runs.


    // trigger_capture — RenderDoc frame capture via in-app API.
    // Resolves RenderDoc lazily through app so handlers only need to be
    // registered once (RenderDoc is initialized after first few frames).


    // get_capture_status — RenderDoc availability plus the newest capture on
    // disk. Answers on the calling frame, so it recovers the .rdc path when a
    // trigger_capture call timed out before RenderDoc finished writing.


    // get_pass_descriptors — capture descriptor set bindings at draw time for a named pass.


    // 28. toggle_shadow_debug — toggle cascade color, shadow factor, or PCSS
    // branch classification debug visualization.
    // mode: 0=off, 1=cascade colors, 2=combined factor, 3=PCSS branch
    // 4=base factor, 5=contact factor, 6=contact-only lighting, 7=base-only lighting
    // (green=early-out lit, red=early-out shadowed, blue=full penumbra path).
    // Omit to cycle through modes.


    // 29. toggle_shadow_thumbnails — toggle shadow map cascade thumbnails overlay.
    // enable: true/false. Omit to toggle.
    // The overlay pass uses an activation predicate that checks this flag each frame.


    // 29a2. get_mesh_registry — dump every live MeshGPURegistry entry with its
    // source identity (asset GUID + submesh) and LOD chain. This is the mapping
    // a GPU capture cannot recover on its own: GPUScene mesh-row index -> asset,
    // per-LOD index counts, achieved simplify errors, sloppy flags, and the
    // derived screen-coverage switch points the scatter consumes. Read-only.


    // 29b. set_lod — drive the GPU LOD selection knobs for diagnosing coverage
    // thresholds and simplified-mesh seams.
    // {forceLevel?: int, bias?: float, shadowBias?: float, cullCoverage?: float,
    //  budgetPx?: float, skinnedScale?: float, crossfadeDuration?: float,
    //  mode?: string}.
    //   forceLevel:  0..3 pins every instance to that LOD; -1 (or omit) restores
    //                screen-coverage auto-selection.
    //   bias:        log2 coverage bias; positive keeps more detail, negative
    //                switches to coarser LODs earlier. Applies to the auto path.
    //   shadowBias:  extra log2 bias added on shadow buckets only (cascades,
    //                spot/point/area), so shadow maps can run coarser levels
    //                than the view that casts them. Camera slices — including
    //                the depth prepass — never take it: prepass geometry must
    //                stay identical to the forward pass it primes.
    //                TRAP: the bias multiplies COVERAGE, not the threshold
    //                (draw_command_scatter.comp ge_SelectLOD), so a negative
    //                value scales every switch point by 2^-bias — including
    //                kLodSloppyThresholdCap (MeshLODThresholds.h), the floor
    //                that holds sloppy levels back. Sloppy levels drop whole
    //                regions instead of eroding evenly, and submeshes simplify
    //                independently (ModelAsset::GenerateLODs), so admitting
    //                them earlier in shadow reads as missing caster geometry
    //                and as light leaking between the submeshes of a solid
    //                object. Judge a negative value on the shadow, not on the
    //                lit image the coverage thresholds were tuned against.
    //   cullCoverage: small-object cull threshold (prototype; 0 disables) —
    //                camera-slice instances below this projected coverage drop.
    //   budgetPx:    SSE projected-error budget in render-target pixels
    //                (generated chains); <= 0 disables SSE coarsening.
    //   skinnedScale: budget multiplier for skinned/character chains.
    //   gameViewBudget / sceneViewBudget: per-view-class override of budgetPx,
    //                {"enabled": bool, "percent": 10..400}. Omitted sub-keys keep
    //                their current value. Disabled = that class spends budgetPx
    //                unchanged. Scene View covers every scene viewport.
    //   crossfadeDuration: dithered LOD crossfade length in seconds (0
    //                disables). A camera-slice level change dissolves over
    //                this window instead of popping. Shadow buckets never take
    //                it, for the same reason they never take cullCoverage.
    //                Prepass-safe: a fading instance writes its record PAIR to
    //                the row's TAIL region and no head record, and BOTH raster
    //                passes draw that tail through the same dither, so the
    //                prepass writes depth for exactly the fragments the colour
    //                pass keeps — each level on its own half, neither writing
    //                where the other does.
    //                NOTE: a non-zero duration makes a frame's records depend on
    //                the fade CLOCK, so captures stop being reproducible from
    //                pose alone — set 0 before any A/B that assumes they are.
    //   mode:        selection mapping — "sse", "coverage" (the pre-SSE
    //                reference) or "off" (every mesh draws LOD0). Validated
    //                before any knob is applied, so a typo changes nothing.
    //   hysteresis:  LOD dwell band as a fraction of the switch coverage (0
    //                disables). Gaining detail needs coverage >= threshold*(1+band);
    //                holding or losing it needs only the threshold. Camera slices
    //                only. NOTE: a non-zero band makes selection depend on the
    //                previous frame, so captures stop being reproducible from
    //                pose alone — set 0 before any A/B that assumes they are.
    // Returns the resolved knobs so a caller can A/B a fixed pose across levels.


    // 30. set_shadow_quality — switch the shadow filter quality.
    // quality: 0 = 5x5 grid PCF, 1 = 3x3 grid PCF, 2 = Poisson PCF,
    //          3 = PCSS, 4 = MSM4 (Moment Shadow Maps).
    // PCSS / MSM4 silently fall back to PoissonPCF when their prerequisites
    // (bindless / a ready moments texture) aren't met — see
    // ShadowMapRenderFeature::BuildShadowDataGPU.


    // 30b. set_shadow_pcss — toggle PCSS (contact-hardening shadows) and tune.
    // Optional params:
    //   enabled (bool), maxPenumbra (float, world units; shader caps physical
    //   penumbra width), receiverPlaneBias (bool).
    // Penumbra WIDTH is authored per light (Components::Light::ShadowAngularDiameter,
    // read/written through get_entity_components / set_component) — deliberately not
    // exposed here, because a global setter is the defect this slice removed.


    // 30c. set_moments_resolution — set MSM4 moments resolution (snaps to {512,1024,2048}).
    // Param: resolution (uint).


    // 30d. set_msm_blur_mode — set MSM4 blur kernel mode.
    // Param: mode (int): 0 = Linear5Tap (fast), 1 = Discrete9Tap.


    // --- Asset Registry Debug Handlers ---











    // Skeleton store diagnostics: per-world runtime state for debugging multi-world skinning isolation.


    // CPU profiler: the last completed frame as a main-thread call tree, plus
    // the flat aggregate that is the only view carrying scopes which ran off
    // the main thread. Auto-enables on first call so external tooling doesn't
    // need to bounce through the F2 hotkey; enable:false disarms, which matters
    // because an armed profiler takes its mutex in every scope exit on every
    // thread, so leaving it armed taxes every measurement taken afterwards.
    //
    // A disarm holds only while the CPU Profiler panel is closed: that panel
    // re-arms every Update() it spends visible and unpaused, and nothing
    // arbitrates between the two. wasEnabledOnEntry on the next read tells the
    // caller whether the disarm survived.


    // GPU profiler: per-pass CPU/GPU timings from the main window's render graph
    // for the last resolved frame. Pass {"enabled": true|false} to arm/disarm
    // per-pass GPU timestamp profiling (results resolve ~FramesInFlight frames
    // later; the first call after arming returns empty until they land).
    // timingSemantics says what gpuSpanMs measures; under "encoder-span" (Metal)
    // the values are overlapping encoder spans and there is deliberately no
    // frame GPU total in the payload — summing them is wrong. spanCounted marks
    // the one pass per measurement that resolveStats.distinctSpanGpuMs counts
    // (RGPassTiming::SpanCounted), so a total over any set of passes adds only
    // those.


    // Named monitor history (frame-time ring buffer, memory, draw counts, ...).
    // Returns the list of registered monitors with their latest/peak/average,
    // and optionally the full history for a single monitor when 'name' is set.


    // R0.11 diagnostic: the device's HDR output state (swapchain mode, encode
    // anchors, static metadata) next to the OS per-monitor SDR white levels so
    // the paper-white divisor can be verified against what Windows reports.




    // get_msaa — report the engine-wide default MSAA sample count (the value cameras
    // with MSAASamples==0 inherit), the device cap, and the persisted project setting.
    //
    // The sample count alone does NOT say whether anything is multisampled: it
    // keeps its value while aaMode is off or taa, so a caller reading only
    // "samples" can report 8x on a run with no MSAA at all. aaMode is therefore
    // reported alongside it and is the field that decides whether samples is in
    // effect (it is honored only in msaa mode). aaModePersisted is the project
    // setting, which names a concrete mode: opening a project writes one for it
    // if it never chose. It is empty only for a project whose file has not been
    // materialized, and aaMode then reports the capability default the renderer
    // resolved in memory.
    //
    // Both fields describe the ENGINE-WIDE default, which is not necessarily what
    // a given view renders. At least two things reroute it downstream, so treat
    // this as a floor rather than an exhaustive list: any TAA-resolved mode is
    // dropped per-view where TAA does not apply (SceneViewController /
    // GameViewController drop it for orthographic, 2D, fixed-orientation,
    // letterboxed or already-multisampled views, and they do so identically
    // for every stored mode, including a materialized default), and a camera can
    // override the mode outright via RenderServices::ResolveAntiAliasing.


    // set_msaa — persist rendering.msaa (same form the Settings panel uses) and
    // re-apply live (fans out to every window). samples: 1 = off, or 2/4/8 —
    // every accepted value is a concrete count, so what lands in the file is
    // what the renderer runs. The device clamps unsupported counts down. Shadow
    // maps are unaffected (depth-only, always single-sample).


    // set_aa_mode — persist rendering.aaMode and re-apply live (fans out to
    // every window; targets re-spec next frame). mode: "off" | "msaa" | "taa" |
    // "fxaa" | "smaa" | "temporalfxaa" | "ssaa".
    //
    // "ssaa" is the one that is not an engine mode: it is editor-level and
    // applies as Off, with set_taa_render_scale carrying the supersampling.
    // Every mode leaves the sample count as rendering.msaa / set_msaa
    // configured it (honored only in msaa mode). The response reports the
    // APPLIED mode and samples, which differ from the request only under
    // GE_AA_MODE — that override persists the value but suppresses the live
    // apply, because the env var outranks project settings. get_msaa reports
    // the same applied mode without mutating anything.


    // set_taa_render_scale — persist rendering.taaRenderScale and re-apply
    // live (fans out to every window; the pipeline pre-pass re-derives the
    // internal extent next frame). scale: [0.5, 2.0]; 1.0 = native, above 1.0
    // supersamples (SSAA). Fully live, including the material texture mip bias
    // (per-view ViewParams uniform derived from the actual extent ratio).
    // Under GE_TAA_RENDER_SCALE the value persists but the live apply is
    // suppressed — the env override outranks project settings.


    // set_dynamic_resolution — mode + target for DRS, live, for same-session
    // A/B. mode: "off" | "fixed" | "dynamic". targetFps / targetMs optionally
    // retune the controller. Dynamic mode arms the render-graph GPU profiler
    // (its per-pass timestamps are the cost signal) and drives the same render
    // scale set_taa_render_scale writes, so the two are mutually exclusive by
    // construction. Returns the live controller state so a harness can watch
    // the loop converge without a screenshot.
    //
    // Fans out live rather than through the settings file like its three
    // siblings: this knob does not persist (targetMs, minScale and maxScale
    // have no project key at all), so the file cannot carry the state, and
    // under GE_TAA_RENDER_SCALE the file-backed re-apply skips the whole DRS
    // block — which would make this handler a no-op in exactly the A/B runs it
    // exists for.


    // debug_crash — deliberately kill the editor through a chosen crash path so
    // automation can gate the crash-evidence pipeline (SEH filter / terminate
    // handler / signal handler → minidump in every config + synchronous stderr
    // backtrace). This exists because a real editor death used to leave zero
    // evidence in DebugFast/Release, and got misattributed to whatever IPC call
    // happened nearby (the 2026-07-16 "set_component float crash" mirage).
    // kind: "av" (default) = access violation on the main thread,
    //       "terminate"    = std::terminate,
    //       "abort"        = std::abort (signal path).
    // The process dies inside the handler, so no response is ever sent — the
    // caller sees the connection drop, then verifies the dump + stderr output.

}

} // namespace GameEngine
