#pragma once

// CBTRenderFeature — owns the single C3 CBT GPU instance (compute kernels +
// persistent buffers + material) and the per-frame drive state that CBTUpdateSystem
// pushes from the ECS world. CBTRenderNode records the update compute pass and the
// indexed-indirect forward draw from this feature each frame. Lazily initialized on
// the render thread (first DeclareForView), mirroring OceanRenderFeature.

#include "AssetCore/GUID.h"
#include "CBTTerrain/CBTActivityReadback.h"
#include "CBTTerrain/CBTInstance.h"
#include "CBTTerrain/CBTKernelSet.h"
#include "CBTTerrain/SphereSculptLayer.h"
#include "CBTTerrain/CBTUpdateRestGate.h"
#include "CBTTerrainECS/CBTTreeSeed.h"
#include "CBTTerrainECS/CBTValidationReadback.h"
#include "CBTTerrainECS/TerrainShadowBake.h"
#include "Components/Terrain/Terrain.h" // kNoTerrainSeaLevel
#include "Engine/Rendering/DrawBindings.h"
#include "Engine/Rendering/IRenderFeature.h"
#include "Engine/Rendering/WorldContentDigestTable.h"
#include "Rendering/Materials/ShaderVariantKey.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>
#include <span>
#include <vector>

namespace GameEngine::Rendering
{
class IDevice;
struct CameraData;
using ViewId = uint32_t;
} // namespace GameEngine::Rendering

namespace GameEngine::Engine::Renderer
{
class RenderServices;
class Material;
struct DrawCommand;
} // namespace GameEngine::Engine::Renderer

namespace GameEngine::TerrainECS
{
class TerrainRenderFeature;
struct TerrainInstanceInfo;
} // namespace GameEngine::TerrainECS

namespace GameEngine::CBTTerrainECS
{

class CBTRenderFeature final : public Engine::Renderer::IRenderFeature
{
  public:
    CBTRenderFeature() = default;
    ~CBTRenderFeature() override;

    // Lazy one-time GPU bring-up: load kernels from the staged CBT shader dir and
    // request their pipelines, then, on the first call that finds them all built,
    // allocate the persistent buffers and upload the roots + draw resources. Safe to
    // call every frame; a no-op once ready. Returns false while the kernel pipelines
    // are still building (the feature declares nothing meanwhile), and false for good
    // if the compiled kernels are missing or a kernel pipeline fails.
    bool EnsureInitialized(Rendering::IDevice& device, Engine::Renderer::RenderServices& rs);
    bool IsInitialized() const { return m_Initialized; }

    // Device-lost recovery. An in-place device rebuild destroys the 20 kernel pipelines
    // and every persistent buffer/texture/sampler the instance owns, while the cached
    // handles keep reading IsValid() — the next DispatchDirect would then SetPipeline a
    // dead handle (a silently skipped bind) and dispatch with nothing bound. Forget the
    // dead state and re-arm the lazy bring-up so the next EnsureInitialized re-creates it.
    void OnDeviceRebuilt(Rendering::IDevice* device) override;

    // The rendering domain + planet tuning CBTUpdateSystem pushes from the Terrain
    // component. DomainMode is kDomainPlanar / kDomainSpherical (CBTLayout.h).
    struct DomainConfig
    {
        uint32_t DomainMode = CBTTerrain::kDomainPlanar;
        float PlanetRadius = 2000.0f;
        float ReliefAmplitude = 60.0f;
        float ReliefFrequency = 6.0f;
        uint32_t ReliefOctaves = 4u; // fBM octaves for the sphere relief (matches TerrainPlanetRelief default)
        // Domain SHAPE only. A terrain's materials do not travel this path: both domains read the
        // terrain material table (TerrainRenderFeature), which TerrainExtractionSystem authors from
        // the same component this config is built from.
    };

