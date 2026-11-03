#include "DebugHandlerHelpers.h"

namespace GameEngine
{
void RegisterRenderStatsDebugHandlers(EditorDebugServer& server, EditorApplication& app)
{
server.RegisterHandler("get_validation_stats", [](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        auto* rs = EngineCore::GetInstance().GetRenderServices();
        auto* device = rs ? rs->GetDevice() : nullptr;
        if (!device)
            return Editor::RefuseRequest("No rendering device");
        const bool reset = ctx.params.value("reset", false);
        const auto stats = device->GetValidationStats();
        if (reset)
            device->ResetValidationStats();
        json vuids = json::array();
        for (const auto& v : stats.Vuids)
        {
            vuids.push_back({{"id", v.Vuid},
                             {"severity", v.IsError ? "error" : "warning"},
                             {"count", v.Count},
                             {"suppressedCount", v.SuppressedCount},
                             {"firstFrame", v.FirstFrame},
                             {"lastFrame", v.LastFrame},
                             {"firstMessage", v.FirstMessage},
                             {"objects", v.FirstObjects},
                             {"labels", v.FirstLabels}});
        }
        return json{{"enabled", stats.Enabled},
                    {"frameSerial", stats.FrameSerial},
                    {"errors", stats.ErrorCount},
                    {"warnings", stats.WarningCount},
                    {"suppressed", stats.SuppressedCount},
                    {"overflow", stats.OverflowCount},
                    {"vuids", std::move(vuids)}}; });

server.RegisterHandler("get_render_stats", [&app](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        json result;
        auto* world = EngineCore::GetInstance().GetPrimaryWorld();
        result["entityCount"] = world ? static_cast<uint64_t>(world->GetEntityCount()) : 0;
        if (world)
        {
            // Change-signaling telemetry (P1.5 IdleEditorStampCount gate):
            // world-wide column write-grant stamps + the grant counter. Diff
            // across frames - an idle editor frame stamp delta is ~0.
            result["ecsColumnStamps"] = world->GetColumnStampCount();
            result["ecsGlobalSystemVersion"] = world->GetGlobalSystemVersion();
        }
        // App-loop timing straight from Application — independent of the
        // DebugMetrics ring, and frameCount lets callers diff exact frames.
        result["frameCount"] = app.GetFrameCount();
        result["fps"] = app.GetFps();
        result["frameMsMean"] = app.GetFrameMsMean();
        result["frameMsWorst"] = app.GetFrameMsWorst();

        // Scene-close lifetime counters. Poll once per scene open (allowing the
        // deferred-destroy quarantine to drain first — the texture destroy is
        // deferred by frames-in-flight, so an immediate sample cannot tell a
        // pending retire from a leak). slotCount is the load-bearing one: the
        // slot tables never shrink, so a table growing per open is the leak
        // whatever the active counts say.
        {
            json terrain;
            if (auto* svc = TerrainECS::TerrainService::TryGet())
            {
                terrain["activeCount"] = svc->GetActiveTerrainCount();
                terrain["activeTiledCount"] = svc->GetActiveTiledTerrainCount();
                terrain["slotCount"] = svc->GetTerrainSlotCount();
                terrain["tiledSlotCount"] = svc->GetTiledTerrainSlotCount();
            }
            if (auto* rs = EngineCore::GetInstance().GetRenderServices())
            {
                if (auto* feature = rs->GetFeature<TerrainECS::TerrainRenderFeature>())
                    terrain["textureRetireGeneration"] =
                        feature->GetTerrainTextureRetireGeneration();
            }
            if (!terrain.empty())
                result["terrain"] = terrain;

            if (auto* pw = PhysicsECS::PhysicsWorldService::TryGet())
                result["physics"] = {{"bodyCount", pw->GetBodyCount()}};
            if (auto* splines = SplineECS::SplineService::TryGet())
                result["splines"] = {{"activeCount", splines->GetActiveSplineCount()}};
        }

        if (auto* rs = EngineCore::GetInstance().GetRenderServices())
        {
            json renderServices;
            renderServices["meshEntryCount"] = rs->GetMeshGPURegistry().GetEntryCount();
            renderServices["drawStreamSlots"] =
                rs->GetDrawStreamBuilder() ? static_cast<uint64_t>(rs->GetDrawStreamBuilder()->GetRangeCount()) : 0ull;
            // Non-skipped MaterialParams repacks; flat while idle = the
            // PackMaterialSSBO skip gate is engaging.
            renderServices["materialSSBOPackCount"] = rs->Materials().GetMaterialSSBOPackCount();
            // Frames whose material rows overflowed the MaterialParams ring and
            // shaded from the zero-filled fallback. Non-zero means the scene has
            // outgrown the ring's starting capacity; still climbing after a few
            // frames means it has outgrown the grow cap.
            renderServices["materialParamsOverflowCount"] =
                rs->Materials().GetMaterialParamsOverflowCount();

            // Mesh residency gate. The `*Total` numbers are monotonic for the
            // process and are what a steady-state claim is checked against:
            // `routeBRefusedNonResidentTotal` and `routeASkippedNonResidentTotal`
            // must read 0. The unsuffixed siblings describe the CURRENT frame
            // (Route B, cleared by BeginFrame) and the CURRENT pool-group plan
            // (Route A, rebuilt by every Refresh), so a zero there means only
            // that nothing was refused in the sample window.
            // `routeBRefusedBucketMissing` is a designed state (tombstones,
            // failed allocations, meshes with no built geometry) and is
            // reported without being a defect signal. `routeARefreshes` scopes
            // Route A: it stays 0 with draw consolidation off, where Route A
            // does not run and its zeros carry no information.
            // `windowExhausted` cannot become non-zero while the upload bracket
            // is a stamp's only obligation — see MeshGPUResidencyStats.
            {
                const auto residency = rs->GetMeshGPURegistry().GetResidencyStats();
                const auto poolGroups = rs->GetMeshPoolGroupResidencyStats();
                renderServices["meshResidency"] = {
                    {"routeBRefusedNonResident", residency.RouteBRefusedNonResident},
                    {"routeBRefusedNonResidentTotal", residency.RouteBRefusedNonResidentTotal},
                    {"routeBRefusedBucketMissing", residency.RouteBRefusedBucketMissing},
                    {"routeBRefusedBucketMissingTotal", residency.RouteBRefusedBucketMissingTotal},
                    {"routeASkippedNonResident", poolGroups.SkippedNonResident},
                    {"routeASkippedNonResidentTotal", poolGroups.SkippedNonResidentTotal},
                    {"routeARefreshes", poolGroups.Refreshes},
                    {"windowExhausted", residency.WindowExhausted}
                };
            }

            // Per-pool suballocator occupancy, one entry per pool buffer.
            // `fragRatio` is (capacity - used - largestFreeRange) / capacity:
            // 0.0 is perfectly packed, and a high value with plenty of free
            // bytes means live ranges are splitting the free space rather than
            // the pool being full. That matters because a best-fit failure does
            // not fail the upload — it appends a sibling pool at double the
            // capacity, which grows GPU memory and adds a pool group (and so an
            // indirect draw). `freeRangeCount` is the same story per frame:
            // morph's free-then-reallocate cannot reuse a freed range for
            // kRetireDelay frames, so a climbing count is that churn showing up.
            {
                json meshPools = json::array();
                for (const auto& pool : rs->GetMeshGPURegistry().GetPoolStats())
                {
                    meshPools.push_back({
                        {"bucketKey", static_cast<uint32_t>(pool.BucketKey)},
                        {"stream", pool.Stream},
                        {"poolIndex", pool.PoolIndex},
                        {"capacity", pool.Allocator.capacity},
                        {"used", pool.Allocator.used},
                        {"peak", pool.Allocator.peak},
                        {"largestFreeRange", pool.Allocator.largestFreeRange},
                        {"freeRangeCount", pool.Allocator.freeRangeCount},
                        {"deferredCount", pool.Allocator.deferredCount},
                        {"fragRatio", pool.Allocator.fragRatio}
                    });
                }
                renderServices["meshPools"] = std::move(meshPools);
            }

            // A2.1: RenderExtractionSystem sub-phase wall-clock timing (last
            // frame + short windowed mean/max) plus record/rebuild counters.
            {
                const auto& ex = rs->GetRenderExtractionStats();
                renderServices["extraction"] = {
                    {"totalMs", ex.TotalMs},
                    {"lightsMs", ex.LightsMs},
                    {"gatherMs", ex.GatherMs},
                    {"processMs", ex.ProcessMs},
                    // A2.1 parallel lane sub-splits (0 in the serial OFF lane).
                    {"prepareMs", ex.PrepareMs},
                    {"applyMs", ex.ApplyMs},
                    {"submitMs", ex.SubmitMs},
                    {"totalMsMean", ex.TotalMsMean},
                    {"gatherMsMean", ex.GatherMsMean},
                    {"processMsMean", ex.ProcessMsMean},
                    {"prepareMsMean", ex.PrepareMsMean},
                    {"applyMsMean", ex.ApplyMsMean},
                    {"totalMsMax", ex.TotalMsMax},
                    {"recordCount", ex.RecordCount},
                    {"submissionCount", ex.SubmissionCount},
                    {"rebuildCount", ex.RebuildCount},
                    {"skippedCount", ex.SkippedCount},
                    // GE_EXTRACTION_FEED lane telemetry (fusion S2b): the
                    // soak/bench instrument — fast-frame ratio, per-trigger
                    // escalation bits (E9 split out for OnDemand view churn),
                    // per-shape pending breakdown (a permanent full-lane pin
                    // is attributable in one poll), and the S0 composition
                    // counts (residual ~= CLEANUP only holds when these are
                    // zero).
                    {"fastFrame", ex.FastFrame},
                    {"fastFramesInWindow", ex.FastFramesInWindow},
                    {"windowTicks", ex.WindowTicks},
                    {"escalationReasonBits", ex.EscalationReasonBits},
                    {"feedPatchedCount", ex.FeedPatchedCount},
                    {"subsetRefreshedCount", ex.SubsetRefreshedCount},
                    {"pendingMeshEntry", ex.PendingMeshEntry},
                    {"pendingMaterialLoad", ex.PendingMaterialLoad},
                    {"pendingPipeline", ex.PendingPipeline},
                    {"pendingSentinelIndex", ex.PendingSentinelIndex},
                    {"particleEmitterCount", ex.ParticleEmitterCount},
                    {"liveParticles", ex.LiveParticles},
                    {"particleSimulationMs", ex.ParticleSimulationMs},
                    {"volumeCount", ex.VolumeCount},
                    {"oceanCount", ex.OceanCount}
                };
            }

            // A2 STEP-0: render-thread CPU timeline. Stitches the RenderServices
            // sort/bucketer brackets with the main window's RenderGraph phase +
            // record-floor brackets so one poll = the whole CPU decomposition
            // (alongside the extraction block above). The record-walk bucket is
            // the summed per-pass cpuMs from get_gpu_profiler (profiling-gated).
            {
                const auto& tl = rs->GetRenderTimelineStats();
                json timeline = {
                    {"sortMs", tl.SortMs},
                    {"sortViewCount", tl.SortViewCount},
                    {"sortMsMean", tl.SortMsMean},
                    {"bucketerScheduleMs", tl.BucketerScheduleMs},
                    {"bucketerScheduleMsMean", tl.BucketerScheduleMsMean}
                };
                if (RG::RGFrame* frame = MainRgFrame(app))
                {
                    const RG::RGFrame::FrameStats& fs = frame->Stats();
                    timeline["renderGraph"] = {
                        {"compileMs", fs.CompileMs},
                        {"scheduleMs", fs.ScheduleMs},
                        {"generateBarriersMs", fs.GenerateBarriersMs},
                        {"buildSubmissionPlanMs", fs.BuildSubmissionPlanMs},
                        {"queueSubmitMs", fs.QueueSubmitMs},
                        {"barrierEmitMs", fs.BarrierEmitMs},
                        {"submissionsMade", fs.SubmissionsMade},
                        {"renderPassesBegun", fs.RenderPassesBegun}
                    };
                }
                // Per-ECS-system wave timings (SystemManager performance data;
                // lazily enabled on first read — the parallel wave path times
                // each system on its worker). This is the engineUpdateMs
                // decomposition the A2 P0-D branch decision reads.
                if (auto* loop = EngineCore::GetInstance().GetRenderingLoop())
                {
                    if (auto* sm = loop->GetSystemManager())
                    {
                        sm->SetPerformanceTracking(true);
                        json systemsJson = json::array();
                        for (const auto& perf : sm->GetPerformanceData())
                        {
                            if (perf.UpdateCount == 0)
                                continue;
                            systemsJson.push_back({{"name", perf.Name},
                                                   {"lastMs", perf.LastTime},
                                                   {"avgMs", perf.AverageTime},
                                                   {"maxMs", perf.MaxTime}});
                        }
                        timeline["ecsSystems"] = std::move(systemsJson);
                        // maxMs is the slowest update since tracking began (a scene load's
                        // bake included). resetSystemMaxima clears it after this read, so the
                        // next read's maxMs is the worst single frame in between.
                        if (ctx.params.value("resetSystemMaxima", false))
                            sm->ResetPerformanceMaxima();
                    }
                }
                renderServices["renderThreadCpu"] = std::move(timeline);
            }

            if (auto* gpuScene = rs->GetGPUScene())
            {
                // Cumulative; the bench diffs samples for per-frame upload BW.
                renderServices["instanceUploadBytesTotal"] = gpuScene->GetInstanceUploadBytesTotal();
                renderServices["gpuSceneInstanceCount"] = gpuScene->GetInstanceCount();
                renderServices["gpuSceneLiveInstanceCount"] = gpuScene->GetLiveInstanceCount();
            }
            if (auto* device = rs->GetDevice())
            {
                renderServices["lastGraphicsPipelineFailure"] = device->GetLastGraphicsPipelineFailure();

                // Output-chain facts: the negotiated swapchain and the
                // terminal encode most recently declared for the backbuffer,
                // so a capture/A-B self-documents its output condition instead
                // of relying on log archaeology. swapchainNativeFormat /
                // swapchainNativeColorSpace are the raw VkFormat /
                // VkColorSpaceKHR the WSI negotiation returned (0 before a
                // swapchain exists); backbufferOutEncoding follows the encode
                // shader's arms (1=sRGB, 2=PQ, 3=HLG, 4=passthrough,
                // 5=encoded-input raw requantize, 6=encoded-input D(c) for an
                // _SRGB ROP, 0=not yet declared).
                const Rendering::HdrOutputState hdrState = device->GetHdrOutputState();
                const auto encodeFacts = Rendering::Passes::GetBackbufferEncodeFacts();
                renderServices["output"] = {
                    {"swapchainNativeFormat", hdrState.display.nativeFormat},
                    {"swapchainNativeColorSpace", hdrState.display.nativeColorSpace},
                    {"swapchainTextureFormat", static_cast<int>(device->GetSwapchainTextureFormat())},
                    {"swapchainNeedsManualSRGBEncode", device->SwapchainNeedsManualSRGBEncode()},
                    {"hdrMode", Rendering::HdrOutputModeToString(hdrState.activeMode)},
                    {"backbufferOutEncoding", encodeFacts.OutEncoding},
                    {"backbufferDitherLsb", encodeFacts.DitherLsb}
                };
                // The effective deband gate of the most recent backbuffer encode.
                // On an SDR editor frame that encode is a filter-off transfer
                // (backbufferDitherLsb 0), and the gate reported here is the one
                // the world views used inside their own finalize.
                renderServices["output"]["debandThresholdLsb"] =
                    Rendering::Passes::GetBackbufferOutputDebandThresholdLsb();

                // Deferred-destroy queue depths, for watching a retire path that
                // has stopped retiring: poll across frames and diff. A healthy
                // device plateaus once the scene settles; a monotonic climb while
                // idle means entries are being queued and never freed.
                //
                // Each field is a snapshot sampled under the queue's own lock, not
                // a barrier: the counts are consistent per queue but not with each
                // other, and a submitting thread may push the instant after the
                // read. stagingBuffers retires on the transfer timeline while the
                // other four retire on per-queue destroy tags, so they are reported
                // apart -- a climb in one does not implicate the other.
                const Rendering::IDevice::ResourcePoolStats pools = device->GetResourcePoolStats();
                result["deferred"] = {
                    {"textures", static_cast<uint64_t>(pools.deferredTextures)},
                    {"textureViews", static_cast<uint64_t>(pools.deferredTextureViews)},
                    {"samplers", static_cast<uint64_t>(pools.deferredSamplers)},
                    {"buffers", static_cast<uint64_t>(pools.deferredBuffers)},
                    {"stagingBuffers", static_cast<uint64_t>(pools.deferredStagingBuffers)}
                };

                // Live (created and not yet destroyed) resource counts, the
                // whole-device view the deferred queues cannot give: a resource
                // that is never retired never enters a deferred queue, so a leak
                // is invisible there and shows only here. Sample after the
                // deferred quarantine has drained, or a pending retire reads as
                // a leak.
                result["live"] = {
                    {"textures", static_cast<uint64_t>(pools.liveTextures)},
                    {"textureViews", static_cast<uint64_t>(pools.liveTextureViews)},
                    {"buffers", static_cast<uint64_t>(pools.liveBuffers)}
                };
            }

            if (auto* culling = rs->GetGPUCullingPipeline())
            {
                const char* publishArm = "none";
                switch (culling->LastPublishArm())
                {
                    case Rendering::GPUCullingPipeline::PublishArm::None:
                        publishArm = "none";
                        break;
                    case Rendering::GPUCullingPipeline::PublishArm::RenderGraph:
                        publishArm = "renderGraph";
                        break;
                }

                // Candidate-set layout only: visibility flags live GPU-side
                // (consumed by the scatter) and are never read back to the CPU.
                json rangesJson = json::array();
                const auto& ranges = culling->GetViewVisibilityRanges();
                for (std::size_t i = 0; i < ranges.size(); ++i)
                {
                    rangesJson.push_back({
                        {"viewId", static_cast<uint32_t>(ranges[i].viewId)},
                        {"cascadeIndex", static_cast<uint32_t>(ranges[i].cascadeIndex)},
                        {"slicePhase", static_cast<uint32_t>(ranges[i].slicePhase)},
                        {"visibilityOffset", ranges[i].visibilityOffset},
                        {"visibilityCount", ranges[i].visibilityCount}
                    });
                }

                // Eventually-consistent HZB occlusion counters (zero until a
                // phase-B dispatch runs — i.e. a view opted into HZB culling).
                const auto occ = culling->ReadOcclusionStats();
                renderServices["gpuCulling"] = {
                    {"lastPublishArm", publishArm},
                    {"rangeCount", static_cast<uint64_t>(ranges.size())},
                    {"ranges", std::move(rangesJson)},
                    {"occlusion", {
                        {"frustumPassed", occ.frustumPassed},
                        {"hzbCulled", occ.hzbCulled},
                        {"recovered", occ.recovered},
                        {"prevVisibleNew", occ.prevVisibleNew}
                    }}
                };
            }

            // Resolved (adapted) auto-exposure per metered view: the linear scene-
            // exposure multiplier and the absolute EV100 it represents. Exposure
            // readback is per-view opt-in (Game View reports its auto camera; Scene View seeds Fixed
            // EV100; lens flare ensures it), so the feature is probed, never created,
            // and only views with a landed metered frame appear.
            if (auto* exposureReadback = rs->GetFeature<Engine::Renderer::ExposureReadbackFeature>())
            {
                json meteredViews = json::array();
                for (const auto& view : rs->Views().GetViews())
                {
                    float scale = 0.0f;
                    uint64_t frameIndex = 0;
                    if (exposureReadback->TryResolveAdaptedExposure(view.id, scale, &frameIndex))
                    {
                        auto sample = Editor::DescribeMeteredExposure(scale, frameIndex);
                        sample["viewId"] = static_cast<uint32_t>(view.id);
                        sample["viewName"] = view.debugName ? view.debugName : "";
                        meteredViews.push_back(std::move(sample));
                    }
                }
                renderServices["exposure"] = {
                    {"meteredViews", std::move(meteredViews)}
                };
            }

            if (auto* csm = rs->GetFeature<Engine::Renderer::ShadowMapRenderFeature>())
            {
                json csmViews = json::array();
                for (const auto& view : rs->Views().GetViews())
                {
                    const auto* fd = csm->GetCachedFrameData(view.id);
                    if (!fd)
                        continue;

                    uint64_t hash = Hashing::kFnv1a64OffsetBasis;
                    auto mixBytes = [&hash](const void* data, std::size_t size)
                    { hash = Hashing::Fnv1a64(data, size, hash); };
                    mixBytes(fd->SplitDistances, sizeof(fd->SplitDistances));
                    mixBytes(fd->OrthoHalfExtent, sizeof(fd->OrthoHalfExtent));
                    mixBytes(fd->DepthSpan, sizeof(fd->DepthSpan));
                    for (uint32_t c = 0; c < fd->NumCascades && c < Engine::Renderer::kMaxShadowCascades; ++c)
                        mixBytes(fd->LightVP[c].Data(), sizeof(float) * 16u);

                    json cascades = json::array();
                    for (uint32_t c = 0; c < fd->NumCascades && c < Engine::Renderer::kMaxShadowCascades; ++c)
                    {
                        const float* m = fd->LightVP[c].Data();
                        cascades.push_back({
                            {"index", c},
                            {"split", fd->SplitDistances[c]},
                            {"orthoHalfExtent", fd->OrthoHalfExtent[c]},
                            {"depthSpan", fd->DepthSpan[c]},
                            {"m00", m[0]},
                            {"m11", m[5]},
                            {"m22", m[10]},
                            {"m30", m[12]},
                            {"m31", m[13]},
                            {"m32", m[14]},
                            // Full matrix: the six scalars above are a partial
                            // view, and for a light whose rotation zeroes a
                            // diagonal element they read as structural zeros —
                            // which makes "the fit did not move" unfalsifiable
                            // from them alone. Shimmer questions need every
                            // element, so emit all 16 in Data() order.
                            {"lightVP", json::array({m[0], m[1], m[2], m[3],
                                                     m[4], m[5], m[6], m[7],
                                                     m[8], m[9], m[10], m[11],
                                                     m[12], m[13], m[14], m[15]})}
                        });
                    }

                    csmViews.push_back({
                        {"viewId", static_cast<uint32_t>(view.id)},
                        {"numCascades", fd->NumCascades},
                        {"hash", hash},
                        {"depthBias", fd->DepthBias},
                        {"normalBias", fd->NormalBias},
                        {"maxShadowDistance", fd->MaxShadowDistance},
                        {"cascades", std::move(cascades)}
                    });
                }
                renderServices["cascadedShadows"] = {
                    {"debugMode", static_cast<int>(csm->GetDebugMode())},
                    {"filterQuality", static_cast<int>(csm->GetFilterQuality())},
                    {"views", std::move(csmViews)}
                };
            }

            // P0 shadow-arc instrumentation: per-slice passed-caster + drawn-
            // triangle counts from the previous frame's scatter (one-frame-stale;
            // valid at steady state). The per-cascade GPU ms lives in the profiler
            // timings (get_gpu_profiler pass "…CascadedShadowMap.View{id}.Cascade{c}")
            // — the harness stitches the two, so it is NOT duplicated here.
            // cascadeIndex 0..3 are directional cascades; 0xFF is the main/color
            // slice; larger values are area/spot/point culling indices.
            if (auto* stream = rs->GetDrawStreamBuilder())
            {
                json slices = json::array();
                for (const auto& s : stream->GetShadowArcStats())
                {
                    slices.push_back({
                        {"viewId", s.viewId},
                        {"cascadeIndex", static_cast<uint32_t>(s.cascadeIndex)},
                        {"phase", static_cast<uint32_t>(s.phase)},
                        {"passedCasters", s.passedCasters},
                        {"drawnTriangles", s.drawnTriangles},
                        // Crossfade tail rows, also included in passedCasters.
                        // Non-zero == a transition was mid-dissolve when this
                        // slice last dispatched; it is the signal the elision
                        // suppression lifts on.
                        {"tailRecords", s.tailRecords},
                        // Dwell-band thrash instrument: instances that changed
                        // LOD this frame. Also SCATTER_STATS-gated, and 0 when
                        // the slice carries no band (set_lod hysteresis == 0).
                        {"lodChanges", s.lodChanges}
                    });
                }
                const auto trip = stream->ReadScatterStats();
                renderServices["shadowArc"] = {
                    {"slices", std::move(slices)},
                    {"tableMisses", trip.tableMisses},
                    {"overflows", trip.overflows},
                    // False when GE_SCATTER_STATS is off (the shipping default):
                    // drawnTriangles is compiled out and reads 0 for every slice,
                    // so a reader must not mistake it for a genuine zero. Tripwires
                    // and passedCasters are unaffected.
                    {"statsEnabled", stream->IntrospectScatter().StatsActive},
                    // M2a fence-gate reject rate: reduceAttempts vs staleRejects.
                    // A staleRejects/reduceAttempts ratio near 1.0 means the
                    // batch-walk early-out almost never has survivor data to act on
                    // (frame pacing keeps the readback in flight) — the M2a slice
                    // has degenerated to a no-op.
                    {"reduceAttempts", stream->GetShadowArcReduceAttempts()},
                    {"staleRejects", stream->GetShadowArcStaleRejects()}
                };
                const auto scatter = stream->IntrospectScatter();
                // Per-view rendered level and phase history. These buffers are
                // created on the device rather than through the render graph, so
                // the graph's resource list cannot show them: this is where a
                // reader sees whether any view is paying for one. Zero views
                // means zero allocation; rotations advance once per view per
                // frame that was actually recorded.
                renderServices["renderedLevelHistory"] = {
                    {"views", scatter.RenderedHistoryViews},
                    {"rotations", scatter.RenderedHistoryRotations}
                };
            }

            json views = json::array();
            for (const auto& view : rs->Views().GetViews())
            {
                const auto entityKeys = rs->GetEntityBatchKeys(view.id);
                const auto forwardCommands = rs->GetForwardCommands(view.id);
                const auto depthCommands =
                    rs->GetDepthCommands(view.id, Engine::Renderer::DepthPassType::Prepass);
                auto* device = rs->GetDevice();
                auto* drawStream = rs->GetDrawStreamBuilder();
                auto* gpuScene = rs->GetGPUScene();
                const auto& meshRegistry = rs->GetMeshGPURegistry();
                const uint64_t gpuInstancesBda =
                    (device && gpuScene && gpuScene->GetInstanceBuffer().IsValid())
                        ? device->GetBufferDeviceAddress(gpuScene->GetInstanceBuffer())
                        : 0ull;

                // Read once per view, never per previewed key: the scatter state
                // is per-builder, and the read must not create the pipeline it
                // reports (IntrospectScatter, not GetOrCreateScatterPipeline).
                const bool drawStreamPipelineReady =
                    drawStream && drawStream->IntrospectScatter().PipelineReady;

                json keyPreview = json::array();
                const std::size_t previewCount = std::min<std::size_t>(entityKeys.size(), 8u);
                // Mirror the color pass exactly: cascade None, phase A, and the
                // same resolved (class, pool group) axes. The published range is
                // keyed on neither raw BatchKey field, so probing with them
                // reports every draw as skipped.
                const std::span<const uint32_t> meshPoolGroups = rs->MeshPoolGroupSpanForDraws();
                for (std::size_t i = 0; i < previewCount; ++i)
                {
                    const auto& key = entityKeys[i];
                    const Engine::Renderer::DrawStreamLookupKey lookup =
                        Engine::Renderer::ResolveDrawStreamLookupKey(
                            key, Engine::Renderer::GPUDrawStreamBuilder::kCascadeIndexNone,
                            Rendering::MaterialDepthClass::MaterialDependent, meshPoolGroups);
                    const auto range = drawStream
                        ? drawStream->FindBatchDrawRange(
                              static_cast<uint32_t>(view.id),
                              Engine::Renderer::GPUDrawStreamBuilder::kCascadeIndexNone,
                              lookup.classKey, lookup.meshKey,
                              Engine::Renderer::GPUDrawStreamBuilder::SlicePhase::A)
                        : Engine::Renderer::GPUDrawStreamBuilder::BatchDrawRange{};
                    const auto* meshEntry = meshRegistry.Find(key.mesh);
                    bool meshDrawable = false;
                    uint32_t indexCount = 0;
                    if (meshEntry)
                    {
                        // The uncounted read: every refusal at the residency
                        // gate's own chokepoint is an EVENT, banked into a total
                        // with no reset, and the gate is accepted on that total
                        // reading zero. A poll previews several keys per view,
                        // so asking there would make the observer the defect.
                        meshDrawable = meshRegistry.IsEntryDrawable(*meshEntry);
                        indexCount = meshEntry->indexCount;
                    }
                    const uint64_t indirectionBda =
                        drawStream ? drawStream->GetSharedIndirectionAddress() : 0ull;

                    // Guard order mirrors the color recorder's: it reports
                    // device-null ahead of draw-range-invalid, and tests both
                    // BDAs only after the range resolved.
                    const char* skipReason = "prebind-ok";
                    if (!key.material)
                        skipReason = "material-null";
                    else if (!meshEntry)
                        skipReason = "mesh-entry-missing";
                    else if (!meshDrawable)
                        skipReason = "mesh-not-drawable";
                    else if (indexCount == 0)
                        skipReason = "index-count-zero";
                    else if (!drawStreamPipelineReady)
                        skipReason = "drawstream-pipeline-invalid";
                    else if (!device)
                        skipReason = "device-null";
                    else if (!range.IsValid())
                        skipReason = "draw-range-invalid";
                    else if (indirectionBda == 0)
                        skipReason = "indirection-bda-zero";
                    else if (gpuInstancesBda == 0)
                        skipReason = "instance-bda-zero";

                    keyPreview.push_back({
                        {"materialIndex", key.materialIndex},
                        {"meshIndex", key.meshIndex},
                        // The axes actually probed: color-class id, and the
                        // pool group when draw consolidation is active.
                        {"drawStreamClassKey", lookup.classKey},
                        {"drawStreamMeshKey", lookup.meshKey},
                        {"hasMaterial", key.material != nullptr},
                        {"meshHandle", static_cast<uint64_t>(key.mesh)},
                        {"skipReason", skipReason},
                        {"meshEntryValid", meshEntry != nullptr},
                        {"meshDrawable", meshDrawable},
                        {"indexCount", indexCount},
                        {"drawStreamPipelineReady", drawStreamPipelineReady},
                        {"rangeValid", range.IsValid()},
                        {"rangeCmdByteOffset", static_cast<uint64_t>(range.even.cmdByteOffset)},
                        {"rangeCountByteOffset", static_cast<uint64_t>(range.even.countByteOffset)},
                        {"rangeMaxDrawCount", range.even.maxDrawCount},
                        {"rangeTailMaxDrawCount", range.evenTail.maxDrawCount},
                        {"indirectionBdaZero", indirectionBda == 0},
                        {"gpuInstancesBdaZero", gpuInstancesBda == 0},
                        {"supportsBufferDeviceAddress",
                         device ? device->GetCapabilities().supportsBufferDeviceAddress : false}
                    });
                }

                views.push_back({
                    {"id", static_cast<uint32_t>(view.id)},
                    {"name", view.debugName ? view.debugName : ""},
                    {"worldId", static_cast<uint64_t>(view.worldId)},
                    {"entityBatchCount", static_cast<uint64_t>(entityKeys.size())},
                    {"forwardCommandCount", static_cast<uint64_t>(forwardCommands.size())},
                    {"cameraPrepassCommandCount", static_cast<uint64_t>(depthCommands.size())},
                    {"keyPreview", std::move(keyPreview)}
                });
            }
            renderServices["views"] = std::move(views);

            json meshDrawDiagnostics = json::array();
            const auto diagSnapshot = rs->GetRecentMeshDrawDiagnostics();
            const std::size_t diagStart =
                diagSnapshot.size() > 40u ? diagSnapshot.size() - 40u : 0u;
            for (std::size_t i = diagStart; i < diagSnapshot.size(); ++i)
            {
                const auto& d = diagSnapshot[i];
                meshDrawDiagnostics.push_back({
                    {"pass", d.pass},
                    {"reason", d.reason},
                    {"viewId", d.viewId},
                    {"materialIndex", d.materialIndex},
                    {"meshIndex", d.meshIndex},
                    {"maxDrawCount", d.maxDrawCount},
                    {"entityKeyCount", static_cast<uint64_t>(d.entityKeyCount)},
                    {"commandCount", static_cast<uint64_t>(d.commandCount)},
                    {"slotCount", static_cast<uint64_t>(d.slotCount)}
                });
            }
            renderServices["meshDrawDiagnostics"] = std::move(meshDrawDiagnostics);
            result["renderServices"] = std::move(renderServices);
        }


        // Per-frame timing from the editor's scene view perf breakdown.
        auto& perf = app.m_LastSceneViewPerf;
        result["timing"] = {
            {"pollMs", perf.pollMs},
            {"inputMs", perf.inputMs},
            {"appUpdateMs", perf.appUpdateMs},
            {"engineUpdateMs", perf.engineUpdateMs},
            {"beginFrameMs", perf.beginFrameMs},
            {"worldViewsMs", perf.worldViewsMs},
            {"thumbnailsMs", perf.thumbnailsMs},
            {"uiRecordMs", perf.uiRecordMs},
            {"terminalPassesMs", perf.terminalPassesMs},
            {"rgCompileMs", perf.rgCompileMs},
            {"rgExecuteMs", perf.rgExecuteMs},
            {"variantCompileMs", perf.variantCompileMs},
            {"prewarmMs", perf.prewarmMs},
            {"presentMs", perf.presentMs}
        };

        // Application's own phase timings (covers the full Render() call,
        // which includes cross-window DnD routing + per-window iteration
        // not captured by the per-window perf sub-phases above). The diff
        // between RenderMs and the render sub-phase sum reveals unmeasured
        // render work; the diff between wallClockDelta and total of all
        // phases reveals frame-loop overhead (profiler/metrics/sleep).
        const auto& phases = app.GetLastFramePhaseTimings();
        result["appPhases"] = {
            {"pollEventsMs", phases.PollEventsMs},
            {"inputMs", phases.InputMs},
            {"appUpdateMs", phases.AppUpdateMs},
            {"engineUpdateMs", phases.EngineUpdateMs},
            {"renderMs", phases.RenderMs},
            {"sleepMs", phases.SleepMs},
            {"renderPreloopMs", app.m_LastRenderPreloopMs}
        };

        // AppUpdate sub-phase breakdown (added with 9af486eb). Lets us
        // attribute the AppUpdateMs column without a CSV capture: preUi
        // (startup+modals+ECS+SceneEditor), debugPanels (metrics+panels),
        // uiWindows (the big one — per-window UIManager::Update loop),
        // tail (post-UI deferred+hotkeys).
        const auto& sub = app.m_LastAppUpdateSubPhases;
        result["appUpdateSubPhases"] = {
            {"preUiMs", sub.preUiMs},
            {"debugPanelsMs", sub.debugPanelsMs},
            {"uiWindowsMs", sub.uiWindowsMs},
            {"tailMs", sub.tailMs}
        };

        double totalMs = perf.pollMs + perf.inputMs + perf.appUpdateMs
                       + perf.engineUpdateMs + perf.beginFrameMs
                       + perf.worldViewsMs + perf.thumbnailsMs + perf.uiRecordMs
                       + perf.terminalPassesMs + perf.rgCompileMs + perf.rgExecuteMs
                       + perf.presentMs;
        result["totalFrameMs"] = totalMs;
        result["estimatedFps"] = totalMs > 0.0 ? 1000.0 / totalMs : 0.0;

        // Slice 2 subtree-skip fast-path counters + UI phase breakdown.
        // Aggregated across ALL windows — the editor has >1 UIManager
        // (scene + floating panels) and uiWindowsMs is the sum, so
        // per-window-[0] data under-reports the dominant stage.
        if (!app.m_Windows.empty())
        {
            uint32_t fastPathHits = 0, slowPathWalks = 0;
            uint32_t mismatches = 0, cumMismatches = 0;
            uint32_t cascadeCalls = 0, cascadeShared = 0, cascadeCallsBuildYoga = 0, cascadeCallsConverge = 0;
            double uiTotalMs = 0.0, schedulerMs = 0.0, buildYogaMs = 0.0, yogaMs = 0.0;
            double postLayoutMs = 0.0, hitTestMs = 0.0, eventDispatchMs = 0.0;
            double geometryMs = 0.0, cleanupMs = 0.0;
            double preSolveDrainMs = 0.0, indicesAndClipsMs = 0.0;
            double solveAndApplyMs = 0.0, focusOrderMs = 0.0;
            uint32_t elementCount = 0;
            bool enabled = false, validatorEnabled = false;
            std::vector<UIManager::UpdateProfileFrame::DirtySourceEntry> dirtySources;
            size_t windowsWithProf = 0;
            for (const auto& wCtx : app.m_Windows)
            {
                if (!wCtx || !wCtx->ui) continue;
                auto* ui = wCtx->ui.get();
                enabled = enabled || ui->IsSubtreeSkipEnabled();
                // Stage 7 step 7 option 2: validator path deleted along with
                // m_Nodes; report disabled / zero mismatches for compatibility.
                (void)validatorEnabled;
                (void)cumMismatches;
                UIManager::UpdateProfileFrame prof{};
                if (!ui->GetLastUpdateProfileFrame(prof)) continue;
                ++windowsWithProf;
                fastPathHits           += prof.SubtreeFastPathHits;
                slowPathWalks          += prof.SubtreeSlowPathWalks;
                cascadeCalls           += prof.CascadeCalls;
                cascadeShared          += prof.CascadeShared;
                cascadeCallsBuildYoga  += prof.CascadeCallsBuildYoga;
                cascadeCallsConverge   += prof.CascadeCallsConverge;
                uiTotalMs              += prof.TotalMs;
                schedulerMs            += prof.SchedulerMs;
                buildYogaMs            += prof.BuildYogaMs;
                yogaMs                 += prof.YogaMs;
                postLayoutMs           += prof.PostLayoutMs;
                hitTestMs              += prof.HitTestMs;
                eventDispatchMs        += prof.EventDispatchMs;
                geometryMs             += prof.GeometryMs;
                cleanupMs              += prof.CleanupMs;
                preSolveDrainMs        += prof.PreSolveDrainMs;
                indicesAndClipsMs      += prof.IndicesAndClipsMs;
                solveAndApplyMs        += prof.SolveAndApplyMs;
                focusOrderMs           += prof.FocusOrderMs;
                elementCount           += prof.ElementCount;
                for (auto& s : prof.DirtySources)
                    dirtySources.push_back(s);
            }
            if (windowsWithProf > 0)
            {
                result["uiSubtreeSkip"] = {
                    {"fastPathHits", fastPathHits},
                    {"slowPathWalks", slowPathWalks},
                    {"validatorMismatches", mismatches},
                    {"cumulativeMismatches", cumMismatches},
                    {"enabled", enabled},
                    {"validatorEnabled", validatorEnabled},
                    {"cascadeCalls", cascadeCalls},
                    {"cascadeShared", cascadeShared},
                    {"cascadeCallsBuildYoga", cascadeCallsBuildYoga},
                    {"cascadeCallsConverge", cascadeCallsConverge},
                    {"windowsWithProf", (uint32_t)windowsWithProf}
                };
                result["uiPhaseMs"] = {
                    {"totalMs", uiTotalMs},
                    {"schedulerMs", schedulerMs},
                    {"buildYogaMs", buildYogaMs},
                    {"yogaMs", yogaMs},
                    {"postLayoutMs", postLayoutMs},
                    {"hitTestMs", hitTestMs},
                    {"eventDispatchMs", eventDispatchMs},
                    {"geometryMs", geometryMs},
                    {"cleanupMs", cleanupMs},
                    {"preSolveDrainMs", preSolveDrainMs},
                    {"indicesAndClipsMs", indicesAndClipsMs},
                    {"solveAndApplyMs", solveAndApplyMs},
                    {"focusOrderMs", focusOrderMs},
                    {"elementCount", elementCount}
                };

                // Peak-over-history frame selected by totalMs across all
                // windows' ring buffers. Lets a polling client observe the
                // worst recent frame even if the last-frame snapshot above
                // is already post-spike.
                const UIManager::UpdateProfileFrame* bestPeak = nullptr;
                const UIManager* bestPeakUi = nullptr;
                for (const auto& wCtx2 : app.m_Windows)
                {
                    if (!wCtx2 || !wCtx2->ui) continue;
                    const auto* ui2 = wCtx2->ui.get();
                    const auto& hist = ui2->GetUpdateProfilingHistory();
                    for (const auto& f : hist)
                    {
                        if (!bestPeak || f.TotalMs > bestPeak->TotalMs)
                        {
                            bestPeak = &f;
                            bestPeakUi = ui2;
                        }
                    }
                }
                if (bestPeak)
                {
                    (void)bestPeakUi;
                    result["uiPeakFrame"] = {
                        {"historySize", (uint32_t)(bestPeakUi ? bestPeakUi->GetUpdateProfilingHistory().size() : 0u)},
                        {"frameIndex", bestPeak->FrameIndex},
                        {"elementCount", bestPeak->ElementCount},
                        {"totalMs", bestPeak->TotalMs},
                        {"schedulerMs", bestPeak->SchedulerMs},
                        {"buildYogaMs", bestPeak->BuildYogaMs},
                        {"cascadeComputeMs", bestPeak->CascadeComputeMs},
                        {"yogaApplyMs", bestPeak->YogaApplyMs},
                        {"yogaMs", bestPeak->YogaMs},
                        {"postLayoutMs", bestPeak->PostLayoutMs},
                        {"hitTestMs", bestPeak->HitTestMs},
                        {"eventDispatchMs", bestPeak->EventDispatchMs},
                        {"geometryMs", bestPeak->GeometryMs},
                        {"cleanupMs", bestPeak->CleanupMs},
                        {"transitionMs", bestPeak->TransitionMs},
                        {"preSolveDrainMs", bestPeak->PreSolveDrainMs},
                        {"indicesAndClipsMs", bestPeak->IndicesAndClipsMs},
                        {"solveAndApplyMs", bestPeak->SolveAndApplyMs},
                        {"focusOrderMs", bestPeak->FocusOrderMs},
                        {"subtreeFastPathHits", bestPeak->SubtreeFastPathHits},
                        {"subtreeSlowPathWalks", bestPeak->SubtreeSlowPathWalks},
                        {"cascadeCalls", bestPeak->CascadeCalls},
                        {"cascadeShared", bestPeak->CascadeShared},
                        {"cascadeCallsBuildYoga", bestPeak->CascadeCallsBuildYoga},
                        {"cascadeCallsConverge", bestPeak->CascadeCallsConverge},
                        {"lateRelayoutCount", bestPeak->LateRelayoutCount},
                        {"lateRelayoutReasons", bestPeak->LateRelayoutReasons},
                        {"layoutOverridePatchCount", bestPeak->LayoutOverridePatchCount}
                    };
                }
                if (!dirtySources.empty())
                {
                    // Sort aggregated list by count descending and keep top 10.
                    std::sort(dirtySources.begin(), dirtySources.end(),
                        [](const auto& a, const auto& b) { return a.Count > b.Count; });
                    const size_t keep = std::min<size_t>(dirtySources.size(), 10);
                    auto arr = nlohmann::json::array();
                    for (size_t i = 0; i < keep; ++i)
                    {
                        const auto& src = dirtySources[i];
                        arr.push_back({
                            {"instanceId", src.InstanceId},
                            {"id", src.Id},
                            {"class", src.ClassName},
                            {"count", src.Count},
                            {"flagsSeen", src.FlagsSeen}
                        });
                    }
                    result["uiDirtySources"] = std::move(arr);
                }
            }
        }

        // Wall-clock FPS from the Application's actual delta time (matches the
        // scene view FPS label). This includes all overhead not covered by
        // the timed phases above (OS scheduling, window messages, etc.).
        const double dt = app.GetDeltaTime();
        const double wallClockDeltaMs = dt * 1000.0;
        result["wallClockDeltaMs"] = wallClockDeltaMs;
        result["wallClockFps"] = dt > 0.0 ? 1.0 / dt : 0.0;

        // Pipeline variant counts. Reveals how many unique pipelines the scene
        // forces — each distinct cache entry maps to its own vkCmdBindPipeline
        // per pass per cascade.
        if (auto* rs = EngineCore::GetInstance().GetRenderServices())
        {
            result["variantCacheCounts"] = {
                {"colorInstanced", static_cast<uint64_t>(rs->Materials().Variants().InstancedCount())},
                {"depthInstanced", static_cast<uint64_t>(rs->Materials().Variants().DepthInstancedCount())},
                {"sharedDepth", static_cast<uint64_t>(rs->Materials().Variants().SharedDepthCount())},
                // Monotonic count of variant compiles the async publish-gate ran.
                // Unlike the three cache sizes, it moves on an in-place republish
                // (hot-reload replaces an entry without changing the map size), so
                // it is the signal that a shader edit actually landed a new pipeline.
                {"asyncVariantCompiles", rs->Materials().Variants().AsyncVariantCompileCount()}
            };
        }

        // GPU sync timing breakdown from the Vulkan device. Shows where the
        // CPU stalls waiting for the GPU (fence waits, swapchain acquire, present).
        if (!app.m_Windows.empty() && app.m_Windows[0]->renderCtx)
        {
            if (auto* device = app.m_Windows[0]->renderCtx->GetDevice())
            {
                Rendering::IDevice::FrameSyncTimings sync{};
                if (device->GetLastFrameSyncTimings(sync))
                {
                    // A frame whose BeginFrame spends most of its wall time parked on the
                    // swapchain fence is paced by the compositor, not by the engine: an idle
                    // Windows desktop drops DWM to ~4 Hz and every windowed FIFO swapchain
                    // follows it, so wallClockFps then measures DWM and says nothing about
                    // render cost. Derive the verdict here, once, rather than leaving every
                    // caller to re-derive it from the raw ratio (and most never do).
                    const bool measurable = wallClockDeltaMs > 0.0;
                    const bool presentPaced = measurable &&
                                              sync.beginFrameWaitMs >= 0.5 * wallClockDeltaMs;
                    result["gpuSync"] = {
                        {"beginFrameWaitMs", sync.beginFrameWaitMs},
                        {"waitedGraphicsFence", sync.waitedGraphicsFence},
                        {"waitedComputeFence", sync.waitedComputeFence},
                        {"waitedTransferFence", sync.waitedTransferFence},
                        {"acquireMs", sync.acquireMs},
                        {"acquireTimedOut", sync.acquireTimedOut},
                        {"presentTransitionSubmitMs", sync.presentTransitionSubmitMs},
                        {"presentMs", sync.presentMs},
                        {"frameGpuPeriodMs", sync.frameGpuPeriodMs},
                        {"presentPaced", presentPaced}
                    };
                    // No wall-clock delta yet (first frame) means the ratio has no
                    // denominator — unknown, which is not the same as ok.
                    result["measurementValidity"] = !measurable ? "unknown — no wall-clock delta"
                                                    : presentPaced ? "UNMEASURED — display-paced"
                                                                   : "ok";
                }
            }
        }

        return result; });

server.RegisterHandler("get_ui_clip_stats", [&app](const EditorDebugServer::RequestContext&) -> json
                           {
        auto windowsArr = nlohmann::json::array();
        for (const auto& wCtx : app.m_Windows)
        {
            if (!wCtx || !wCtx->ui) continue;
            windowsArr.push_back({
                {"clipSlotsUsed", wCtx->ui->GetClipSlotsUsed()},
                {"clipSlotsTotal", wCtx->ui->GetClipSlotsTotal()},
            });
        }
        return json{{"windows", windowsArr}}; });

server.RegisterHandler("get_ui_profile_history", [&app](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        // Optional {"maxFrames": N} param to cap response size.
        const int64_t req = ctx.params.value("maxFrames", (int64_t)120);
        size_t maxFrames = (req > 0 && req <= 120) ? (size_t)req : 120;
        nlohmann::json result;
        auto windowsArr = nlohmann::json::array();
        for (const auto& wCtx : app.m_Windows)
        {
            if (!wCtx || !wCtx->ui) continue;
            const auto* ui = wCtx->ui.get();
            const auto& hist = ui->GetUpdateProfilingHistory();
            if (hist.empty()) { continue; }
            auto framesArr = nlohmann::json::array();
            const size_t start = hist.size() > maxFrames ? hist.size() - maxFrames : 0u;
            for (size_t i = start; i < hist.size(); ++i)
            {
                const auto& f = hist[i];
                framesArr.push_back({
                    {"frameIndex", f.FrameIndex},
                    {"elementCount", f.ElementCount},
                    {"totalMs", f.TotalMs},
                    {"idleFrame", f.IdleFrame},
                    {"idleDecline", f.IdleDecline},
                    {"schedulerMs", f.SchedulerMs},
                    {"buildYogaMs", f.BuildYogaMs},
                    {"cascadeComputeMs", f.CascadeComputeMs},
                    {"yogaApplyMs", f.YogaApplyMs},
                    {"yogaMs", f.YogaMs},
                    {"postLayoutMs", f.PostLayoutMs},
                    {"hitTestMs", f.HitTestMs},
                    {"eventDispatchMs", f.EventDispatchMs},
                    {"geometryMs", f.GeometryMs},
                    {"cleanupMs", f.CleanupMs},
                    {"transitionMs", f.TransitionMs},
                    {"preSolveDrainMs", f.PreSolveDrainMs},
                    {"indicesAndClipsMs", f.IndicesAndClipsMs},
                    {"solveAndApplyMs", f.SolveAndApplyMs},
                    {"focusOrderMs", f.FocusOrderMs},
                    {"subtreeFastPathHits", f.SubtreeFastPathHits},
                    {"subtreeSlowPathWalks", f.SubtreeSlowPathWalks},
                    {"cascadeCalls", f.CascadeCalls},
                    {"cascadeShared", f.CascadeShared},
                    {"cascadeCallsBuildYoga", f.CascadeCallsBuildYoga},
                    {"cascadeCallsConverge", f.CascadeCallsConverge},
                    {"lateRelayoutCount", f.LateRelayoutCount},
                    {"lateRelayoutReasons", f.LateRelayoutReasons},
                    {"layoutOverridePatchCount", f.LayoutOverridePatchCount},
                    {"onPostLayoutDispatchMs", f.OnPostLayoutDispatchMs},
                    {"postLayoutRetryMs", f.PostLayoutRetryMs},
                    {"postLayoutRetryCount", f.PostLayoutRetryCount},
                    {"genAllPrimitivesMs", f.GenAllPrimitivesMs},
                    {"genAllPrimitivesSkipped", f.GenAllPrimitivesSkipped},
                    {"genAllPrimitivesRegenCause", f.GenAllPrimitivesRegenCause},
                    {"drainItems", f.DrainItems},
                    {"drainParallelTasks", f.DrainParallelTasks},
                    {"drainEscalated", f.DrainEscalated},
                    {"lateRelayoutBuildYogaMs", f.LateRelayoutBuildYogaMs},
                    {"lateRelayoutYogaMs", f.LateRelayoutYogaMs},
                    {"subtreeSlowPathReasonUnion", f.SubtreeSlowPathReasonUnion},
                    {"subtreeSlowPathReasonCounts", nlohmann::json::array({
                        f.SubtreeSlowPathReasonCounts[0], f.SubtreeSlowPathReasonCounts[1],
                        f.SubtreeSlowPathReasonCounts[2], f.SubtreeSlowPathReasonCounts[3],
                        f.SubtreeSlowPathReasonCounts[4], f.SubtreeSlowPathReasonCounts[5],
                        f.SubtreeSlowPathReasonCounts[6], f.SubtreeSlowPathReasonCounts[7],
                        f.SubtreeSlowPathReasonCounts[8], f.SubtreeSlowPathReasonCounts[9],
                        f.SubtreeSlowPathReasonCounts[10], f.SubtreeSlowPathReasonCounts[11]
                    })}
                });
                // Attach top dirtySources for this frame (only populated when
                // GE_UI_DIRTY_TRACE=1). Lets spike analysis locate the offending
                // elements without a separate call.
                if (!f.DirtySources.empty())
                {
                    auto& frameObj = framesArr.back();
                    auto srcArr = nlohmann::json::array();
                    auto sorted = f.DirtySources;
                    std::sort(sorted.begin(), sorted.end(),
                        [](const auto& a, const auto& b) { return a.Count > b.Count; });
                    const size_t keep = std::min<size_t>(sorted.size(), 10);
                    for (size_t j = 0; j < keep; ++j)
                    {
                        const auto& src = sorted[j];
                        srcArr.push_back({
                            {"instanceId", src.InstanceId},
                            {"id", src.Id},
                            {"class", src.ClassName},
                            {"count", src.Count},
                            {"flagsSeen", src.FlagsSeen}
                        });
                    }
                    frameObj["dirtySources"] = std::move(srcArr);
                }
            }
            windowsArr.push_back({{"frames", std::move(framesArr)}});
        }
        result["windows"] = std::move(windowsArr);
        return result; });
}
}
