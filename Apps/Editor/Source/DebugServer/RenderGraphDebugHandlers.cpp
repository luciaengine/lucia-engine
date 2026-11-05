#include "DebugHandlerHelpers.h"

namespace GameEngine
{
void RegisterRenderGraphDebugHandlers(EditorDebugServer& server, EditorApplication& app)
{
server.RegisterHandler("get_render_graph_overview", [&app](const EditorDebugServer::RequestContext& /*ctx*/) -> json
                           {
        RG::RGFrame* frame = MainRgFrame(app);
        if (!frame)
            return Editor::RefuseRequest(kRgNoFrameMsg);
        const RG::RGGraph& g = frame->Graph();
        const RG::RGFrame::FrameStats& s = frame->Stats();

        size_t transient = 0, persistent = 0, imported = 0;
        const size_t resCount = g.ResourceCount();
        for (RG::RGResourceId r = 0; r < resCount; ++r)
        {
            if (!g.IsResourceUsed(r)) continue;
            if (g.IsImported(r)) ++persistent;
            else if (g.IsExternal(r)) ++imported;
            else ++transient;
        }

        json result;
        result["passCount"] = g.ScheduledOrder().size();
        result["declaredPassCount"] = g.PassCount();
        result["culledPassCount"] = g.CulledPassCount();
        result["resourceCount"] = resCount;
        result["barrierCount"] = g.BarrierCount();
        result["barrierBatches"] = s.BarrierBatchesEmitted;
        result["barriersHoisted"] = s.BarriersHoisted;
        result["crossQueueTransitions"] = g.CrossQueueTransitionsConsidered();
        result["crossQueueRelocationsDeclined"] = g.CrossQueueRelocationsDeclined();
        result["renderPassesBegun"] = s.RenderPassesBegun;
        result["submissionCount"] = s.SubmissionsMade;
        result["transientCount"] = transient;
        result["persistentCount"] = persistent;
        result["importedCount"] = imported;
        result["workingSetTransitions"] = g.WorkingSetTransitions();
        result["profilingEnabled"] = frame->ProfilingEnabled();
        result["frameIndex"] = frame->FrameIndex();
        // Render pipeline compiles so far: a count that climbs every frame is a pipeline compiled
        // every frame.
        if (const Engine::Renderer::RenderServices* rs = EngineCore::GetInstance().GetRenderServices())
            result["pipelineCompiles"] = rs->Spine().LastCompileReport().Compiles;
        return result; });

server.RegisterHandler("get_render_graph_passes", [&app](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        RG::RGFrame* frame = MainRgFrame(app);
        if (!frame)
            return Editor::RefuseRequest(kRgNoFrameMsg);
        const RG::RGGraph& g = frame->Graph();
        const auto timings = RgTimingMap(frame);
        const bool includeAccesses = ctx.params.value("includeAccesses", false);

        json passes = json::array();
        uint32_t idx = 0;
        for (RG::RGPassId p : g.ScheduledOrder())
        {
            const char* name = g.PassName(p);
            json jp;
            jp["id"] = p;
            jp["scheduledIndex"] = idx++;
            jp["name"] = name ? name : "(unnamed)";
            jp["queue"] = RgQueueName(g.PassQueue(p));
            jp["phase"] = g.PassPhase(p);
            jp["level"] = g.Level(p);
            jp["barrierCount"] = RgBarrierCountForPass(g, p);
            if (name)
            {
                if (const auto* timing = timings.Find(name))
                {
                    jp["cpuMs"] = timing->CpuMs;
                    jp["workerRecordMs"] = timing->WorkerRecordMs;
                    jp["gpuSpanMs"] = timing->GpuSpanMs;
                }
            }
            if (includeAccesses)
            {
                json accesses = json::array();
                for (const RG::RGAccessRecord& a : g.Accesses())
                {
                    if (a.Pass != p) continue;
                    const char* rn = g.ResourceName(a.Resource);
                    accesses.push_back({{"resource", rn ? rn : "(resource)"},
                                        {"access", RgAccessName(a.Access)},
                                        {"write", RG::IsWrite(a.Access)}});
                }
                jp["accesses"] = std::move(accesses);
            }
            passes.push_back(std::move(jp));
        }
        return json{{"passCount", passes.size()},
                    {"profilingEnabled", frame->ProfilingEnabled()},
                    {"passes", std::move(passes)}}; });

server.RegisterHandler("get_render_graph_resources", [&app](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        RG::RGFrame* frame = MainRgFrame(app);
        if (!frame)
            return Editor::RefuseRequest(kRgNoFrameMsg);
        const RG::RGGraph& g = frame->Graph();

        const Editor::RenderGraphResourceFilter filter(ctx.params);
        if (!filter.Error().empty())
            return Editor::RefuseRequest(filter.Error());

        json resources = json::array();
        const size_t resCount = g.ResourceCount();
        for (RG::RGResourceId r = 0; r < resCount; ++r)
        {
            const RG::RGResourceDesc& d = g.ResourceDesc(r);
            const bool used = g.IsResourceUsed(r);
            const char* kind = d.Kind == RG::RGResourceKind::Buffer                  ? "Buffer"
                               : d.Kind == RG::RGResourceKind::AccelerationStructure ? "AccelerationStructure"
                                                                                       : "Texture";
            const char* lifetime = RgResourceLifetimeName(*frame, r);
            if (!filter.Accepts(kind, lifetime, used))
                continue;

            const char* name = g.ResourceName(r);
            json jr;
            jr["id"] = r;
            jr["name"] = name ? name : "(unnamed)";
            jr["kind"] = kind;
            jr["used"] = used;
            jr["external"] = g.IsExternal(r);
            jr["imported"] = g.IsImported(r);
            jr["lifetime"] = lifetime;
            if (d.Kind == RG::RGResourceKind::Buffer)
            {
                jr["sizeBytes"] = d.SizeBytes;
            }
            else if (d.Kind == RG::RGResourceKind::Texture)
            {
                jr["width"] = d.Width;
                jr["height"] = d.Height;
                jr["format"] = d.Format;
                jr["mipLevels"] = d.MipLevels;
                jr["arrayLayers"] = d.ArrayLayers;
                jr["sampleCount"] = d.SampleCount;
            }
            resources.push_back(std::move(jr));
        }
        // totalResourceCount is the unfiltered size, so a caller can see that a
        // filter applied rather than guessing from the count alone.
        return json{{"resourceCount", resources.size()},
                    {"totalResourceCount", resCount},
                    {"resources", std::move(resources)}}; });

server.RegisterHandler("get_render_graph_pass_detail", [&app](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        RG::RGFrame* frame = MainRgFrame(app);
        if (!frame)
            return Editor::RefuseRequest(kRgNoFrameMsg);
        const std::string wanted = ctx.params.value("name", std::string());
        if (wanted.empty())
            return Editor::RefuseRequest("Provide a 'name' parameter (use get_render_graph_passes to list names)");
        const RG::RGGraph& g = frame->Graph();

        RG::RGPassId target = RG::kInvalidId;
        for (RG::RGPassId p : g.ScheduledOrder())
        {
            const char* n = g.PassName(p);
            if (n && wanted == n) { target = p; break; }
        }
        if (target == RG::kInvalidId)
            return Editor::RefuseRequest("Pass not in the current scheduled graph");

        json accesses = json::array();
        for (const RG::RGAccessRecord& a : g.Accesses())
        {
            if (a.Pass != target) continue;
            const char* rn = g.ResourceName(a.Resource);
            accesses.push_back({{"resource", rn ? rn : "(resource)"},
                                {"access", RgAccessName(a.Access)},
                                {"write", RG::IsWrite(a.Access)}});
        }

        json barriers = json::array();
        const auto& flat = g.Barriers();
        for (const auto& batch : g.BarrierBatches())
        {
            if (batch.Pass != target) continue;
            for (uint32_t i = 0; i < batch.Count; ++i)
            {
                const uint32_t bi = batch.First + i;
                if (bi >= flat.size()) break;
                const RG::RGBarrier& b = flat[bi];
                const char* rn = g.ResourceName(b.Resource);
                barriers.push_back({{"resource", rn ? rn : "(resource)"},
                                    {"from", RgLayoutName(b.OldLayout)},
                                    {"to", RgLayoutName(b.NewLayout)}});
            }
        }

        json successors = json::array();
        for (RG::RGPassId succ : g.PassSuccessors(target))
        {
            const char* n = g.PassName(succ);
            successors.push_back(n ? n : "(unnamed)");
        }

        json detail;
        detail["name"] = wanted;
        detail["queue"] = RgQueueName(g.PassQueue(target));
        detail["phase"] = g.PassPhase(target);
        detail["level"] = g.Level(target);
        detail["barrierCount"] = RgBarrierCountForPass(g, target);
        detail["accesses"] = std::move(accesses);
        detail["barriers"] = std::move(barriers);
        detail["successors"] = std::move(successors);
        const auto timings = RgTimingMap(frame);
        if (const auto* timing = timings.Find(wanted))
        {
            detail["cpuMs"] = timing->CpuMs;
            detail["workerRecordMs"] = timing->WorkerRecordMs;
            detail["gpuSpanMs"] = timing->GpuSpanMs;
        }
        return detail; });

server.RegisterHandler("get_render_graph_dependencies", [&app](const EditorDebugServer::RequestContext& /*ctx*/) -> json
                           {
        RG::RGFrame* frame = MainRgFrame(app);
        if (!frame)
            return Editor::RefuseRequest(kRgNoFrameMsg);
        const RG::RGGraph& g = frame->Graph();
        json edges = json::array();
        for (RG::RGPassId p : g.ScheduledOrder())
        {
            const char* from = g.PassName(p);
            for (RG::RGPassId succ : g.PassSuccessors(p))
            {
                const char* to = g.PassName(succ);
                edges.push_back({{"from", from ? from : "(unnamed)"},
                                 {"to", to ? to : "(unnamed)"}});
            }
        }
        return json{{"edgeCount", edges.size()}, {"edges", std::move(edges)}}; });

server.RegisterHandler("get_render_graph_validation", [&app](const EditorDebugServer::RequestContext& /*ctx*/) -> json
                           {
        RG::RGFrame* frame = MainRgFrame(app);
        if (!frame)
            return Editor::RefuseRequest(kRgNoFrameMsg);
        const RG::RGGraph& g = frame->Graph();
        auto reasonName = [](RG::RGCullReason r) -> const char* {
            switch (r)
            {
            case RG::RGCullReason::NotCulled:      return "NotCulled";
            case RG::RGCullReason::NoConsumer:     return "NoConsumer";
            case RG::RGCullReason::ProducerCulled: return "ProducerCulled";
            case RG::RGCullReason::Cycle:           return "Cycle";
            }
            return "?";
        };
        json issues = json::array();
        const size_t passCount = g.PassCount();
        for (RG::RGPassId p = 0; p < passCount; ++p)
        {
            if (!g.IsCulled(p)) continue;
            const char* n = g.PassName(p);
            issues.push_back({{"pass", n ? n : "(unnamed)"},
                              {"severity", "warning"},
                              {"reason", reasonName(g.CullReason(p))}});
        }
        return json{{"errorCount", 0},
                    {"warningCount", issues.size()},
                    {"passIssues", std::move(issues)}}; });
}
}