    // Pushed by CBTUpdateSystem from the ECS world each frame. `classify` selects the
    // metric (screen-space in production; TargetDepth carries the max-depth cap);
    // `targetPixelError` is the authored split target (px at 1080 rows; BuildFrameParams scales it
    // to the view's render height through CBTTerrain::SplitThresholdPixels); `terrainSeaLevel` is the
    // Terrain component's authored water surface height (Components::kNoTerrainSeaLevel =
    // none; an Ocean module surface overrides it — see BuildFrameParams); `domain` picks
    // planar vs the spherical cube-sphere planet (plan §8 C7).
    void SetActive(bool active, const CBTTerrain::CBTClassifyDesc& classify, float targetPixelError,
                   float terrainSeaLevel, const DomainConfig& domain, uint32_t debugView)
    {
        // A drive change or an edited region makes this frame's update run, which the depth-derived
        // elision readers learn from WritesDynamicDepth before the update is declared.
        const bool edited = classify.DirtyMaxU > classify.DirtyMinU && classify.DirtyMaxV > classify.DirtyMinV;
        if (edited || active != m_Active ||
            std::memcmp(&classify, &m_Classify, sizeof(classify)) != 0 ||
            targetPixelError != m_TargetPixelError || terrainSeaLevel != m_TerrainSeaLevel ||
            std::memcmp(&domain, &m_DomainConfig, sizeof(domain)) != 0)
            m_UpdateAtRest.store(false, std::memory_order_relaxed);
        m_Active = active;
        m_Classify = classify;
        m_TargetPixelError = targetPixelError;
        m_TerrainSeaLevel = terrainSeaLevel;
        m_DebugView = debugView;
        // Force a full-pool VertexEval on the next recorded update when the planet's shape
        // params (radius / relief / octaves / domain) changed: those alter every bisector's
        // world corners, but none are flagged MODIFIED, so the quiescence gate would otherwise
        // leave stale corners until the region re-splits (plan §planet-shading perf).
        if (domain.DomainMode != m_DomainConfig.DomainMode ||
            domain.PlanetRadius != m_DomainConfig.PlanetRadius ||
            domain.ReliefAmplitude != m_DomainConfig.ReliefAmplitude ||
            domain.ReliefFrequency != m_DomainConfig.ReliefFrequency ||
            domain.ReliefOctaves != m_DomainConfig.ReliefOctaves)
            RequestForceVertexEval();
        m_DomainConfig = domain;
    }

    // A full evaluation stays pending until its recorded update submits. A newer request
    // survives an older update's submission, including an update retried after abandonment.
    uint64_t PendingForceVertexEval() const
    {
        const uint64_t requested = m_ForceVertexEvalRequested.load(std::memory_order_relaxed);
        return requested > m_ForceVertexEvalAcknowledged.load(std::memory_order_relaxed) ? requested : 0u;
    }

    // Also used by cbt_force_vertex_eval to re-sample every live bisector against the
    // selected height texture. Requesting it wakes an otherwise resting update.
    void RequestForceVertexEval()
    {
        m_ForceVertexEvalRequested.fetch_add(1u, std::memory_order_relaxed);
        m_UpdateAtRest.store(false, std::memory_order_relaxed);
    }
    bool IsActive() const { return m_Active; }
    Engine::Renderer::WorldContentDigestTable& GetWorldContent() { return m_WorldContent; }
    uint64_t ContentDigest(uint64_t worldId) const override { return m_WorldContent.Get(worldId); }
    // The terrain's depth can change unless its update is at rest (IsUpdateAtRest): a tree that
    // splits or merges, and a sculpt or streamed height edit, change depth with a static camera
    // and no other elision key moving. Read by the frame spine before this frame's update is
    // decided, so besides the last decision it reports every change that will make this frame's
    // update run: one SetActive or RequestForceVertexEval announced, a pending full re-evaluation,
    // a held tree restart, and a terrain retire or a new height source the node has not consumed
    // yet (read live from TerrainRenderFeature). A camera or render-extent change is not reported:
    // every depth-derived reader keys on those itself.
    bool WritesDynamicDepth() const override;

