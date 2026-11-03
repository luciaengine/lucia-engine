#include "DebugHandlerHelpers.h"

namespace GameEngine
{
void RegisterProfilerDebugHandlers(EditorDebugServer& server, EditorApplication& app)
{
server.RegisterHandler("get_cpu_profiler", [](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        auto& prof = Profiling::CpuProfiler::Get();
        const bool wasEnabled = prof.IsEnabled();
        const bool wantEnabled = ctx.params.value("enable", true);

        std::vector<Profiling::CpuProfiler::TreeRow> tree;
        prof.CopyFrameTree(tree);

        std::vector<std::pair<std::string_view, Profiling::CpuProfiler::Sample>> flat;
        prof.CopyFrameSamples(flat);
        std::sort(flat.begin(), flat.end(),
                  [](const auto& a, const auto& b) { return a.second.totalMs > b.second.totalMs; });

        const std::size_t maxRows = ctx.params.value("maxRows", static_cast<std::size_t>(64));

        json treeJson = json::array();
        const std::size_t nt = std::min(tree.size(), maxRows);
        for (std::size_t i = 0; i < nt; ++i)
        {
            const auto& r = tree[i];
            treeJson.push_back({
                {"name", r.name},
                {"depth", r.depth},
                {"totalMs", r.totalMs},
                {"selfMs", r.selfMs},
                {"calls", r.callCount}
            });
        }

        json flatJson = json::array();
        const std::size_t nf = std::min(flat.size(), maxRows);
        for (std::size_t i = 0; i < nf; ++i)
        {
            const auto& sample = flat[i].second;
            // Which thread paid for it. The call tree is main-thread only, so
            // for an off-main scope this row is the whole report, and reading
            // it as main-thread cost would misattribute a job worker's work.
            const char* thread = "main";
            if (sample.offMainCount > 0)
                thread = (sample.offMainCount == sample.count) ? "off-main" : "mixed";

            flatJson.push_back({
                {"name", std::string(flat[i].first)},
                {"totalMs", sample.totalMs},
                {"calls", sample.count},
                {"thread", thread}
            });
        }

        // State change last: a disarming call still returns the frame the
        // profiler had already paid for.
        if (wantEnabled != wasEnabled)
            prof.SetEnabled(wantEnabled);

        json result;
        result["enabled"] = prof.IsEnabled();
        result["wasEnabledOnEntry"] = wasEnabled;
        result["tree"] = treeJson;
        result["flat"] = flatJson;
        return result; });

server.RegisterHandler("get_gpu_profiler", [&app](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        RG::RGFrame* frame = MainRgFrame(app);
        if (!frame)
            return Editor::RefuseRequest(kRgNoFrameMsg);

        if (ctx.params.contains("enabled"))
            frame->SetProfilingEnabled(ctx.params.value("enabled", false));

        const auto timings = frame->LastFrameTimings();
        const RG::RGFrame::RGProfilingResolveStats rs = frame->LastResolveStats();
        const bool encoderSpan = frame->TimingSemantics() == Rendering::TimestampSemantics::EncoderSpan;

        json passes = json::array();
        double totalCpu = 0.0;
        for (const auto& t : timings)
        {
            totalCpu += t.CpuMs;
            passes.push_back({{"name", t.Name},
                              {"queue", RgQueueName(t.Queue)},
                              {"phase", t.Phase},
                              {"cpuMs", t.CpuMs},
                              {"gpuSpanMs", t.GpuSpanMs},
                              {"spanShared", t.SpanShared},
                              {"spanCounted", t.SpanCounted}});
        }

        json warnings = json::array();
        if (encoderSpan)
        {
            warnings.push_back(
                "gpuSpanMs values are summed command-encoder spans, not additive pass costs: "
                "one encoder can carry several passes and is then reported to each of them "
                "(spanShared), and encoders overlap one another because the GPU runs them "
                "concurrently. Treat each as an upper bound and do NOT sum them — use "
                "resolveStats.distinctSpanGpuMs (itself an upper bound) or the device's GPU frame "
                "period for a frame total. To total a group of passes, add only the ones with "
                "spanCounted true: each measurement is counted on exactly one pass.");
        }
        if (rs.NonMonotonic > 0)
        {
            warnings.push_back("resolveStats.nonMonotonicTimestamps = " +
                               std::to_string(rs.NonMonotonic) +
                               ": that many passes' raw timestamp pairs closed before they opened "
                               "and were discarded, so their gpuSpanMs reads 0.");
        }
        if (rs.ReadFailures > 0 || rs.InvalidQueryIndices > 0)
        {
            warnings.push_back("resolveStats readFailures/invalidQueryIndices are non-zero: those "
                               "passes have no measurement this frame and read 0.");
        }

        json result;
        result["enabled"] = frame->ProfilingEnabled();
        if (!frame->ProfilingEnabled())
            result["hint"] = "Profiling disarmed — call with {\"enabled\": true}, then poll again.";
        result["timingSemantics"] = encoderSpan ? "encoder-span" : "pipeline-point";
        result["warnings"] = std::move(warnings);
        result["resolveStats"] = {
            {"invalidQueryIndices", rs.InvalidQueryIndices},
            {"readFailures", rs.ReadFailures},
            {"nonMonotonicTimestamps", rs.NonMonotonic},
            {"resolvedPasses", rs.ResolvedPasses},
            {"sharedSpanPasses", rs.SharedSpanPasses},
            {"distinctSpanGpuMs", rs.DistinctSpanGpuMs}
        };
        result["lastFrame"] = {
            {"passes", std::move(passes)},
            {"totalCpuMs", totalCpu}
        };
        return result; });

server.RegisterHandler("get_monitors", [](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        auto& metrics = Debug::DebugMetrics::Get();

        json result;
        result["paused"] = metrics.IsPaused();

        std::vector<Debug::MonitorInfo> infos;
        metrics.Snapshot(infos);
        std::sort(infos.begin(), infos.end(),
                  [](const Debug::MonitorInfo& a, const Debug::MonitorInfo& b) {
                      return a.Name < b.Name;
                  });

        json monitors = json::array();
        for (const auto& info : infos)
        {
            const char* typeStr = "Quantity";
            switch (info.Type)
            {
                case Debug::MonitorType::Memory:  typeStr = "Memory";  break;
                case Debug::MonitorType::TimeMs:  typeStr = "TimeMs";  break;
                case Debug::MonitorType::Percent: typeStr = "Percent"; break;
                default: break;
            }
            monitors.push_back({
                {"name", info.Name},
                {"type", typeStr},
                {"unit", info.Unit},
                {"latest", info.Latest},
                {"peak", info.Peak},
                {"average", info.Average}
            });
        }
        result["monitors"] = monitors;

        if (auto it = ctx.params.find("name"); it != ctx.params.end() && it->is_string())
        {
            std::vector<float> history;
            const std::size_t count = metrics.CopyHistory(it->get<std::string>(), history);
            // Return only the valid tail so the JSON isn't padded with zeros.
            json histJson = json::array();
            const std::size_t start = history.size() > count ? 0 : 0;
            const std::size_t len = std::min(count, history.size());
            for (std::size_t i = start; i < len; ++i)
                histJson.push_back(history[i]);
            result["history"] = {
                {"name", it->get<std::string>()},
                {"samples", histJson},
                {"count", count}
            };
        }

        return result; });
}
}
