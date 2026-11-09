#include "CBTTerrainECS/CBTRenderNode.h"

#include "CBTTerrainECS/CBTRenderFeature.h"

#include "CBTTerrain/CBTInstance.h"
#include "CBTTerrain/CBTLayout.h"
#include "CBTTerrain/CBTResources.h"
#include "CBTTerrain/CBTUpdateNode.h"

#include "TerrainECS/TerrainRenderFeature.h"

#include "Engine/Rendering/DrawCommand.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/Pipeline/ReflectionsProvider.h"

#include "Rendering/CameraTypes.h" // ViewPurpose
#include "Rendering/Core/Device.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"

#include "Logger/Logger.h"
#include "Platform/Environment.h"

namespace GameEngine::CBTTerrainECS
{

using namespace GameEngine::Rendering;
using namespace GameEngine::Engine::Renderer;
namespace RG = ::GameEngine::Rendering::RenderGraph;

namespace
{
void EmitCbtForwardDraw(Pipeline::ViewDeclare& d, CBTRenderFeature& feature)
{
    auto& rs = d.Services;
    const void* graph = &d.Frame;
    const uint64_t rgFrame = d.Frame.FrameIndex();

    static const bool kCbtViewDebug = Platform::EnvironmentSwitchEnabled("GE_CBT_DESC_DEBUG", false);
    if (kCbtViewDebug)
    {
        Logger::Log::Info("CBT.View id={} purpose={} rgFrame={} graph={} instance=0",
                          static_cast<uint32_t>(d.View.id), static_cast<int>(d.View.purpose),
                          rgFrame, graph);
    }

    // Bisect toggle (GE_CBT_NO_DRAW=1): keep the CBT.Update compute running but skip
    // the forward DRAW. Isolates whether the entity-deletion device-lost needs CBT's
    // draw (its forward pass / descriptor-buffer usage) or just CBT's compute update
    // (GPU time / barriers). The device-lost is engine-side in frame N+1's consumption
    // of frame N's scatter/culling state (RenderDoc proved frame N healthy); this
    // narrows CBT's trigger for the engine-side fix.
    static const bool kCbtNoDraw = Platform::EnvironmentSwitchEnabled("GE_CBT_NO_DRAW", false);
    if (kCbtNoDraw)
        return;

    // Re-import the update outputs for THIS view's forward reads (idempotent per
    // frame -> the same RGBuffers -> the compute->graphics edge forms).
    const CBTTerrain::CBTUpdateOutputs outputs =
        CBTTerrain::CBTUpdateNode::ImportOutputs(d.Frame, feature.GetInstance());
    if (!outputs.IndirectDraw.IsValid())
        return;

    MaterialKeyword passKeywords = MaterialKeyword::None;
    if (auto kw = rs.GetWorldPassKeywords(d.View.id))
        passKeywords = *kw;
    // The world pass owns the reflection MRT slice, and WorldRenderNode folds
    // ReflectionsProvider's keyword into the variants it draws with. Terrain
    // renders INTO that same pass, so it must key the same way: with SSSR
    // active the pass carries a second colour target, and a variant compiled
    // without the keyword declares no output for it. Vulkan ignores the
    // surplus target; WebGPU rejects the pipeline outright and the invalid
    // pipeline sinks the frame's command buffer — SSR blacked the browser
    // canvas while desktop merely wrote nothing to the slice.
    //
    // Ask the provider, not the frame: CBT declares before WorldRenderNode, so
    // both the recorded world-pass keywords and the published *Written texture
    // names are still empty at this point every frame.
    passKeywords |= Engine::Renderer::Pipeline::ReflectionsProvider::WorldPassKeyword(d);

    DrawCommand cmd{};
    if (!feature.BuildDrawCommand(rs, d.View.id, passKeywords, cmd))
        return;

    // The terrain's depth goes into the camera prepass, so GTAO, the screen-space shadows, the
    // light-cull depth bounds and every other reader of the prepass depth see the ground, and the
    // world pass can attach that depth read-only. The head is the colour draw drawn again by the
    // material's depth-only variant: the same VISIBLE record through the same vertex modifier, so
    // the two rasterize the same depth. Until the device has built that variant's pipeline the
    // colour draw writes its own.
    DrawCommand head{};
    const bool hasHead = feature.BuildPrepassHead(rs, d.View.id, cmd, head);

    // Emit into the MAIN world pass as a forward contributor (the terrain/ocean-
    // surface path), NOT a separate forward pass. The separate pass rebuilt a full
    // per-pass descriptor set (set 0: Cam + clustered lights + shadows + IBL) every
    // frame on top of the world pass's, and — being an always-on extra graphics
    // pass — was the reliable straddle trigger behind the entity-deletion device-
    // lost. As a contributor CBT reuses the world pass's cached set 0 and adds only
    // its per-draw set 2.
    rs.EmitForwardCommand(d.View.id, cmd, hasHead ? ForwardDrawDepth::Prepass : ForwardDrawDepth::ColourPass,
                          hasHead ? &head : nullptr);
    // Order update storage writes before vertex/index/height-chain reads and indirect-argument reads.
    // Camera draws use visible indices; shadows use all indices. Both streams remain graph outputs.
    // Static identity/count buffers need no per-frame edge; draw and prepass setup declare the dynamic reads.
    const std::pair<RG::RGBuffer, RG::RGBufferRead> drawReads[] = {
        {outputs.IndirectDraw, RG::RGBufferRead::Indirect},
        {outputs.IndicesVisible, RG::RGBufferRead::Storage},
        {outputs.CurrentVertex, RG::RGBufferRead::Storage},
        {outputs.HeightChain, RG::RGBufferRead::Storage},
    };
    const ForwardBufferReaders readers =
        hasHead ? ForwardBufferReaders::WorldPassAndPrepass : ForwardBufferReaders::WorldPass;
    for (const auto& [buffer, access] : drawReads)
        rs.EmitForwardSampledBufferRead(d.Frame, d.View.id, buffer, access, readers);
    // The surface samples the far-field normal chain the update writes (fragment stage, world pass).
    if (outputs.NormalChain.IsValid())
        rs.EmitForwardSampledRead(d.Frame, d.View.id, outputs.NormalChain);
}

// The terrain's shadow on the directional light (TerrainShadowBake): declares this frame's bake
// when the sun or the heights changed and publishes the clearance map for this view. Planar
// terrains only; a spherical domain casts no map.
void DeclareTerrainShadow(Pipeline::ViewDeclare& d, CBTRenderFeature& feature,
                          const CBTTerrain::CBTFrameParams& params)
{
    if (feature.GetDomainConfig().DomainMode != CBTTerrain::kDomainPlanar)
        return;
    auto& rs = d.Services;
    TerrainShadowBakeInputs inputs;
    inputs.Source = feature.GetTerrainShadowSource();
    inputs.TerrainX = params.TerrainOrigin[0];
    inputs.TerrainZ = params.TerrainOrigin[1];
    inputs.BaseY = params.TerrainSize[3];
    inputs.SizeX = params.TerrainSize[0];
    inputs.SizeZ = params.TerrainSize[1];
    inputs.HeightScale = params.TerrainSize[2];
    inputs.DrawLevelScale = params.HeightChain[0];
    const CBTTerrain::CBTClassifyDesc& classify = feature.GetClassify();
    inputs.DirtyMinU = classify.DirtyMinU;
    inputs.DirtyMinV = classify.DirtyMinV;
    inputs.DirtyMaxU = classify.DirtyMaxU;
    inputs.DirtyMaxV = classify.DirtyMaxV;
    if (auto* terrainFeature = rs.GetFeature<TerrainECS::TerrainRenderFeature>())
        inputs.HeightUploadPass = terrainFeature->GetHeightmapUploadPassRG(d.Frame.FrameIndex(), &d.Frame);
    feature.GetTerrainShadowBake().DeclareForView(rs, d.Frame, d.View.id, d.View.worldId, inputs);
}
} // namespace

bool CBTRenderNode::Initialize(std::string /*nodeId*/, std::string /*nodeJson*/,
                               std::string* /*outError*/)
{
    return true;
}

void CBTRenderNode::DeclareForView(Pipeline::ViewDeclare& d)
{
    auto& rs = d.Services;
    auto* device = rs.GetDevice();
    if (!device)
        return;

    // Game / EditorScene claim CBT.Update and draw. EditorPreview may draw only as
    // a follower of an update already recorded in THIS graph this frame (bookmark
    // preview shares the window graph with EditorScene). It must not ClaimUpdate or
    // BuildFrameParams: those would overwrite the scene camera's classify UBO, and
    // a solo EditorPreview graph (thumbnails / asset preview) is the device-lost
    // that closed this gate — draw without a producer, null set-2 vertex fetch.
    // UtilityCapture stays skipped. A bookmark looking away from the scene camera
    // still rides the scene's VISIBLE stream (same tessellation, different set-0
    // camera); that is the cost of not running a second classify.
    if (d.View.purpose != Rendering::ViewPurpose::Game &&
        d.View.purpose != Rendering::ViewPurpose::EditorScene &&
        d.View.purpose != Rendering::ViewPurpose::EditorPreview)
        return;

    // Quiescent by default: EnsureFeature only constructs the (empty) feature; the
    // ~14 MiB GPU bring-up in EnsureInitialized is deferred until a live Terrain
    // entity marks the feature active (CBTUpdateSystem, Extraction phase — runs
    // before this render-graph declaration), so an editor with no CBT terrain pays
    // nothing.
    auto& feature = rs.EnsureFeature<CBTRenderFeature>();
    if (!feature.IsActive())
        return;
    if (!feature.EnsureInitialized(*device, rs))
        return;

    if (d.View.purpose == Rendering::ViewPurpose::EditorPreview)
    {
        if (!feature.UpdateDeclaredInGraph(&d.Frame, d.Frame.FrameIndex()))
            return;
    }

    // In-place re-provision safety: a SamplesPerMeter/size edit retires and recreates the terrain's
    // unified heightmap, which CBT samples through its own descriptor ring (a path the bindless-slot
    // invalidation does not cover). Poll the terrain feature's retire generation and, on a bump,
    // drop the retired views from every ring element this frame — before the quarantine frees the
    // image. No-op when nothing retired since last frame.
    if (auto* terrainFeature = rs.GetFeature<TerrainECS::TerrainRenderFeature>())
    {
        if (!feature.ConsumeTerrainRetire(terrainFeature->GetTerrainTextureRetireGeneration()))
            return;
    }

    // C7 planet observability (GE_CBT_PLANET_DEBUG): the falsifiable "planet is active"
    // signal for a runtime verify. The seed/re-seed already logs GE_CBT.Planet with the
    // domain + root count; this re-affirms it periodically (stall-free — no GPU readback
    // in the render path; live-bisector counts are covered by CBTTerrainTests). Idle
    // runs (env unset) pay nothing.
    static const bool kCBTPlanetDebug = Platform::EnvironmentSwitchEnabled("GE_CBT_PLANET_DEBUG", false);
    if (kCBTPlanetDebug)
    {
        static uint32_t sPlanetLogThrottle = 0u;
        if ((sPlanetLogThrottle++ % 120u) == 0u)
        {
            const CBTTerrain::CBTInstance& inst = feature.GetInstance();
            Logger::Log::Info("GE_CBT.Planet domain={} roots={} baseDepth={} maxDepth={}",
                              inst.GetDomainMode() == CBTTerrain::kDomainSpherical ? "spherical"
                                                                                   : "planar",
                              inst.GetRootCount(), inst.GetBaseDepth(),
                              feature.GetClassify().TargetDepth);
        }
    }

    // The one place a device frame enters CBT: everything below (the height/atlas/coarse
    // ring binds, the params + surface + sculpt slot writes, and the push constant the GPU
    // reduces) derives its ring element from this counter, so CPU and GPU cannot disagree.
    // The device's own index is a CHANGE TOKEN, not a slot — reducing its wrapped value by
    // the ring depth strands element kCBTFrameParamsRing-1 forever, which is the whole
    // write-after-fence margin (see CBTFrameRingSlot). Per view, so idempotent within a frame.
    const uint32_t frameCounter = feature.GetInstance().AdvanceFrameCounter(device->GetFrameIndex());

    // Restart the tree from its roots when the domain changed, a terrain was retired above, or
    // the depth cap was lowered: at once, or, for repeats within CBTTreeSeed's window, once they
    // settle. Waits only its graphics submissions; no-op when nothing changed.
    feature.EnsureSeeded(frameCounter);

    const void* graph = &d.Frame;
    const uint64_t rgFrame = d.Frame.FrameIndex();
    const bool follower = feature.UpdateDeclaredInGraph(graph, rgFrame);
    if (!follower && !feature.ClaimUpdate(graph, rgFrame))
        return;

    // C4: gather this view's camera + the active terrain's heightmap/world-size (the
    // SAME data CDLOD renders) and bind the height source into this frame's ring slot.
    // Returns false when there is no active terrain -> nothing to render this frame.
    CBTTerrain::CBTFrameParams params{};
    if (!feature.BuildFrameParams(rs, d.View.id, d.RenderWidth, d.RenderHeight, frameCounter, params, d.Frame))
        return;

    if (d.View.purpose == Rendering::ViewPurpose::EditorPreview)
    {
        EmitCbtForwardDraw(d, feature);
        return;
    }

    // Followers draw the owner's buffers and keep its immutable frame bindings.
    if (follower)
    {
        // Already declared for THIS graph this frame (an earlier view) -> draw.
        DeclareTerrainShadow(d, feature, params);
    }
    else
    {
        // Cross-graph ordering tripwire (multi-graph upload/update hazard, PR #506 ledger).
        // CBT.Update samples the heightmap this graph, but the once-per-device-frame upload
        // FLUSH (TerrainUpload) may be claimed by a different graph. An ordering edge reaches
        // only inside one graph — a pass id means nothing in another — so across graphs the
        // flush stays edge-less, with no GPU sync. If that other graph submits after this one,
        // VertexEval reads pre-copy texels while CBTUpdateSystem commits the dirty rect anyway
        // -> a one-shot streamed upload freezes stale. This never fires today (the terrain-owning
        // RenderServices builds one graph per frame, co-locating upload + update, ordered by the
        // edge added below); it is a loud tripwire for a future change that splits them.
        // Warn-once, non-fatal: surfacing the assumption beats a silent stale frame.
        if (auto* terrainFeature = rs.GetFeature<TerrainECS::TerrainRenderFeature>())
        {
            if (terrainFeature->HeightmapUploadClaimedInOtherGraph(rgFrame, graph))
            {
                static bool sWarned = false;
                if (!sWarned)
                {
                    sWarned = true;
                    Logger::Log::Error(
                        "CBT.Update claimed in a different render graph than the terrain heightmap "
                        "upload flush this frame; the edge-less upload has no cross-graph ordering, "
                        "so streamed height edits can freeze stale. The single-graph-per-frame "
                        "assumption is broken — co-locate the upload flush with CBT.Update or add a "
                        "cross-graph read edge.");
                }
            }
        }

        // At rest (CBTRenderFeature::IsUpdateAtRest): the inputs match the last update's and the
        // updates since they changed split and merged nothing, so the buffers the last update
        // wrote hold this frame's tree. Declare nothing and draw from them. The frame counter and
        // this frame's params ring slot advanced above, which is what the draw reads.
        feature.ConsumeUpdateActivity();
        DeclareTerrainShadow(d, feature, params);
        if (feature.IsUpdateAtRest(params))
        {
            EmitCbtForwardDraw(d, feature);
            return;
        }

        // Page bounds must cover the selected publication, including abandoned updates.
        // Missing coverage and explicit full requests conservatively evaluate every vertex.
        CBTTerrain::CBTClassifyDesc classify = feature.GetClassify();
        const uint64_t fullEvaluationTicket = feature.PendingForceVertexEval();
        bool fullEvaluation = fullEvaluationTicket != 0u;
        if (!fullEvaluation && feature.HasUnconsumedHeightPages())
            fullEvaluation = !feature.ApplyHeightPageDirtyRegion(classify);
        classify.GateVertexEval = fullEvaluation ? 0u : 1u;
        classify.Settle = feature.InputsHeld() ? 1u : 0u;
        // Forward the bound heightmap so the update pass declares its read edge on
        // the texture VertexEval samples (plan §8 C5 — live-edit region uploads).
        const CBTTerrain::CBTActivitySlot activitySlot =
            feature.BeginUpdateActivityReadback(d.Frame, classify.GateVertexEval == 0u);
        const CBTTerrain::CBTUpdateOutputs updateOut = CBTTerrain::CBTUpdateNode::DeclareUpdatePass(
            d.Frame, feature.GetInstance(), classify, params, frameCounter,
            feature.GetBoundHeightmap(), activitySlot, feature.BuildUpdateSubmittedCallback(fullEvaluationTicket));

        // Paged cache hazards order old readers before its overwrite and new readers after it.
        // Other height sources still require an explicit edge from their bindless upload.
        constexpr float kPagedHeightSourceMode = 2.0f; // CBTFrameParams::AtlasParams1.z
        if (auto* terrainFeature = rs.GetFeature<TerrainECS::TerrainRenderFeature>();
            terrainFeature != nullptr && params.AtlasParams1[2] != kPagedHeightSourceMode)
        {
            const Rendering::RenderGraph::RGPassId uploadPass =
                terrainFeature->GetHeightmapUploadPassRG(rgFrame, graph);
            if (uploadPass != Rendering::RenderGraph::kInvalidId && updateOut.Update.IsValid())
                d.Frame.AddOrderingEdge(Rendering::RenderGraph::RGPass{uploadPass},
                                        updateOut.Update);
        }

        // GE_CBT_VALIDATE: log the Validate kernel's error counters (CBTValidationReadback).
        static const bool kCbtValidateDbg = Platform::EnvironmentSwitchEnabled("GE_CBT_VALIDATE", false);
        if (kCbtValidateDbg)
            feature.GetValidationReadback().Tick(d.Frame, device, feature.GetInstance());

        // Falsifiable edit trace (GE_CBT_EDIT_DEBUG): log ONCE per edit frame, at the
        // point the update is recorded WITH a non-empty dirty rect. The C1 mirage
        // (ConsumeDirtyRegion always early-returning) produced an empty rect, so this
        // line would never fire on an edit; a real edit that reaches the GPU update
        // fires it. Idle frames (empty rect) stay silent. Rect only — a per-frame
        // MODIFIED-count readback would stall the frame (WaitForIdle mid-graph).
        static const bool kCbtEditDebug = Platform::EnvironmentSwitchEnabled("GE_CBT_EDIT_DEBUG", false);
        if (kCbtEditDebug)
        {
            const CBTTerrain::CBTClassifyDesc& c = classify;
            if (c.DirtyMaxU > c.DirtyMinU && c.DirtyMaxV > c.DirtyMinV)
            {
                // Planet editing (plan §planet-editing): the spherical form names the cube
                // face the (face-local) rect lies on; planar keeps the rect-only form.
                if (feature.GetDomainConfig().DomainMode == CBTTerrain::kDomainSpherical)
                    Logger::Log::Info("CBT.Edit face={} rect=({},{},{},{})", c.DirtyFace,
                                      c.DirtyMinU, c.DirtyMinV, c.DirtyMaxU, c.DirtyMaxV);
                else
                    Logger::Log::Info("CBT.Edit rect=({},{},{},{})", c.DirtyMinU, c.DirtyMinV,
                                      c.DirtyMaxU, c.DirtyMaxV);
            }
        }
    }

    EmitCbtForwardDraw(d, feature);
}

} // namespace GameEngine::CBTTerrainECS