    // Folds the newest landed update reading (CBTActivityReadback) into the rest gate. Called by
    // CBTRenderNode once per frame, before it decides on the update.
    void ConsumeUpdateActivity();
    // True when this frame's CBT update can be skipped: its inputs (`params` from BuildFrameParams,
    // the classify, the bound heightmap) equal the previous update's, the updates since they last
    // changed left the tree at rest (CBTUpdateRestGate), and neither an edited region nor a full
    // vertex re-evaluation is pending. The draw then reads the buffers the last update wrote.
    bool IsUpdateAtRest(const CBTTerrain::CBTFrameParams& params);
    // True when the inputs IsUpdateAtRest last compared equal the update before them: the camera
    // holds still, so the update about to be recorded applies the rest rule
    // (CBTTerrain::CBTClassifyDesc::Settle) and the tree settles to the one tree the pose asks for.
    bool InputsHeld() const { return m_RestGate.InputsHeld(); }
    // For the update about to be declared into `frame`: its readback slot (invalid = unread).
    // `forced` = the update re-evaluates every vertex (GateVertexEval 0).
    CBTTerrain::CBTActivitySlot BeginUpdateActivityReadback(const Rendering::RenderGraph::RGFrame& frame,
                                                            bool forced);

    void OnFrameSubmittedRG(Rendering::RenderGraph::RGFrame& frame,
                            const Rendering::IDevice::GpuSyncToken& token) override
    {
        m_ActivityReadback.OnFrameSubmitted(frame, token);
    }
    void OnFrameStreamRetiredRG(Rendering::RenderGraph::RGFrame& frame) override
    {
        m_ActivityReadback.OnFrameStreamRetired(frame);
        if (m_ParamsGraph == &frame)
        {
            m_ParamsGraph = nullptr;
            m_ParamsFrame = ~0ull;
            m_ParamsValid = false;
        }
        if (m_UpdateGraph == &frame)
        {
            m_UpdateGraph = nullptr;
        }
    }
    const CBTTerrain::CBTClassifyDesc& GetClassify() const { return m_Classify; }
    const DomainConfig& GetDomainConfig() const { return m_DomainConfig; }
    float GetTargetPixelError() const { return m_TargetPixelError; }
    // The split threshold (render pixels) and the render height BuildFrameParams applied on its last
    // frame: the authored target scaled to that height. Both 0 before the first frame.
    float GetAppliedSplitThresholdPx() const { return m_AppliedSplitThresholdPx; }
    uint32_t GetAppliedRenderHeightPx() const { return m_AppliedRenderHeightPx; }

    CBTTerrain::CBTInstance& GetInstance() { return m_Instance; }
    CBTValidationReadback& GetValidationReadback() { return m_ValidationReadback; }

    // Planet editing (plan §planet-editing): the AUTHORITATIVE editable sculpt layer (dab +
    // baked terrain-modifier stack) now lives in TerrainService — the two writers (editor
    // brush + TerrainModifierSystem) span modules the feature cannot reach. The feature only
    // UPLOADS it: BuildFrameParams reads TerrainService's version + atlas into this frame's
    // ring slot (gated). CBTUpdateSystem reads TerrainService's version + dirty (face,rect) to
    // drive Classify; the physics colliders read TerrainService's mirror directly.

    // The CPU-authoritative planet surface height (metres above PlanetRadius) at a world
    // direction: procedural relief (from the active planet tuning) + TerrainService's editable
    // sculpt layer — the SAME height the GPU VertexEval displaces to. The editor brush refines
    // its ray->planet hit against this so the cursor tracks tall relief / sculpt instead of
    // drifting on the analytic base sphere (#488 follow-up). `dir` need not be unit.
    float SampleSphereSurfaceHeight(float dx, float dy, float dz) const;

