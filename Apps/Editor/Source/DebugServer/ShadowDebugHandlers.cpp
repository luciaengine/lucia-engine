#include "DebugHandlerHelpers.h"

namespace GameEngine
{
void RegisterShadowDebugHandlers(EditorDebugServer& server, EditorApplication& app)
{
server.RegisterHandler("toggle_shadow_debug", [](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        auto* rs = EngineCore::GetInstance().GetRenderServices();
        if (!rs)
            return Editor::RefuseRequest("No RenderServices");

        auto* feature = rs->GetFeature<Engine::Renderer::ShadowMapRenderFeature>();
        if (!feature)
            return Editor::RefuseRequest("ShadowMapRenderFeature not initialized");

        using Mode = Engine::Renderer::ShadowDebugMode;
        Mode mode;
        if (ctx.params.contains("mode"))
        {
            int m = ctx.params["mode"].get<int>();
            mode = static_cast<Mode>(std::clamp(m, 0, static_cast<int>(Mode::Count) - 1));
        }
        else
        {
            // Cycle through all shadow debug views.
            int current = static_cast<int>(feature->GetDebugMode());
            mode = static_cast<Mode>((current + 1) % static_cast<int>(Mode::Count));
        }

        feature->SetDebugMode(mode);

        static const char* kModeNames[] = {"off", "cascade_colors", "shadow_factor", "pcss_branch", "base_shadow_factor", "contact_shadow_factor", "contact_only", "base_only"};
        return json{
            {"ok", true},
            {"mode", static_cast<int>(mode)},
            {"modeName", kModeNames[static_cast<int>(mode)]}
        }; });

server.RegisterHandler("toggle_shadow_thumbnails", [](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        auto* rs = EngineCore::GetInstance().GetRenderServices();
        if (!rs)
            return Editor::RefuseRequest("No RenderServices");

        auto* feature = rs->GetFeature<Engine::Renderer::ShadowMapRenderFeature>();
        if (!feature)
            return Editor::RefuseRequest("ShadowMapRenderFeature not initialized");

        bool enable;
        if (ctx.params.contains("enable"))
            enable = ctx.params["enable"].get<bool>();
        else
            enable = !feature->GetShowThumbnails();

        feature->SetShowThumbnails(enable);

        return json{{"ok", true}, {"enabled", enable}}; });

server.RegisterHandler("set_shadow_quality", [](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        auto* rs = EngineCore::GetInstance().GetRenderServices();
        if (!rs)
            return Editor::RefuseRequest("No RenderServices");

        auto* feature = rs->GetFeature<Engine::Renderer::ShadowMapRenderFeature>();
        if (!feature)
            return Editor::RefuseRequest("ShadowMapRenderFeature not initialized");

        int quality = ctx.params.value("quality", 0);
        feature->SetPcfQuality(std::clamp(
            quality, 0, static_cast<int>(Engine::Renderer::ShadowFilterQuality::DPCF)));

        static const char* kNames[] = {
            "5x5 weighted PCF (9 taps)",
            "3x3 grid PCF (9 taps)",
            "Poisson PCF (variable taps)",
            "PCSS (contact hardening)",
            "MSM4 (Moment Shadow Maps)",
            "DPCF (contact hardening, no blocker search)",
        };
        return json{
            {"ok", true},
            {"quality", feature->GetPcfQuality()},
            {"description", kNames[feature->GetPcfQuality()]}
        }; });

server.RegisterHandler("set_shadow_pcss", [&app](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        auto* rs = EngineCore::GetInstance().GetRenderServices();
        if (!rs)
            return Editor::RefuseRequest("No RenderServices");

        auto* feature = rs->GetFeature<Engine::Renderer::ShadowMapRenderFeature>();
        if (!feature)
            return Editor::RefuseRequest("ShadowMapRenderFeature not initialized");

        if (ctx.params.contains("enabled"))
            feature->SetPcssEnabled(ctx.params.value("enabled", true));
        if (ctx.params.contains("maxPenumbra"))
            feature->SetPcssMaxPenumbra(ctx.params.value("maxPenumbra", feature->GetPcssMaxPenumbra()));
        if (ctx.params.contains("receiverPlaneBias"))
            feature->SetPcssReceiverPlaneBias(ctx.params.value("receiverPlaneBias", feature->IsPcssReceiverPlaneBias()));
        // Tap count bounds the kernel now that the penumbra cap is derived from
        // it, so tuning softness against banding needs it live rather than per
        // rebuild. The setter snaps to {8,16,32,64}.
        if (ctx.params.contains("tapCount"))
            feature->SetPcssTapCount(ctx.params.value("tapCount", feature->GetPcssTapCount()));
        // Dither basis is a look A/B rather than a quality tier, so it belongs
        // on the live knob for the same reason tapCount does: the question it
        // answers is "which of these two reads better at this range", and that
        // cannot be asked one rebuild at a time.
        // Cascade projection mode: 0 = Stable, 1 = Close, 2 = WorldTexel.
        // Live because the whole point of WorldTexel is a look/stability A/B
        // against the two fitted modes, and a rebuild per arm makes that
        // comparison unusable.
        if (ctx.params.contains("projection"))
        {
            const int mode = std::clamp(ctx.params.value("projection", 1), 0, 2);
            feature->SetShadowProjection(static_cast<Engine::Renderer::ShadowProjection>(mode));
        }
        if (ctx.params.contains("cascade0TexelSize") || ctx.params.contains("cascadeTexelRatio"))
        {
            feature->SetWorldTexelLadder(
                ctx.params.value("cascade0TexelSize", feature->GetCascade0TexelSize()),
                ctx.params.value("cascadeTexelRatio", feature->GetCascadeTexelRatio()));
        }
        if (ctx.params.contains("ditherBasis"))
        {
            const int basis = ctx.params.value("ditherBasis", 0);
            feature->SetDitherBasis(basis == 1 ? Engine::Renderer::ShadowDitherBasis::ShadowSpace
                                               : Engine::Renderer::ShadowDitherBasis::Screen);
        }

        const bool stableShadowFallback =
            rs->GetDevice() &&
            rs->GetDevice()->GetCapabilities().prefersStableShadowFiltering;

        // Resolve through the same fallback chain the shader actually runs
        // this frame, including the MSM4-without-moments promotion into PCSS
        // -- a hand-rolled IsPcssEnabled() check only sees the requested
        // FilterQuality and misses that promotion entirely.
        Rendering::ViewId viewId = 0;
        if (!app.m_Windows.empty() && app.m_Windows[0]->scene)
            viewId = app.m_Windows[0]->scene->GetViewId();
        const bool effective =
            feature->ResolveEffectiveFilterQuality(*rs, viewId) ==
            Engine::Renderer::ShadowFilterQuality::PCSS;

        return json{
            {"ok", true},
            {"enabled", feature->IsPcssEnabled()},
            {"maxPenumbra", feature->GetPcssMaxPenumbra()},
            {"receiverPlaneBias", feature->IsPcssReceiverPlaneBias()},
            // Every settable field reports back. A knob that accepts a value and
            // returns only {"ok":true} cannot be distinguished from one that
            // silently ignored it -- tapCount was write-only until now.
            {"tapCount", feature->GetPcssTapCount()},
            {"ditherBasis", static_cast<int>(feature->GetDitherBasis())},
            {"projection", static_cast<int>(feature->GetShadowProjection())},
            {"cascade0TexelSize", feature->GetCascade0TexelSize()},
            {"cascadeTexelRatio", feature->GetCascadeTexelRatio()},
            {"bindlessAvailable", rs->Textures().IsBindlessEnabled()},
            {"stableShadowFallback", stableShadowFallback},
            {"effective", effective}
        }; });

server.RegisterHandler("set_moments_resolution", [](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        auto* rs = EngineCore::GetInstance().GetRenderServices();
        if (!rs)
            return Editor::RefuseRequest("No RenderServices");
        auto* feature = rs->GetFeature<Engine::Renderer::ShadowMapRenderFeature>();
        if (!feature)
            return Editor::RefuseRequest("ShadowMapRenderFeature not initialized");
        uint32_t res = ctx.params.value("resolution", 1024u);
        feature->SetMomentsResolution(res);
        return json{
            {"ok", true},
            {"resolution", feature->GetMomentsResolution()},
        }; });

server.RegisterHandler("set_msm_blur_mode", [](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        auto* rs = EngineCore::GetInstance().GetRenderServices();
        if (!rs)
            return Editor::RefuseRequest("No RenderServices");
        auto* feature = rs->GetFeature<Engine::Renderer::ShadowMapRenderFeature>();
        if (!feature)
            return Editor::RefuseRequest("ShadowMapRenderFeature not initialized");
        int mode = ctx.params.value("mode", 0);
        feature->SetMsmBlurMode(static_cast<Engine::Renderer::MsmBlurMode>(std::clamp(mode, 0, 1)));
        static const char* kNames[] = {"Linear5Tap", "Discrete9Tap"};
        return json{
            {"ok", true},
            {"mode", static_cast<int>(feature->GetMsmBlurMode())},
            {"description", kNames[static_cast<int>(feature->GetMsmBlurMode())]},
        }; });
}
}
