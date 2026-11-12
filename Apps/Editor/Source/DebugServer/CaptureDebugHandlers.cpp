#include "DebugHandlerHelpers.h"

namespace GameEngine
{
namespace
{
EditorApplication::EditorWindowContext* FindCaptureWindow(EditorApplication& app, uint64_t windowId)
{
    for (const auto& window : app.m_Windows)
        if (window && window->windowId == windowId)
            return window.get();
    return nullptr;
}
} // namespace
void RegisterCaptureDebugHandlers(EditorDebugServer& server, EditorApplication& app)
{
server.RegisterHandler("take_screenshot", [&app, &server](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        if (app.m_Windows.empty())
            return Editor::RefuseRequest("No editor window");

        const int windowIndex = ctx.params.value("windowIndex", 0);
        if (windowIndex < 0 || static_cast<size_t>(windowIndex) >= app.m_Windows.size())
            return Editor::RefuseRequest("windowIndex out of range");
        const uint64_t windowId = app.m_Windows[static_cast<size_t>(windowIndex)]->windowId;
        std::string requestId = ctx.id;

        // Parse parameters up-front (before scheduling render callback).
        const std::string target = ctx.params.value("target", std::string("window"));
        if (windowIndex != 0 && target != "window")
            return Editor::RefuseRequest("Secondary windows support target='window'");
        const std::string panelId = ctx.params.value("panelId", std::string());
        const std::string elementId = ctx.params.value("elementId", std::string());
        const int32_t rectX = ctx.params.value("x", 0);
        const int32_t rectY = ctx.params.value("y", 0);
        const int32_t rectW = ctx.params.value("w", 0);
        const int32_t rectH = ctx.params.value("h", 0);
        // coords: "logical" (default) treats x/y/w/h as logical UI pixels —
        // matches get_ui_tree / get_panel_tree output and panel/element bounds.
        // "physical" treats them as raw pixel coords on the saved PNG, useful
        // when post-processing a previously-saved screenshot. Only meaningful
        // for target='rect'.
        const std::string coords = ctx.params.value("coords", std::string("logical"));
        // dither: default ON — captures quantize the linear readback with the same
        // TPDF the screen's sRGB encode pass applies, so screenshots match what the
        // eye sees. Harness/golden consumers that diff or hash pixels numerically
        // should pass dither=false for deterministic staircase-quantized output.
        // GE_OUTPUT_DITHER=0 turns the screen's dither off, so it has to turn the
        // mirror's off too — otherwise that A/B silently compares two identically
        // dithered captures. The step stays the capture's own 8-bit LSB (the PNG
        // quantizes at 1/255 in every output mode); only the enable is shared.
        const bool dither =
            ctx.params.value("dither", true) && Rendering::Passes::IsOutputDitherEnabled();
        // deband: default ON — same parity rationale as dither, for the screen's
        // pre-dither gradient deband (also process-gated by GE_DEBAND). Pass
        // deband=false to profile the raw quantization structure of the source.
        const bool deband = ctx.params.value("deband", true);
        // allowWindowCapture: opt in to an OS window capture (PrintWindow) when the
        // render graph cannot hand over the frame. Off by default — a window
        // capture is a picture of what the compositor holds, not the frame the
        // render graph produced, and it is a plausible PNG either way, so serving
        // one unasked turns a rendering failure into evidence the caller trusts.
        // The response states which one it is in `method`.
        const bool allowWindowCapture = ctx.params.value("allowWindowCapture", false);

        // Validate target.
        if (target != "window" && target != "viewport" && target != "panel"
            && target != "element" && target != "rect" && target != "asset_preview")
        {
            return Editor::RefuseRequest("Unknown target '" + target + "'. Use one of: window, viewport, panel, element, rect, asset_preview.");
        }
        if (coords != "logical" && coords != "physical")
            return Editor::RefuseRequest("coords must be 'logical' or 'physical'");
        if (target == "panel" && panelId.empty())
            return Editor::RefuseRequest("target='panel' requires 'panelId'");
        if (target == "element" && elementId.empty())
            return Editor::RefuseRequest("target='element' requires 'elementId'");
        if (target == "rect" && (rectW <= 0 || rectH <= 0))
            return Editor::RefuseRequest("target='rect' requires positive 'w' and 'h'");

        auto cropHolder = std::make_shared<ScreenshotCropDesc>();
        // What the readback pixels hold, stated by whichever declaration arm below
        // schedules the copy. Disengaged means "nobody stated a space" — the
        // conversion refuses rather than guessing from the format, so a new capture
        // target cannot inherit a silent default.
        auto sourceSpaceHolder = std::make_shared<std::optional<UI::UITextureSpace>>();
        // RenderGraph arm (8c-4): ticket readback — declared against the frame driver's
        // published per-frame capture context.
        auto ticketHolder = std::make_shared<std::shared_ptr<Rendering::RGReadbackTicket>>();
        auto assetPreviewPathHolder = std::make_shared<std::string>();
        // Frame the readback was declared on — reported for staleness diagnosis.
        auto frameIndexHolder = std::make_shared<uint64_t>(0);
        // Deband gate + scope snapshotted at readback declaration (right after
        // this frame's encode declared, same render thread) so the deferred
        // conversion mirrors THE frame it read — not whichever window
        // published last while the GPU finished.
        auto encodeFilterStateHolder = std::make_shared<std::optional<CaptureEncodeFilterState>>();

        LOG_DEBUG("Screenshot: request received target={} requestId={}", target, requestId);

        // How long this request keeps asking the render graph for a frame, and
        // what the caller is told when it stops. The attempt below reports into
        // it from the render phase; the poll drives it.
        auto wait = std::make_shared<Editor::ScreenshotCaptureWait>(allowWindowCapture);

        // One render-phase attempt at declaring the readback (pre-Execute on the
        // pure RenderGraph path, where the frame driver publishes the capture
        // context). The poll re-arms it every frame until the wait ends, so a
        // pane that is one frame behind — a scene view catching up on extraction —
        // is a retry rather than a failed capture.
        //
        // At most one attempt is in flight: the poll runs once per tick and the
        // render phase drains the whole callback queue at once, so queuing an
        // attempt per tick would make a starved frame loop spend the whole
        // refusal budget inside a single rendered frame.
        auto attemptQueued = std::make_shared<bool>(false);
        auto attempt = std::make_shared<EditorDebugServer::RenderCallback>();
        *attempt = [&app, wait, attemptQueued, ticketHolder, cropHolder, sourceSpaceHolder,
                    assetPreviewPathHolder, frameIndexHolder, encodeFilterStateHolder, target,
                    panelId, elementId, rectX, rectY, rectW, rectH, coords](uint64_t declaringWindowId)
        {
            *attemptQueued = false;
            // The poll can end the wait with this attempt still queued, and the
            // request is answered by then: declaring a readback nobody consumes
            // would cost the frame another composite for nothing.
            if (!wait->GiveUpReason().empty())
                return;
            // Every attempt re-declares from scratch: a crop from an earlier
            // frame must never ride along with this frame's pixels.
            *cropHolder = ScreenshotCropDesc{};

            auto* wndCtx = FindCaptureWindow(app, declaringWindowId);
            if (!wndCtx)
            {
                wait->RecordBlocked("the editor has no window");
                return;
            }

            if (!wndCtx->renderCtx)
            {
                wait->RecordBlocked("the editor window has no render context");
                return;
            }

            auto* device = wndCtx->renderCtx->GetDevice();
            if (!device)
            {
                wait->RecordBlocked("the editor window has no render device");
                return;
            }

            // 8c-4: RenderGraph capture context, filled by the frame driver right
            // before this callback runs; ids are frame-local and valid only
            // for the published incarnation.
            auto* rg2Frame = wndCtx->rg2Capture.frame;
            const bool rg2 =
                rg2Frame && wndCtx->rg2Capture.frameIndex == rg2Frame->FrameIndex();
            if (!rg2)
            {
                wait->RecordRefusal("the frame published no render-graph capture context");
                return;
            }
            *frameIndexHolder = wndCtx->rg2Capture.frameIndex;

            // ---- ASSET PREVIEW path: readback live video frame texture ---------
            if (target == "asset_preview")
            {
                auto* assetViewPanel = FindPanel<AssetViewPanel>(app.m_PanelStorage);
                if (!assetViewPanel)
                {
                    wait->RecordBlocked("the Asset View panel is not open");
                    return;
                }

                Rendering::TextureHandle previewTexture{};
                Rendering::TextureFormat previewFormat = Rendering::TextureFormat::RGBA8_UNORM;
                if (!assetViewPanel->TryGetVideoPreviewReadbackTexture(previewTexture, previewFormat))
                {
                    wait->RecordBlocked(
                        "the Asset View has no uploaded video frame — select an .mp4 first");
                    return;
                }

                *assetPreviewPathHolder = assetViewPanel->GetPreviewDebugState().previewPath;
                *ticketHolder = Rendering::RequestDeviceTextureReadbackRG(
                    device, *rg2Frame, previewTexture,
                    Rendering::ResourceState::ShaderResource, "AssetViewVideoPreview");
                // Decoded video frames are authored sRGB — the same stamp the Asset
                // View panel registers this texture to the UI with.
                *sourceSpaceHolder = UI::UITextureSpace::SrgbAuthored();
                return;
            }

            // ---- VIEWPORT path: scene-view pipeline-output readback ------------
            if (target == "viewport")
            {
                if (!wndCtx->scene)
                {
                    wait->RecordBlocked("the editor window has no scene view controller");
                    return;
                }
                auto* renderServices = app.m_EditorContext ? app.m_EditorContext->RenderServices : nullptr;
                if (!renderServices)
                {
                    wait->RecordBlocked("the editor has no render services");
                    return;
                }
                // The active scene view may be a maximized non-Perspective quad pane
                // (a secondary controller), not ctx->scene. Read back the controller
                // that's actually rendering the visible viewport, else the readback
                // targets a suspended view and never schedules.
                SceneViewController* activeSceneCtrl = wndCtx->scene.get();
                if (wndCtx->ui)
                    if (auto* root = wndCtx->ui->GetRootElement())
                        if (auto* mount = dynamic_cast<Mount*>(root->FindById(EditorPanelIds::MountSceneView)))
                            if (auto* svPanel = dynamic_cast<SceneViewPanel*>(mount->GetTarget()))
                                if (auto* ac = svPanel->GetSceneControllerForSlot(svPanel->GetActiveViewportSlot()))
                                    activeSceneCtrl = ac;
                Rendering::ViewId viewId = activeSceneCtrl ? activeSceneCtrl->GetViewId() : 0u;
                if (viewId == 0)
                {
                    wait->RecordRefusal("the scene view has no registered view yet");
                    return;
                }
                // The scene view's pipeline output — the texture the UI
                // samples (post-FX FinalColor).
                const auto out = renderServices->GetPipelineOutputRG(*rg2Frame, viewId);
                if (!out.IsValid())
                {
                    // The pane declared no pipeline output this frame: its tab is
                    // behind another, its rect collapsed, or its view is a frame
                    // behind on extraction. The first two do not clear on their
                    // own; the wait tells them apart by how long the refusal lasts.
                    wait->RecordRefusal(
                        "the scene view produced no render-graph output — its tab may be behind "
                        "another, its pane collapsed, or the view is still catching up");
                    return;
                }
                *ticketHolder = Rendering::RequestTextureReadbackRG(
                    device, *rg2Frame, out.Out, "ViewportScreenshot");
                // The pipeline's own stamp for what it wrote into FinalColor —
                // LINEAR, overlays and gizmos included. The view's own finalize
                // is downstream of this point, so a viewport capture does its own
                // encode and its own 8-bit filters rather than inheriting the
                // screen's, which are sized to the display's step. Same content,
                // filtered for the file's depth.
                *sourceSpaceHolder = renderServices->GetPipelineOutputSpaceRG();
                return;
            }

            // ---- WINDOW / PANEL / ELEMENT / RECT: presentation-target readback --
            // Every pure editor frame routes through Editor.FinalLinear, so
            // the full composite is always readable there.
            if (wndCtx->rg2Capture.finalLinearId != 0xFFFFFFFFu)
            {
                Rendering::RenderGraph::RGTexture finalLinear{};
                finalLinear.Id = wndCtx->rg2Capture.finalLinearId;
                *ticketHolder = Rendering::RequestTextureReadbackRG(
                    device, *rg2Frame, finalLinear, "WindowScreenshot");
                // The frame driver publishes the space the composite's bytes
                // hold under the declared UI target (#767/#784): SrgbAuthored
                // on SDR frames (encoded blend — no second OETF at readback),
                // DisplayLinearSdr on HDR-display capture frames.
                *sourceSpaceHolder = wndCtx->rg2Capture.compositeSpace;
                // This window's encode declared just before this callback ran —
                // the publish is exactly the state this readback's pixels see.
                *encodeFilterStateHolder = CaptureEncodeFilterState{
                    Rendering::Passes::GetBackbufferOutputDebandThresholdLsb()};
            }
            else
            {
                wait->RecordRefusal("the frame declared no composite to read back");
                return;
            }

            // Compute crop rect for panel/element/rect targets (image-space crop).
            // GetLayoutX/Y/Width/Height returns logical pixels; the readback is in
            // physical pixels — scale by content scale.
            if (target == "window")
                return;

            float layoutX = 0.0f;
            float layoutY = 0.0f;
            float layoutW = 0.0f;
            float layoutH = 0.0f;
            std::string label;
            bool found = false;

            if (target == "panel")
            {
                if (app.m_Docking)
                {
                    if (UIElement* panel = app.m_Docking->GetPanel(panelId))
                    {
                        layoutX = panel->GetLayoutX();
                        layoutY = panel->GetLayoutY();
                        layoutW = panel->GetLayoutWidth();
                        layoutH = panel->GetLayoutHeight();
                        label = "panel:" + panelId;
                        found = true;
                    }
                }
            }
            else if (target == "element")
            {
                if (wndCtx->ui)
                {
                    if (UIElement* root = wndCtx->ui->GetRootElement())
                    {
                        if (UIElement* el = root->FindById(elementId))
                        {
                            layoutX = el->GetLayoutX();
                            layoutY = el->GetLayoutY();
                            layoutW = el->GetLayoutWidth();
                            layoutH = el->GetLayoutHeight();
                            label = "element:" + elementId;
                            found = true;
                        }
                    }
                }
            }
            else if (target == "rect")
            {
                layoutX = static_cast<float>(rectX);
                layoutY = static_cast<float>(rectY);
                layoutW = static_cast<float>(rectW);
                layoutH = static_cast<float>(rectH);
                label = "rect";
                found = true;
            }

            if (!found || layoutW <= 0.0f || layoutH <= 0.0f)
            {
                LOG_WARNING("Screenshot: crop target not found or empty — returning full window");
                return;
            }

            // panel/element values come from layout (always logical). For
            // rect, honor the caller's coords choice.
            const bool applyScale = (target != "rect") || (coords == "logical");
            const float scale = applyScale && wndCtx->ui
                                    ? std::max(0.01f, wndCtx->ui->GetContentScale())
                                    : 1.0f;
            cropHolder->valid = true;
            cropHolder->x = static_cast<int32_t>(std::floor(layoutX * scale));
            cropHolder->y = static_cast<int32_t>(std::floor(layoutY * scale));
            cropHolder->w = static_cast<int32_t>(std::ceil(layoutW * scale));
            cropHolder->h = static_cast<int32_t>(std::ceil(layoutH * scale));
            cropHolder->label = std::move(label);
            LOG_DEBUG("Screenshot: crop {} -> ({},{},{},{})",
                      cropHolder->label, cropHolder->x, cropHolder->y, cropHolder->w, cropHolder->h);
        };
        *attemptQueued = true;
        server.EnqueueRenderCallback(windowId, attempt);

        // Deferred poll: each frame, re-arm the attempt until the readback is
        // declared, then wait for the GPU copy. The wait decides when to stop and
        // says why; a window capture is served only to a caller that asked for
        // one, because it is a picture of the window rather than the frame the
        // render graph produced.
        const auto requestStart = std::chrono::steady_clock::now();
        // Naming the opt-in flag helps only a caller that has not already passed
        // it, and only where an OS window capture can stand in: asset_preview
        // reads a video texture no picture of the window substitutes for.
        const bool offerWindowCaptureHint = !allowWindowCapture && target != "asset_preview";
        // The job that converts, encodes and serializes the finished readback.
        auto encodeHolder = std::make_shared<JobSystem::TaskHandle>();
        server.EnqueueDeferredResponse(requestId,
            [&app, &server, attempt, attemptQueued, wait, ticketHolder, requestStart, cropHolder,
             sourceSpaceHolder,
             assetPreviewPathHolder, frameIndexHolder, encodeFilterStateHolder, encodeHolder, target,
             panelId, elementId, rectX, rectY, rectW, rectH, coords, dither, deband,
             offerWindowCaptureHint, requestId, windowId](json& outResult) -> bool
            {
                auto reportFailure = [&](const std::string& reason,
                                         bool offerWindowCapture = false) -> bool
                {
                    LOG_WARNING("Screenshot ({}): {}", target, reason);
                    outResult =
                        Editor::RefuseRequest(Editor::ScreenshotFailureMessage(reason, offerWindowCapture),
                                              json{{"fallbackReason", reason}});
                    return true;
                };

                auto* liveWindow = FindCaptureWindow(app, windowId);
                if (!liveWindow || !liveWindow->window)
                    return reportFailure("the requested editor window was closed");

                // The readback has been handed to the encode job: wait for its line.
                if (encodeHolder->IsValid())
                {
                    std::string jobError;
                    if (!Editor::PollHandlerReplyJob(*encodeHolder, outResult, jobError))
                        return false;
                    if (!jobError.empty())
                        return reportFailure("the screenshot encode job did not complete: " + jobError);
                    return true;
                }

                auto windowCapture = [&](const std::string& reason) -> bool
                {
                    // asset_preview reads a video texture no window capture can
                    // substitute for — it fails whatever the caller allowed.
                    if (target == "asset_preview")
                        return reportFailure(reason + "; asset_preview has no window-capture substitute");
                    if (!liveWindow->window)
                        return reportFailure(reason + "; no window to capture either");
                    auto* wndCtx = liveWindow;
                    std::vector<uint8_t> rgba8;
                    uint32_t srcW = 0;
                    uint32_t srcH = 0;
                    if (!CaptureWindowClientAreaRGBA8(wndCtx->window.get(), rgba8, srcW, srcH))
                        return reportFailure(reason +
                                             "; the window capture also failed (non-Windows "
                                             "platform, hidden window, or capture error)");
                    LOG_WARNING("Screenshot ({}): {} — served the window capture the request allowed",
                                target, reason);

                    // The render callback that normally fills cropHolder may
                    // never have run on this path — compute the crop from the
                    // live layout instead. viewport degrades to the scene-view
                    // mount's on-screen region of the composite.
                    ScreenshotCropDesc crop;
                    float layoutX = 0.0f;
                    float layoutY = 0.0f;
                    float layoutW = 0.0f;
                    float layoutH = 0.0f;
                    bool found = false;
                    if (target == "panel" && app.m_Docking)
                    {
                        if (UIElement* panel = app.m_Docking->GetPanel(panelId))
                        {
                            layoutX = panel->GetLayoutX();
                            layoutY = panel->GetLayoutY();
                            layoutW = panel->GetLayoutWidth();
                            layoutH = panel->GetLayoutHeight();
                            crop.label = "panel:" + panelId;
                            found = true;
                        }
                    }
                    else if ((target == "element" || target == "viewport") && wndCtx->ui)
                    {
                        if (UIElement* root = wndCtx->ui->GetRootElement())
                        {
                            const std::string lookupId =
                                target == "viewport" ? std::string(EditorPanelIds::MountSceneView)
                                                     : elementId;
                            if (UIElement* el = root->FindById(lookupId))
                            {
                                layoutX = el->GetLayoutX();
                                layoutY = el->GetLayoutY();
                                layoutW = el->GetLayoutWidth();
                                layoutH = el->GetLayoutHeight();
                                crop.label = "element:" + lookupId;
                                found = true;
                            }
                        }
                    }
                    else if (target == "rect")
                    {
                        layoutX = static_cast<float>(rectX);
                        layoutY = static_cast<float>(rectY);
                        layoutW = static_cast<float>(rectW);
                        layoutH = static_cast<float>(rectH);
                        crop.label = "rect";
                        found = true;
                    }
                    if (found && layoutW > 0.0f && layoutH > 0.0f)
                    {
                        const bool applyScale = (target != "rect") || (coords == "logical");
                        const float scale = applyScale && wndCtx->ui
                                                ? std::max(0.01f, wndCtx->ui->GetContentScale())
                                                : 1.0f;
                        crop.valid = true;
                        crop.x = static_cast<int32_t>(std::floor(layoutX * scale));
                        crop.y = static_cast<int32_t>(std::floor(layoutY * scale));
                        crop.w = static_cast<int32_t>(std::ceil(layoutW * scale));
                        crop.h = static_cast<int32_t>(std::ceil(layoutH * scale));
                    }

                    FinishScreenshotResult(outResult, std::move(rgba8), srcW, srcH, crop, target,
                                           "printwindow", PendingMaterialTextureBinds());
                    outResult["fallbackReason"] = reason;
                    outResult["windowId"] = windowId;
                    return true;
                };

                Rendering::ViewReadbackResult result;
                bool readbackReady = false;
                // RenderGraph arm: deterministic ticket completion (8c-1). The
                // attempt creates the ticket in the render phase.
                if (*ticketHolder)
                {
                    if ((*ticketHolder)->IsConsumed())
                    {
                        // A cancelled ticket (declaring frame abandoned) can never
                        // resolve — drop it and let a later frame declare again.
                        ticketHolder->reset();
                        wait->RecordRefusal("the frame that declared the readback was abandoned "
                                            "before submit");
                    }
                    else
                    {
                        wait->RecordReadbackDeclared();
                        readbackReady = (*ticketHolder)->TryGet(result);
                    }
                }

                if (!readbackReady)
                {
                    // The same condition the render loop skips a window on: with no
                    // framebuffer there is no frame to declare a readback into, and
                    // no later frame changes that on its own.
                    if (!*ticketHolder && liveWindow->window)
                    {
                        int fbW = 0;
                        int fbH = 0;
                        liveWindow->window->GetFramebufferSize(fbW, fbH);
                        if (fbW <= 0 || fbH <= 0)
                            wait->RecordBlocked("the editor window has no framebuffer (minimized "
                                                "or hidden), so it renders no frame to capture");
                    }
                    const double elapsed =
                        std::chrono::duration<double>(std::chrono::steady_clock::now() - requestStart)
                            .count();
                    const auto outcome = wait->Step(elapsed);
                    if (outcome == Editor::ScreenshotCaptureWait::Outcome::ServeWindowCapture)
                        return windowCapture(wait->GiveUpReason());
                    if (outcome == Editor::ScreenshotCaptureWait::Outcome::Fail)
                        return reportFailure(wait->GiveUpReason(), offerWindowCaptureHint);
                    // Keep waiting: the next rendered frame gets its own attempt.
                    if (!*ticketHolder && !*attemptQueued)
                    {
                        *attemptQueued = true;
                        server.EnqueueRenderCallback(windowId, attempt);
                    }
                    return false;
                }

                LOG_DEBUG("Screenshot: readback complete {}x{} fmt={}", result.width, result.height,
                          static_cast<uint32_t>(result.format));

                // The declaring arm states the space; a scheduled ticket without one is
                // a capture target that forgot to, not a licence to infer.
                if (!*sourceSpaceHolder)
                    return reportFailure("the readback declared no source colour space");

                // The frame's share ends with the readback: conversion, PNG encode,
                // file write, base64 and serialization run on a job, and a later poll
                // sends the line it produced.
                *encodeHolder = Editor::SubmitHandlerReplyJob(
                    EngineCore::GetInstance().GetJobSystem(), requestId,
                    [readback = std::move(result), srgbEncode = !UI::IsEncodedAtRest(**sourceSpaceHolder),
                     dither, deband,
                     filterState = encodeFilterStateHolder->value_or(CaptureEncodeFilterState{
                         Rendering::Passes::GetBackbufferOutputDebandThresholdLsb()}),
                     crop = *cropHolder, target, windowId,
                     frameIndex = *frameIndexHolder, previewPath = *assetPreviewPathHolder,
                     pendingBinds = PendingMaterialTextureBinds()]() -> json
                    {
                        auto rgba8 = ConvertToRGBA8(readback.pixels.data(), readback.width, readback.height,
                                                    readback.format, 0.0f, 1.0f, srgbEncode, dither, deband,
                                                    &filterState);
                        json reply;
                        FinishScreenshotResult(reply, std::move(rgba8), readback.width, readback.height, crop,
                                               target, "rendergraph", pendingBinds);
                        if (!Editor::IsRefusal(reply))
                        {
                            reply["frameIndex"] = frameIndex;
                            reply["windowId"] = windowId;
                            if (target == "asset_preview" && !previewPath.empty())
                                reply["previewPath"] = previewPath;
                        }
                        return reply;
                    });
                if (!encodeHolder->IsValid())
                    return reportFailure("the job system is shutting down; the capture was not encoded");
                return false;
            },
            Editor::kScreenshotPollBackstop);

        return EditorDebugServer::DeferredMarker(); });

server.RegisterHandler("capture_shadow_diagnostics", [&app, &server](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        if (app.m_Windows.empty())
            return Editor::RefuseRequest("No editor window");

        const std::string requestId = ctx.id;
        const int requestedViewId = ctx.params.value("viewId", 0);
        const int requestedCascade = ctx.params.value("cascade", -1);
        const uint32_t maxStreams = std::max(1u, ctx.params.value("maxStreams", 8u));
        const uint32_t maxRecords = std::max(1u, ctx.params.value("maxRecords", 64u));

        struct ShadowTicket
        {
            uint32_t cascade = 0;
            std::shared_ptr<Rendering::RGReadbackTicket> ticket;
        };
        struct BufferTicket
        {
            std::string label;
            std::shared_ptr<Rendering::RGBufferReadbackTicket> ticket;
        };
        struct StreamTickets
        {
            uint64_t streamKey = 0;
            uint32_t materialIndex = 0;
            uint32_t meshIndex = 0;
            // The resolved axes the range was published under (depth-class
            // sentinel or color class, and pool group under consolidation).
            uint32_t classKey = 0;
            uint32_t meshKey = 0;
            uint32_t maxDrawCount = 0;
            uint32_t recordsRead = 0;
            BufferTicket count;
            BufferTicket commands;
            BufferTicket indirection;
        };
        struct CaptureState
        {
            Rendering::ViewId viewId = 0;
            uint32_t shadowResolution = 0;
            std::vector<ShadowTicket> shadows;
            std::vector<StreamTickets> streams;
            std::string setupError;
        };
        auto state = std::make_shared<CaptureState>();

        if (app.m_Windows.empty())
            return Editor::RefuseRequest("No editor window");
        const uint64_t windowId = app.m_Windows.front()->windowId;
        auto declaration = std::make_shared<EditorDebugServer::RenderCallback>(
            [&app, state, requestedViewId, requestedCascade, maxStreams, maxRecords](uint64_t declaringWindowId)
        {
            if (app.m_Windows.empty())
            {
                state->setupError = "No editor window";
                return;
            }
            auto* wndCtx = FindCaptureWindow(app, declaringWindowId);
            if (!wndCtx)
            {
                state->setupError = "The requested editor window was closed";
                return;
            }
            auto* renderServices = app.m_EditorContext ? app.m_EditorContext->RenderServices : nullptr;
            if (!renderServices)
            {
                state->setupError = "No RenderServices";
                return;
            }
            auto* device = renderServices->GetDevice();
            if (!device)
            {
                state->setupError = "No render device";
                return;
            }
            auto* rg2Frame = wndCtx->rg2Capture.frame;
            const bool rg2 = rg2Frame && wndCtx->rg2Capture.frameIndex == rg2Frame->FrameIndex();
            if (!rg2)
            {
                state->setupError = "No RenderGraph capture context this frame";
                return;
            }

            Rendering::ViewId viewId = static_cast<Rendering::ViewId>(std::max(0, requestedViewId));
            if (viewId == 0)
            {
                SceneViewController* activeSceneCtrl = wndCtx->scene.get();
                if (wndCtx->ui)
                    if (auto* root = wndCtx->ui->GetRootElement())
                        if (auto* mount = dynamic_cast<Mount*>(root->FindById(EditorPanelIds::MountSceneView)))
                            if (auto* svPanel = dynamic_cast<SceneViewPanel*>(mount->GetTarget()))
                                if (auto* ac = svPanel->GetSceneControllerForSlot(svPanel->GetActiveViewportSlot()))
                                    activeSceneCtrl = ac;
                viewId = activeSceneCtrl ? activeSceneCtrl->GetViewId() : 0u;
            }
            if (viewId == 0)
            {
                state->setupError = "Invalid viewId";
                return;
            }
            state->viewId = viewId;

            const auto shadowArr = renderServices->GetShadowMapArrayRG(*rg2Frame, viewId);
            if (shadowArr.IsValid())
            {
                const auto& desc = rg2Frame->Graph().ResourceDesc(shadowArr.Id);
                state->shadowResolution = desc.Width;
                const uint32_t layers = std::max(1u, desc.ArrayLayers);
                const uint32_t firstCascade =
                    requestedCascade >= 0 ? static_cast<uint32_t>(requestedCascade) : 0u;
                const uint32_t endCascade =
                    requestedCascade >= 0 ? std::min(layers, firstCascade + 1u) : layers;
                for (uint32_t c = firstCascade; c < endCascade; ++c)
                {
                    ShadowTicket st{};
                    st.cascade = c;
                    st.ticket = Rendering::RequestTextureSubresourceReadbackRG(
                        device, *rg2Frame, shadowArr, 0, c, 0, 0, 0, 0, "ShadowCascadeDiag");
                    if (st.ticket)
                        state->shadows.push_back(std::move(st));
                }
            }

            auto* drawStream = renderServices->GetDrawStreamBuilder();
            const auto& frameRG = renderServices->FrameRG();
            const bool orderingValid =
                frameRG.For.IsFor(*rg2Frame) &&
                frameRG.DrawStreamOrdering.IsValid();
            if (drawStream && orderingValid)
            {
                const auto keys = renderServices->GetEntityBatchKeys(viewId);
                const uint8_t cascadeIdx =
                    requestedCascade >= 0
                        ? static_cast<uint8_t>(requestedCascade)
                        : 0u;
                // Same resolved axes the shadow recorder looks up on: the
                // shared-depth class sentinel for eligible casters, and the
                // pool group under draw consolidation. Both merges are
                // many-keys-to-one-range, so keys that land on a range already
                // sampled are skipped rather than spending a readback slot.
                const std::span<const uint32_t> meshPoolGroups =
                    renderServices->MeshPoolGroupSpanForDraws();
                std::unordered_set<uint64_t> sampledStreams;
                uint32_t emitted = 0;
                for (const auto& key : keys)
                {
                    if (emitted >= maxStreams)
                        break;
                    const Engine::Renderer::DrawStreamLookupKey lookup =
                        Engine::Renderer::ResolveDrawStreamLookupKey(
                            key, cascadeIdx,
                            renderServices->Materials().GetMaterialDepthClass(key.materialIndex),
                            meshPoolGroups);
                    const uint64_t streamKey = Rendering::GPUDrawStreamBuilder::MakeStreamKey(
                        static_cast<uint32_t>(viewId), cascadeIdx, lookup.classKey, lookup.meshKey,
                        Rendering::GPUDrawStreamBuilder::SlicePhase::A);
                    if (!sampledStreams.insert(streamKey).second)
                        continue;
                    const auto range = drawStream->FindBatchDrawRange(
                        static_cast<uint32_t>(viewId), cascadeIdx, lookup.classKey, lookup.meshKey,
                        Rendering::GPUDrawStreamBuilder::SlicePhase::A);
                    if (!range.IsValid())
                        continue;

                    const uint32_t records =
                        std::min(maxRecords, std::max(1u, range.even.maxDrawCount));
                    StreamTickets stream{};
                    stream.streamKey = streamKey;
                    stream.materialIndex = key.materialIndex;
                    stream.meshIndex = key.meshIndex;
                    stream.classKey = lookup.classKey;
                    stream.meshKey = lookup.meshKey;
                    stream.maxDrawCount = range.even.maxDrawCount;
                    stream.recordsRead = records;

                    const std::string prefix =
                        "ShadowStreamDiag." + std::to_string(static_cast<uint32_t>(viewId)) +
                        ".c" + std::to_string(static_cast<uint32_t>(cascadeIdx)) +
                        ".m" + std::to_string(lookup.meshKey) +
                        ".mat" + std::to_string(lookup.classKey);
                    // Shared arena buffers: read this batch's region at the
                    // range's byte offsets. Indirection region starts at the
                    // same record index (one uint per record).
                    const size_t indirByteOffset =
                        range.even.cmdByteOffset / (5u * sizeof(uint32_t)) * sizeof(uint32_t);
                    const auto cmdRG = rg2Frame->ImportExternalBuffer("ShadowStreamDiag.Cmd", range.recordBuffer);
                    const auto countRG = rg2Frame->ImportExternalBuffer("ShadowStreamDiag.Count", range.countBuffer);
                    const auto indirRG = rg2Frame->ImportExternalBuffer(
                        "ShadowStreamDiag.Indirection", drawStream->GetSharedIndirectionBuffer());

                    stream.commands.label = "commands";
                    stream.commands.ticket = Rendering::RequestBufferReadbackRG(
                        device, *rg2Frame, cmdRG, range.even.cmdByteOffset,
                        static_cast<size_t>(records) * 5u * sizeof(uint32_t),
                        (prefix + ".cmd").c_str());
                    stream.count.label = "count";
                    stream.count.ticket = Rendering::RequestBufferReadbackRG(
                        device, *rg2Frame, countRG, range.even.countByteOffset, sizeof(uint32_t),
                        (prefix + ".count").c_str());
                    stream.indirection.label = "indirection";
                    stream.indirection.ticket = Rendering::RequestBufferReadbackRG(
                        device, *rg2Frame, indirRG, indirByteOffset,
                        static_cast<size_t>(records) * sizeof(uint32_t),
                        (prefix + ".indir").c_str());
                    state->streams.push_back(std::move(stream));
                    ++emitted;
                }
            }
        });

        server.EnqueueRenderCallback(windowId, declaration);
        auto pollCount = std::make_shared<int>(0);
        server.EnqueueDeferredResponse(requestId,
            [state, pollCount, declaration](json& outResult) -> bool
            {
                (void)declaration; // Keep the queued declaration alive until this response ends.
                ++(*pollCount);
                if (!state->setupError.empty())
                {
                    outResult = Editor::RefuseRequest(state->setupError);
                    return true;
                }
                if (state->shadows.empty() && state->streams.empty())
                {
                    if (*pollCount > 5)
                    {
                        outResult = Editor::RefuseRequest("No shadow or stream readbacks were scheduled");
                        return true;
                    }
                    return false;
                }

                std::vector<Rendering::ViewReadbackResult> shadowResults;
                shadowResults.resize(state->shadows.size());
                for (size_t i = 0; i < state->shadows.size(); ++i)
                {
                    if (!state->shadows[i].ticket)
                        continue;
                    if (state->shadows[i].ticket->IsConsumed())
                    {
                        outResult = Editor::RefuseRequest("A shadow readback ticket was cancelled");
                        return true;
                    }
                    if (!state->shadows[i].ticket->TryGet(shadowResults[i]))
                        return false;
                }

                struct StreamReadback
                {
                    Rendering::BufferReadbackResult count;
                    Rendering::BufferReadbackResult commands;
                    Rendering::BufferReadbackResult indirection;
                };
                std::vector<StreamReadback> streamResults;
                streamResults.resize(state->streams.size());
                auto tryBuffer = [&](const BufferTicket& ticket, Rendering::BufferReadbackResult& out) -> bool
                {
                    if (!ticket.ticket)
                        return true;
                    if (ticket.ticket->IsConsumed())
                    {
                        outResult = Editor::RefuseRequest("A buffer readback ticket was cancelled");
                        return false;
                    }
                    return ticket.ticket->TryGet(out);
                };
                for (size_t i = 0; i < state->streams.size(); ++i)
                {
                    if (!tryBuffer(state->streams[i].count, streamResults[i].count))
                        return false;
                    if (!tryBuffer(state->streams[i].commands, streamResults[i].commands))
                        return false;
                    if (!tryBuffer(state->streams[i].indirection, streamResults[i].indirection))
                        return false;
                }

                json shadows = json::array();
                for (size_t i = 0; i < state->shadows.size(); ++i)
                {
                    const auto& rb = shadowResults[i];
                    float minDepth = std::numeric_limits<float>::infinity();
                    float maxDepth = -std::numeric_limits<float>::infinity();
                    uint64_t nonZero = 0;
                    if (rb.format == Rendering::TextureFormat::D32_FLOAT && !rb.pixels.empty())
                    {
                        const size_t count = rb.pixels.size() / sizeof(float);
                        const float* values = reinterpret_cast<const float*>(rb.pixels.data());
                        for (size_t n = 0; n < count; ++n)
                        {
                            minDepth = std::min(minDepth, values[n]);
                            maxDepth = std::max(maxDepth, values[n]);
                            if (values[n] != 0.0f)
                                ++nonZero;
                        }
                    }
                    shadows.push_back({
                        {"cascade", state->shadows[i].cascade},
                        {"width", rb.width},
                        {"height", rb.height},
                        {"format", static_cast<uint32_t>(rb.format)},
                        {"byteCount", static_cast<uint64_t>(rb.pixels.size())},
                        {"hash", Hashing::Fnv1a64(rb.pixels.data(), rb.pixels.size())},
                        {"minDepth", std::isfinite(minDepth) ? minDepth : 0.0f},
                        {"maxDepth", std::isfinite(maxDepth) ? maxDepth : 0.0f},
                        {"nonZeroDepthSamples", nonZero}
                    });
                }

                json streams = json::array();
                for (size_t i = 0; i < state->streams.size(); ++i)
                {
                    const auto& src = state->streams[i];
                    const auto& rb = streamResults[i];
                    uint32_t drawCount = 0;
                    if (rb.count.bytes.size() >= sizeof(uint32_t))
                        std::memcpy(&drawCount, rb.count.bytes.data(), sizeof(uint32_t));

                    json commands = json::array();
                    const size_t commandCount = rb.commands.bytes.size() / (5u * sizeof(uint32_t));
                    const size_t previewCount = std::min<size_t>(commandCount, 8u);
                    const uint32_t* words = reinterpret_cast<const uint32_t*>(rb.commands.bytes.data());
                    for (size_t c = 0; c < previewCount; ++c)
                    {
                        const uint32_t* cmd = words + c * 5u;
                        int32_t vertexOffset = 0;
                        std::memcpy(&vertexOffset, &cmd[3], sizeof(int32_t));
                        commands.push_back({
                            {"indexCount", cmd[0]},
                            {"instanceCount", cmd[1]},
                            {"firstIndex", cmd[2]},
                            {"vertexOffset", vertexOffset},
                            {"firstInstance", cmd[4]}
                        });
                    }

                    json indirection = json::array();
                    const size_t indirCount = rb.indirection.bytes.size() / sizeof(uint32_t);
                    const size_t indirPreview = std::min<size_t>(indirCount, 16u);
                    const uint32_t* indir = reinterpret_cast<const uint32_t*>(rb.indirection.bytes.data());
                    for (size_t n = 0; n < indirPreview; ++n)
                        indirection.push_back(indir[n]);

                    streams.push_back({
                        {"streamKey", src.streamKey},
                        {"materialIndex", src.materialIndex},
                        {"meshIndex", src.meshIndex},
                        {"drawStreamClassKey", src.classKey},
                        {"drawStreamMeshKey", src.meshKey},
                        {"drawCount", drawCount},
                        {"maxDrawCount", src.maxDrawCount},
                        {"recordsRead", src.recordsRead},
                        {"countHash", Hashing::Fnv1a64(rb.count.bytes.data(), rb.count.bytes.size())},
                        {"commandHash", Hashing::Fnv1a64(rb.commands.bytes.data(), rb.commands.bytes.size())},
                        {"indirectionHash",
                         Hashing::Fnv1a64(rb.indirection.bytes.data(), rb.indirection.bytes.size())},
                        {"commands", std::move(commands)},
                        {"indirection", std::move(indirection)}
                    });
                }

                outResult = {
                    {"viewId", static_cast<uint32_t>(state->viewId)},
                    {"shadowResolution", state->shadowResolution},
                    {"shadows", std::move(shadows)},
                    {"streams", std::move(streams)}
                };
                return true;
            });

        return EditorDebugServer::DeferredMarker(); });

server.RegisterHandler("capture_resource", [&app, &server](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        const std::string resourceName = ctx.params.value("name", "");
        if (resourceName.empty())
            return Editor::RefuseRequest("Provide a 'name' parameter (use get_render_graph_resources to list names)");

        if (app.m_Windows.empty())
            return Editor::RefuseRequest("No editor window");

        struct CaptureState
        {
            std::string requestedName;
            std::string resolvedName;
            float rangeMin = 0.0f;
            float rangeMax = 1.0f;
            uint32_t mip = 0;
            uint32_t layer = 0;
            uint64_t bufferOffset = 0;
            uint64_t bufferBytes = 0;
            std::string decode;
            uint32_t srcX = 0, srcY = 0, regionW = 0, regionH = 0;
            // Return the texels as they sit in memory instead of an 8-bit PNG.
            // An HDR intermediate does not survive the PNG path: values outside
            // [rangeMin,rangeMax] CLAMP, so a bright region reads as "identical"
            // in two captures that actually differ. Attribution needs the numbers.
            bool raw = false;

            bool isTexture = true;
            Rendering::TextureFormat format = Rendering::TextureFormat::RGBA8_UNORM;
            uint64_t frameIndex = 0;
            bool writtenThisFrame = false;
            std::shared_ptr<Rendering::RGReadbackTicket> textureTicket;
            std::shared_ptr<Rendering::RGBufferReadbackTicket> bufferTicket;
            json error;       // set by the render callback; ends the poll
            bool resolved = false;
            uint32_t polls = 0;
            JobSystem::TaskHandle encodeJob;
        };
        auto state = std::make_shared<CaptureState>();
        state->requestedName = resourceName;
        state->rangeMin = ctx.params.value("rangeMin", 0.0f);
        state->rangeMax = ctx.params.value("rangeMax", 1.0f);
        state->mip = ctx.params.value("mip", 0u);
        state->layer = ctx.params.value("layer", 0u);
        state->bufferOffset = ctx.params.value("offset", 0ull);
        state->bufferBytes = ctx.params.value("byteCount", 0ull);
        state->decode = ctx.params.value("decode", std::string("hex"));
        state->srcX = ctx.params.value("x", 0u);
        state->srcY = ctx.params.value("y", 0u);
        state->regionW = ctx.params.value("w", 0u);
        state->regionH = ctx.params.value("h", 0u);
        state->raw = ctx.params.value("raw", false);

        // Declaration phase: runs on the render thread at the RenderGraph capture seam.
        const uint64_t windowId = app.m_Windows.front()->windowId;
        auto declaration = std::make_shared<EditorDebugServer::RenderCallback>(
            [&app, state](uint64_t declaringWindowId)
        {
            if (state->resolved || app.m_Windows.empty())
                return;

            auto* wndCtx = FindCaptureWindow(app, declaringWindowId);
            auto* device = (wndCtx && wndCtx->renderCtx) ? wndCtx->renderCtx->GetDevice() : nullptr;
            auto* frame = wndCtx ? wndCtx->rg2Capture.frame : nullptr;
            if (!device || !frame || wndCtx->rg2Capture.frameIndex != frame->FrameIndex())
                return; // no live frame this tick — the poll retries on the next one

            state->resolved = true;
            state->frameIndex = frame->FrameIndex();

            CaptureResourceMatch match;
            std::vector<std::string> candidates;
            if (!ResolveCaptureResource(*frame, state->requestedName, match, candidates))
            {
                if (candidates.size() > kMaxReportedCandidates)
                    candidates.resize(kMaxReportedCandidates);
                state->error = Editor::RefuseRequest("No unique render-graph resource matches '" + state->requestedName +
                                                         "' this frame (exact name, else a unique case-insensitive substring)",
                                                     json{{"candidates", candidates}});
                return;
            }
            state->resolvedName = match.Name;
            state->isTexture = match.IsTexture;

            // A persistent/imported resource that NO pass writes this frame still
            // has contents — whatever the last frame that did write it left in the
            // pool. Copying that back succeeds and looks like a fresh capture, so
            // a disabled feature reads as "unchanged" rather than "not produced".
            // Report the distinction instead of letting the caller infer it.
            // Our own readback declares a read only, so this never counts itself.
            state->writtenThisFrame = false;
            for (const auto& a : frame->Graph().Accesses())
            {
                if (a.Resource == match.Id && Rendering::RenderGraph::IsWrite(a.Access))
                {
                    state->writtenThisFrame = true;
                    break;
                }
            }

            const auto& desc = frame->Graph().ResourceDesc(match.Id);

            if (match.IsTexture)
            {
                if (desc.SampleCount > 1)
                {
                    state->error = Editor::RefuseRequest("'" + match.Name + "' is multisampled (" +
                                                             std::to_string(desc.SampleCount) +
                                                             " samples) — copy-to-buffer is invalid on an MSAA image; "
                                                             "capture its resolve target instead",
                                                         json{{"resourceName", match.Name}});
                    return;
                }
                const auto fmt = static_cast<Rendering::TextureFormat>(desc.Format);
                if (Rendering::BytesPerPixel(fmt) == 0)
                {
                    state->error = Editor::RefuseRequest("'" + match.Name + "' has no linear texel size (" +
                                                             std::string(Rendering::ToString(fmt)) +
                                                             ") — block-compressed resources cannot be read back",
                                                         json{{"resourceName", match.Name}});
                    return;
                }
                const uint32_t mipCount = desc.MipLevels > 0 ? desc.MipLevels : 1u;
                const uint32_t layerCount = desc.ArrayLayers > 0 ? desc.ArrayLayers : 1u;
                if (state->mip >= mipCount || state->layer >= layerCount)
                {
                    state->error = Editor::RefuseRequest("subresource out of range for '" + match.Name + "'",
                                                         json{{"resourceName", match.Name},
                                                              {"mipLevels", mipCount},
                                                              {"arrayLayers", layerCount}});
                    return;
                }
                state->format = fmt;

                Rendering::RenderGraph::RGTexture tex = frame->FindTexture(match.Name.c_str());
                if (!tex.IsValid())
                {
                    state->error = Editor::RefuseRequest("'" + match.Name + "' is not a texture this frame");
                    return;
                }
                // A raw capture ships texels verbatim, so it must be a region: a
                // full 3072x902 RGBA16F target is 22 MB of bytes (30 MB base64)
                // and would stall the debug channel.
                if (state->raw)
                {
                    const uint32_t mipW = std::max(1u, desc.Width >> state->mip);
                    const uint32_t mipH = std::max(1u, desc.Height >> state->mip);
                    const uint32_t w = state->regionW != 0 ? state->regionW : mipW;
                    const uint32_t h = state->regionH != 0 ? state->regionH : mipH;
                    const uint64_t bytes = static_cast<uint64_t>(w) * h * Rendering::BytesPerPixel(fmt);
                    if (bytes > kMaxRawCaptureBytes)
                    {
                        state->error = Editor::RefuseRequest("raw capture of '" + match.Name + "' would be " +
                                                                 std::to_string(bytes) + " bytes (limit " +
                                                                 std::to_string(kMaxRawCaptureBytes) +
                                                                 ") — narrow it with x/y/w/h",
                                                             json{{"resourceName", match.Name},
                                                                  {"width", mipW},
                                                                  {"height", mipH},
                                                                  {"bytesPerPixel", Rendering::BytesPerPixel(fmt)}});
                        return;
                    }
                }

                state->textureTicket = Rendering::RequestTextureSubresourceReadbackRG(
                    device, *frame, tex, state->mip, state->layer, state->srcX, state->srcY,
                    state->regionW, state->regionH, match.Name.c_str());
                if (!state->textureTicket)
                    state->error = Editor::RefuseRequest("Failed to declare a readback of '" + match.Name +
                                                             "' (region out of bounds?)",
                                                         json{{"resourceName", match.Name}});
                return;
            }

            // ---- buffer ----
            const uint64_t size = desc.SizeBytes;
            if (size == 0 || state->bufferOffset >= size)
            {
                state->error = Editor::RefuseRequest("'" + match.Name + "' offset " +
                                                         std::to_string(state->bufferOffset) +
                                                         " is outside its " + std::to_string(size) + " bytes",
                                                     json{{"resourceName", match.Name}});
                return;
            }
            uint64_t want = state->bufferBytes != 0 ? state->bufferBytes : size - state->bufferOffset;
            want = std::min(want, size - state->bufferOffset);
            want = std::min(want, kMaxBufferCaptureBytes);
            state->bufferBytes = want;

            Rendering::RenderGraph::RGBuffer buf = frame->FindBuffer(match.Name.c_str());
            if (!buf.IsValid())
            {
                state->error = Editor::RefuseRequest("'" + match.Name + "' is not a buffer this frame");
                return;
            }
            state->bufferTicket = Rendering::RequestBufferReadbackRG(
                device, *frame, buf, static_cast<size_t>(state->bufferOffset),
                static_cast<size_t>(want), match.Name.c_str());
            if (!state->bufferTicket)
                state->error = Editor::RefuseRequest("Failed to declare a readback of '" + match.Name + "'",
                                                     json{{"resourceName", match.Name}});
        });

        // Resolution phase: poll until the declaring frame's copy has completed.
        server.EnqueueRenderCallback(windowId, declaration);
        server.EnqueueDeferredResponse(ctx.id,
            [state, declaration, requestId = ctx.id](json& outResult) -> bool
            {
                (void)declaration; // Keep the queued declaration alive until this response ends.
                if (state->encodeJob.IsValid())
                {
                    std::string jobError;
                    if (!Editor::PollHandlerReplyJob(state->encodeJob, outResult, jobError))
                        return false;
                    if (!jobError.empty())
                        outResult = Editor::RefuseRequest("The resource encode job did not complete: " + jobError);
                    return true;
                }
                ++state->polls;
                if (!state->error.is_null())
                {
                    outResult = state->error;
                    return true;
                }
                if (!state->textureTicket && !state->bufferTicket)
                {
                    // The callback needs a live frame; if none arrives the editor
                    // is not driving frames (minimized / paused render loop).
                    constexpr uint32_t kMaxPollsAwaitingFrame = 10;
                    if (state->polls > kMaxPollsAwaitingFrame)
                    {
                        outResult = Editor::RefuseRequest("No render-graph frame was declared while the "
                                                          "capture was pending — is the editor rendering?");
                        return true;
                    }
                    return false;
                }

                constexpr uint32_t kMaxPollsAwaitingGpu = 25;

                if (state->bufferTicket)
                {
                    if (state->bufferTicket->IsConsumed())
                    {
                        outResult = Editor::RefuseRequest("Declaring frame was abandoned before submit");
                        return true;
                    }
                    Rendering::BufferReadbackResult r;
                    if (!state->bufferTicket->TryGet(r))
                    {
                        if (state->polls > kMaxPollsAwaitingGpu)
                        {
                            outResult = Editor::RefuseRequest("Buffer readback never completed");
                            return true;
                        }
                        return false;
                    }
                    outResult = json{
                        {"resourceName", state->resolvedName},
                        {"requestedName", state->requestedName},
                        {"kind", "buffer"},
                        {"frameIndex", state->frameIndex},
                        {"offset", state->bufferOffset},
                        {"byteCount", r.bytes.size()},
                        {"writtenThisFrame", state->writtenThisFrame},
                        {"bytesBase64", Base64Encode(r.bytes.data(), r.bytes.size())}};
                    if (!state->writtenThisFrame)
                        outResult["staleWarning"] =
                            "No pass wrote '" + state->resolvedName +
                            "' this frame — these bytes are whatever the last frame that "
                            "did write it left in the pool, not a current value";
                    if (state->decode == "float")
                    {
                        std::vector<float> v(std::min(r.bytes.size() / sizeof(float),
                                                      kMaxDecodedElements));
                        std::memcpy(v.data(), r.bytes.data(), v.size() * sizeof(float));
                        outResult["floats"] = v;
                    }
                    else if (state->decode == "uint")
                    {
                        std::vector<uint32_t> v(std::min(r.bytes.size() / sizeof(uint32_t),
                                                         kMaxDecodedElements));
                        std::memcpy(v.data(), r.bytes.data(), v.size() * sizeof(uint32_t));
                        outResult["uints"] = v;
                    }
                    return true;
                }

                if (state->textureTicket->IsConsumed())
                {
                    outResult = Editor::RefuseRequest("Declaring frame was abandoned before submit");
                    return true;
                }
                Rendering::ViewReadbackResult result;
                if (!state->textureTicket->TryGet(result))
                {
                    if (state->polls > kMaxPollsAwaitingGpu)
                    {
                        outResult = Editor::RefuseRequest("Texture readback never completed");
                        return true;
                    }
                    return false;
                }

                if (state->raw)
                {
                    outResult = json{
                        {"resourceName", state->resolvedName},
                        {"requestedName", state->requestedName},
                        {"kind", "texture"},
                        {"encoding", "raw"},
                        {"width", result.width},
                        {"height", result.height},
                        {"x", state->srcX},
                        {"y", state->srcY},
                        {"format", Rendering::ToString(result.format)},
                        {"bytesPerPixel", Rendering::BytesPerPixel(result.format)},
                        {"channels", FormatChannelCount(result.format)},
                        {"mip", state->mip},
                        {"layer", state->layer},
                        {"frameIndex", state->frameIndex},
                        {"writtenThisFrame", state->writtenThisFrame},
                        {"byteCount", result.pixels.size()},
                        {"rawBase64", Base64Encode(result.pixels.data(), result.pixels.size())}};
                    if (!state->writtenThisFrame)
                        outResult["staleWarning"] =
                            "No pass wrote '" + state->resolvedName +
                            "' this frame — these texels are whatever the last frame that "
                            "did write it left in the pool, not a current image";
                    return true;
                }

                json metadata{
                    {"resourceName", state->resolvedName},
                    {"requestedName", state->requestedName},
                    {"kind", "texture"},
                    {"width", result.width},
                    {"height", result.height},
                    {"format", Rendering::ToString(result.format)},
                    {"channels", FormatChannelCount(result.format)},
                    {"mip", state->mip},
                    {"layer", state->layer},
                    {"frameIndex", state->frameIndex},
                    {"writtenThisFrame", state->writtenThisFrame}};
                if (!state->writtenThisFrame)
                    metadata["staleWarning"] =
                        "No pass wrote '" + state->resolvedName +
                        "' this frame — these pixels are whatever the last frame that "
                        "did write it left in the pool, not a current image";
                state->encodeJob = Editor::SubmitHandlerReplyJob(
                    EngineCore::GetInstance().GetJobSystem(), requestId,
                    [readback = std::move(result), rangeMin = state->rangeMin,
                     rangeMax = state->rangeMax, metadata = std::move(metadata)]() mutable
                    {
                        return FinishResourceCaptureResult(std::move(readback), rangeMin, rangeMax,
                                                           std::move(metadata));
                    });
                if (!state->encodeJob.IsValid())
                {
                    outResult = Editor::RefuseRequest(
                        "The job system is shutting down; the resource capture was not encoded");
                    return true;
                }
                return false;
            }, Editor::kScreenshotPollBackstop);

        return EditorDebugServer::DeferredMarker(); });

server.RegisterHandler("trigger_capture", [&app, &server](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        auto* rd = app.m_RenderDoc.get();
        if (!rd || !rd->IsAvailable())
            return Editor::RefuseRequest("RenderDoc not available — the editor is not running under "
                                         "RenderDoc. Relaunch it with launch_editor renderdoc=true, or "
                                         "under 'renderdoccmd capture'.");

        uint32_t countBefore = rd->GetCaptureCount();

        rd->TriggerCapture();

        // RenderDoc publishes the capture only once the .rdc is fully written.
        // The render loop stalls while it serializes, so this costs far fewer
        // frames than the wall clock suggests — a measured 133 MB capture took
        // ~2.4 s yet consumed only tens of frames, landing right on the 30-frame
        // default and timing out intermittently. get_capture_status recovers the
        // path when the budget does run out.
        constexpr int kCapturePollBudgetFrames = 300;

        // The capture completes after the next present. Poll for the new
        // capture file over several frames via the deferred response mechanism.
        auto state = std::make_shared<uint32_t>(countBefore);
        server.EnqueueDeferredResponse(ctx.id,
            [&app, state](json& outResult) -> bool
            {
                auto* rd = app.m_RenderDoc.get();
                if (!rd)
                    return false;
                uint32_t current = rd->GetCaptureCount();
                if (current <= *state)
                    return false; // not ready yet

                std::string path = rd->GetLastCapturePath();
                outResult = {
                    {"captured", true},
                    {"captureIndex", current - 1},
                    {"filePath", path}
                };
                return true;
            },
            kCapturePollBudgetFrames);

        return EditorDebugServer::DeferredMarker(); });

server.RegisterHandler("get_capture_status", [&app](const EditorDebugServer::RequestContext&) -> json
                           {
        auto* rd = app.m_RenderDoc.get();
        const bool available = rd && rd->IsAvailable();
        return json{
            {"available", available},
            {"captureCount", available ? rd->GetCaptureCount() : 0u},
            {"lastCapturePath", available ? rd->GetLastCapturePath() : std::string{}}
        }; });

server.RegisterHandler("get_pass_descriptors", [&app, &server](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        std::string passName = ctx.params.value("passName", "");
        if (passName.empty())
            return Editor::RefuseRequest("Provide a 'passName' parameter");

        if (app.m_Windows.empty())
            return Editor::RefuseRequest("No editor window");

        auto* wndCtx = app.m_Windows[0].get();
        auto* device = (wndCtx && wndCtx->renderCtx) ? wndCtx->renderCtx->GetDevice() : nullptr;
        if (!device)
            return Editor::RefuseRequest("No render device");

        // Enable capture for the next frame, filtered to the requested pass.
        device->DebugClearDescriptorCaptures();
        device->DebugSetDescriptorCaptureEnabled(true, passName);

        std::string requestId = ctx.id;
        auto frameCounter = std::make_shared<int>(0);

        server.EnqueueDeferredResponse(requestId,
            [device, passName, frameCounter](json& outResult) -> bool
            {
                ++(*frameCounter);
                // Wait one full frame for the capture to execute.
                if (*frameCounter < 2)
                    return false;

                // Disable capture.
                device->DebugSetDescriptorCaptureEnabled(false, "");

                auto captures = device->DebugGetDescriptorCaptures();

                json draws = json::array();
                for (const auto& entry : captures)
                {
                    json bindings = json::array();
                    for (const auto& b : entry.Bindings)
                    {
                        json bj;
                        bj["binding"] = b.Binding;
                        bj["resourceType"] = b.ResourceType;
                        bj["resourceHandle"] = "0x" + ([](uint64_t v) {
                            char buf[20];
                            snprintf(buf, sizeof(buf), "%llx", static_cast<unsigned long long>(v));
                            return std::string(buf);
                        })(b.ResourceHandle);
                        if (!b.DebugName.empty())
                            bj["debugName"] = b.DebugName;
                        bindings.push_back(std::move(bj));
                    }

                    json drawEntry;
                    drawEntry["passName"] = entry.PassName;
                    drawEntry["drawIndex"] = entry.DrawIndex;
                    drawEntry["descriptorSet"] = "0x" + ([](uint64_t v) {
                        char buf[20];
                        snprintf(buf, sizeof(buf), "%llx", static_cast<unsigned long long>(v));
                        return std::string(buf);
                    })(entry.DescriptorSetHandle);
                    drawEntry["setIndex"] = entry.SetIndex;
                    drawEntry["bindings"] = std::move(bindings);
                    draws.push_back(std::move(drawEntry));
                }

                outResult = {
                    {"passName", passName},
                    {"drawCount", draws.size()},
                    {"draws", std::move(draws)}
                };
                return true;
            });

        return EditorDebugServer::DeferredMarker(); });
}
}