    // Restarts the tree from its roots when it no longer matches what it tessellates: the desired
    // domain (from the Terrain component) differs from the seeded one, a terrain was retired since
    // the seed (ConsumeTerrainRetire), or the depth cap fell below the deepest cap the tree was
    // allowed (CBTTreeSeed). The first request restarts at once; repeats within CBTTreeSeed's
    // window wait until they settle, so an inspector drag restarts the tree at its first step
    // and once when it ends. A restarted tree is refined to CBTTreeSeed::kRestartSeedDepth (or the
    // cap) before its first draw, so no frame draws the bare roots. The re-seed also zeroes the work-queue
    // counters, the overflow total included. `frameCounter` is this frame's value from
    // CBTInstance::AdvanceFrameCounter. Called render-side after EnsureInitialized and
    // ConsumeTerrainRetire (owns the device + command lists). No-op when nothing changed. Logs a
    // GE_CBT.Planet line on every (re)seed so a verify can grep the active domain, the roots and
    // the reason.
    void EnsureSeeded(uint32_t frameCounter);

    // In-place re-provision safety. A SamplesPerMeter/size edit that preserves the runtime tiled
    // handle takes the DEBOUNCED re-provision path: ReleaseTerrainResources retires the old unified
    // heightmap (deferred-destroy quarantine) and recreates it at the new resolution. CBT samples
    // that heightmap through its own descriptor ring (bindings 15/18/19), which the per-slot lazy
    // SetHeightSource only refreshes one element per frame — so a retired image can be freed while a
    // ring element still references it (device-lost in VertexEval a few frames later). This polls the
    // terrain feature's retire generation and, on a bump, retires graphics users and rebinds every ring
    // element to its default in the SAME update the re-provision commits (RefreshTerrainSources),
    // then forces a full VertexEval since the new heightmap staled every cached bisector corner.
    // A retire also means the tree grew on a terrain that is gone, so it marks the tree for a
    // restart (EnsureSeeded). Called render-side before EnsureSeeded and BuildFrameParams. No-op
    // when nothing retired. False leaves the generation unconsumed for retry.
    bool ConsumeTerrainRetire(uint64_t retireGeneration);

    /// Builds and binds the owner's terrain parameters once per graph frame.
    /// Followers reuse that snapshot. False means the owner has no usable source bindings.
    /// frameIndex is the unwrapped CBT counter used by the parameter and descriptor rings.
    bool BuildFrameParams(Engine::Renderer::RenderServices& rs, Rendering::ViewId viewId,
                          uint32_t renderWidth, uint32_t renderHeight, uint32_t frameIndex,
                          CBTTerrain::CBTFrameParams& out, Rendering::RenderGraph::RGFrame& frame);

    // The real terrain heightmap bound by the most recent BuildFrameParams, or an
    // invalid handle when the flat 1x1 default is in use (no terrain / heightmap not
    // uploaded yet). CBTRenderNode forwards this to CBTUpdateNode so the update pass
    // can declare its read edge on the texture VertexEval samples (plan §8 C5).
    Rendering::TextureHandle GetBoundHeightmap() const { return m_BoundHeightmap; }
    // The terrain's shadow source set by the most recent BuildFrameParams (no height texture when
    // none is bound or the terrain is atlas-backed), and the bake that turns it into the map.
    const TerrainShadowSource& GetTerrainShadowSource() const { return m_TerrainShadowSource; }
    TerrainShadowBake& GetTerrainShadowBake() { return m_TerrainShadowBake; }

    /// Claims the update for one terrain-owning graph and frame key. Matching views reuse it.
    /// A retired owner loses its pointer identity but retains its last claimed key.
    /// Independently clocked streams require application-frame ownership outside this gate.
    bool ClaimUpdate(const void* graph, uint64_t rgFrameIndex);
    bool UpdateDeclaredInGraph(const void* graph, uint64_t rgFrameIndex) const
    {
        return m_UpdateGraph == graph && m_UpdateRgFrame == rgFrameIndex;
    }

    /// Captures this update's publication. Only its successful submission consumes pending work.
    std::function<void()> BuildUpdateSubmittedCallback(uint64_t fullEvaluationTicket);
    /// Render thread: the selected table or payload has not reached a submitted CBT update.
    bool HasUnconsumedHeightPages() const;
    /// Unions bounds covering every unconsumed page revision; false requires full evaluation.
    bool ApplyHeightPageDirtyRegion(CBTTerrain::CBTClassifyDesc& classify) const;
    /// Cross-thread confirmation used by the extraction-side dirty-region cursor.
    uint64_t GetUpdateSubmissionCount() const { return m_UpdateSubmissionCount.load(std::memory_order_relaxed); }

    // Builds the indexed-indirect CBT DrawCommand for `viewId`. Returns false without
    // logging while the material's base compile is in flight (the terrain draws once
    // its pipeline publishes), and false loudly (log + no draw) if the material's
    // graphics variant failed to compile (DescriptorSetLayouts empty), the POC's
    // silent-null-variant trap (plan §9).
    // The DrawBindings span points into feature-owned storage stable for the frame.
    bool BuildDrawCommand(Engine::Renderer::RenderServices& rs, Rendering::ViewId viewId,
                          Rendering::MaterialKeyword passKeywords,
                          Engine::Renderer::DrawCommand& outCmd);

    // Builds the camera-prepass head of `colour` (BuildDrawCommand's draw for `viewId`): the same
    // VISIBLE-stream record, identity index buffer and set-2 buffers, drawn with the material's
    // depth-only variant, which runs the same vertex modifier with no fragment stage. Returns false
    // until the device has built that variant's pipeline for the view's prepass (requested on the
    // first miss; never, if the build failed); the colour draw then writes its own depth.
    bool BuildPrepassHead(Engine::Renderer::RenderServices& rs, Rendering::ViewId viewId,
                          const Engine::Renderer::DrawCommand& colour,
                          Engine::Renderer::DrawCommand& outHead) const;

    // Whether the loaded kernels carry the narrow (u32) heap, which caps the decode depth
    // at the heap's own range (TerrainProvisioning). False until the kernels are loaded.
    bool UsesNarrowHeap() const { return m_KernelSet.IsNarrowHeap(); }

  private:
    // Loads the kernel programs and requests their pipelines (EnsureInitialized's
    // first step). False when the programs are missing.
    bool RequestKernels(Rendering::IDevice& device);
    bool EnsureMaterial(Engine::Renderer::RenderServices& rs);
    // True when TerrainRenderFeature has retired a texture set since ConsumeTerrainRetire last ran,
    // or the height source BuildFrameParams would bind differs from the one it last bound.
    bool TerrainSourceChanged() const;
    // The height texture the update samples for a terrain: its atlas when atlas-backed, else its
    // unified heightmap (invalid until uploaded).
    static Rendering::TextureHandle BoundHeightSource(bool atlasBacked, Rendering::TextureHandle atlas,
                                                      Rendering::TextureHandle heightmap);
    // Provision a distance-filtered chain and draw-level ring for the CBT camera from PageLevelRule.
    // Write its scale/top into out.HeightChain for conservative classification.
    // Unsupported domains/heaps or missing unified source/range storage retain cached corners with scale zero.
    void ApplyHeightChain(const TerrainECS::TerrainInstanceInfo& terrain, const Rendering::CameraData& camera,
                          uint32_t frameIndex, CBTTerrain::CBTFrameParams& out);
    // The height page cache the update samples for terrain `terrainIndex` when its height is paged
    // (TerrainHeightPageFeature latched its page table and created the cache) and these kernels
    // read pages (the wide arm); invalid otherwise. Its page-table words go to `words`.
    Rendering::TextureHandle PagedHeightSource(uint32_t terrainIndex, std::span<const uint32_t>& words,
                                               uint64_t& version, uint64_t& contentGeneration,
                                               const Rendering::RenderGraph::RGFrame* frame = nullptr,
                                               uint64_t* revision = nullptr) const;
    // Logs "Terrain pages: the CBT draws terrain <index> (generation <g>) from its pages|texture
    // from frame <frameIndex>" when the committed source differs from the last one logged.
    void LogCommittedHeightSource(uint32_t terrainIndex, uint32_t generation, bool paged, uint32_t frameIndex);

    CBTTerrain::CBTKernelSet m_KernelSet;
    CBTTerrain::CBTInstance m_Instance;
    // The update skip at rest (IsUpdateAtRest): the gate, the readback that feeds it, the input
    // bytes compared each frame (a member so the comparison allocates nothing), and the last answer.
    CBTTerrain::CBTUpdateRestGate m_RestGate;
    CBTTerrain::CBTActivityReadback m_ActivityReadback;
    std::vector<uint8_t> m_UpdateInputs;
    std::atomic<bool> m_UpdateAtRest{false};
    // GE_CBT_VALIDATE's counter readback; destroyed with the feature, before the device.
    CBTValidationReadback m_ValidationReadback;
    Engine::Renderer::Material* m_Material = nullptr;
    // Set-2 SSBO bindings resolved by MaterialBinder by reflected instance name.
    // The CBT buffer handles are persistent, so this is built once and its span
    // stays valid every frame (no per-view scratch needed).
    std::vector<Engine::Renderer::DrawBindings::BufferEntry> m_DrawBuffers;
    // Compat only: cbt_surface declares its terrain maps as named set-2 bindings there, because
    // WebGPU exposes no binding array to index. Empty on the bindless profile.
    std::vector<Engine::Renderer::DrawBindings::TextureEntry> m_DrawTextures;
    CBTTerrain::CBTClassifyDesc m_Classify{};
    // Planet-editing sculpt upload gate (v2): the authoritative paged sculpt store lives in
    // TerrainService (dab + baked modifier stack, shared across modules); the feature only uploads
    // it. The gate refreshes each ring slot once per sculpt version so an idle edited planet uploads
    // nothing; the scratches hold the copied page pool + page table for that upload.
    CBTTerrain::SphereSculptUploadGate m_SphereSculptUploadGate;
    std::vector<float> m_SphereSculptScratch;
    std::vector<uint32_t> m_SphereSculptTableScratch;
    float m_TargetPixelError = Components::kDefaultTerrainTargetPixelError;
    float m_AppliedSplitThresholdPx = 0.0f;
    uint32_t m_AppliedRenderHeightPx = 0u;
    // The Terrain component's authored water surface height (Components::kNoTerrainSeaLevel =
    // none). BuildFrameParams resolves it against the Ocean module's surface.
    float m_TerrainSeaLevel = Components::kNoTerrainSeaLevel;
    uint32_t m_DebugView = 0u;            // TerrainDebugView (from Terrain) -> surface DebugMode
    uint32_t m_LoggedDebugView = 0u;      // last-logged value for the one-shot toggle signal
    DomainConfig m_DomainConfig{};       // desired domain + planet tuning (from Terrain)
    CBTTreeSeed m_TreeSeed; // what the instance's tree was seeded for (domain, cap, retire)
    // Last terrain-texture retire generation consumed (see ConsumeTerrainRetire). Synced to the
    // terrain feature's live value on first sight so a re-provision that happened before CBT went
    // active does not trigger a spurious full-ring rebind; thereafter a bump drives one refresh.
    uint64_t m_LastTerrainRetireGeneration = 0;
    bool m_TerrainRetireGenerationSynced = false;
    // This frame's ring element (CBTFrameRingSlot of the unwrapped frame counter), stashed
    // by BuildFrameParams so BuildDrawCommand can offset-bind the per-slot graphics buffers
    // (surface params + sculpt atlas).
    uint32_t m_FrameSlot = 0u;
    // Phase E: last-uploaded indirection-table version per ring slot. The atlas rows are
    // re-uploaded to a ring slot only when the residency table changed since that slot was last
    // written (quiescence — a parked camera uploads nothing). Value-initialized (all 0) rather than
    // a hardcoded four ~0ull, which would silently under-seed the array if the ring depth changed;
    // the first upload is forced by the m_AtlasBoundTexture change below (invalid -> the real atlas).
    uint64_t m_AtlasUploadedTableVersion[CBTTerrain::kCBTFrameParamsRing] = {};
    // The atlas height texture whose rows were last uploaded into each ring slot. A bound-terrain
    // change brings a different texture whose TableVersion may numerically equal the previous
    // terrain's; the version gate alone would then skip the row upload and the new terrain would
    // resolve through the old terrain's rows. A texture change resets the slot's uploaded version.
    Rendering::TextureHandle m_AtlasBoundTexture[CBTTerrain::kCBTFrameParamsRing] = {};
    // The paged height resolve's page-table upload gate per ring slot, as the atlas rows' above: the
    // terrain whose words each slot holds and their version (a terrain change re-uploads).
    struct PagePublication
    {
        uint32_t Terrain = 0;
        uint64_t Version = 0;
        uint64_t ContentGeneration = 0;
        uint64_t Revision = 0;
        uint32_t TerrainGeneration = 0;
        bool operator==(const PagePublication&) const = default;
    };
    PagePublication m_SelectedPages;
    PagePublication m_ConsumedPages;
    uint64_t m_ParamsFrame = ~0ull;
    const Rendering::RenderGraph::RGFrame* m_ParamsGraph = nullptr;
    bool m_ParamsValid = false;
    CBTTerrain::CBTFrameParams m_OwnerParams{};
    uint64_t m_PageUploadedTableVersion[CBTTerrain::kCBTFrameParamsRing] = {};
    uint32_t m_PageBoundTerrain[CBTTerrain::kCBTFrameParamsRing] = {};
    // The terrain BuildFrameParams last bound (its render handle index), for TerrainSourceChanged.
    uint32_t m_BoundTerrainIndex = 0u;
    // The height source the frame parameters last committed (the terrain's handle, paged or not),
    // logged with the frame when it changes: the source that draws, not the page predicate's intent.
    uint32_t m_CommittedSourceTerrain = ~0u;
    uint32_t m_CommittedSourceGeneration = ~0u;
    bool m_CommittedSourcePaged = false;
    // Full-pool VertexEval requests from initialization, re-seeding and parameter changes.
    // Acknowledgement requires the selected update to record and submit successfully.
    std::atomic<uint64_t> m_ForceVertexEvalRequested{1u};
    std::atomic<uint64_t> m_ForceVertexEvalAcknowledged{0u};
    bool m_SphereShadingSignalLogged = false; // one-shot GE_CBT.SphereShading signal
    bool m_MultiTerrainWarned = false; // warn-once: CBT renders only terrain 0
    // The real terrain heightmap bound this frame (invalid = flat default). Set by
    // BuildFrameParams, read by CBTRenderNode for the CBT.Update read edge.
    Rendering::TextureHandle m_BoundHeightmap{};
    TerrainShadowSource m_TerrainShadowSource{};
    TerrainShadowBake m_TerrainShadowBake;
    Rendering::IDevice* m_Device = nullptr;
    // The owning RenderServices (set by EnsureInitialized), for TerrainSourceChanged.
    Engine::Renderer::RenderServices* m_Services = nullptr;
    // (graph, RGFrame index) the CBT.Update was declared into this frame — the
    // draw/producer inseparability key (see ClaimUpdate).
    const void* m_UpdateGraph = nullptr;
    uint64_t m_UpdateRgFrame = ~0ull;
    // Render-to-extraction confirmation count of submitted updates.
    std::atomic<uint64_t> m_UpdateSubmissionCount{0};
    bool m_Initialized = false;
    bool m_InitAttempted = false; // one bring-up attempt; failures don't retry-spam
    bool m_Active = false;
    bool m_MaterialAttempted = false;
    Engine::Renderer::WorldContentDigestTable m_WorldContent;
};

} // namespace GameEngine::CBTTerrainECS
