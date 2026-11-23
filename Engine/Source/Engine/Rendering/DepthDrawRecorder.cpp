// Prepares depth/shadow draws before recording and replays frame-owned packets.
#include "Engine/Rendering/DepthDrawRecorder.h"
#include "IndexedDepthDraw.h"
#include "Engine/Rendering/VirtualShadows/VirtualShadowFeedback.h"
#include "Rendering/Core/DrawProducerRegistry.h"
#include "Engine/Rendering/ParallaxReliefDepth.h"

#include "Engine/Rendering/CpuDrawStreamBuilder.h"
#include "Engine/Rendering/DrawCommandProducer.h"
#include "Engine/Rendering/DrawStreamLookupKey.h"
#include "Engine/Rendering/Material.h"
#include "Engine/Rendering/MaterialBinder.h"
#include "Engine/Rendering/MaterialSystem.h"
#include "Engine/Rendering/MeshGPURegistry.h"
#include "Engine/Rendering/PipelineVariantCache.h"
#include "Engine/Rendering/TextureService.h"
#include "Engine/Rendering/ViewRegistry.h"
#include "Engine/Rendering/WorldDrawBuilder.h"
#include "Platform/Environment.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/GPUDrawStreamBuilder.h"
#include "Rendering/Core/GPUScene.h"
#include "Rendering/Core/HashUtils.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Geometry/VertexAttributeFlags.h"
#include "Rendering/Materials/LegacyMaterialLanes.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <span>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "RenderServicesDetail.h"

namespace GameEngine
{
namespace Engine::Renderer
{
using namespace ::GameEngine::Rendering;

namespace
{
// The stripped depth layout (StrippedDepthVertexFlags) without the skinning
// streams the entry has no buffer for (DepthHeadVertexFlags adds back what a head reads).
Rendering::VertexAttributeFlags EffectiveVertexFlagsForDepthPass(
    const Rendering::MeshGPUEntry&        entry,
    const Rendering::MeshGPUEntryBindings& bindings)
{
    Rendering::VertexAttributeFlags flags = StrippedDepthVertexFlags(entry.vertexFlags);
    if (!bindings.jointsVB.IsValid())
        flags &= ~Rendering::VertexAttributeFlags::HasJoints;
    if (!bindings.weightsVB.IsValid())
        flags &= ~Rendering::VertexAttributeFlags::HasWeights;
    if (!bindings.joints1VB.IsValid())
        flags &= ~Rendering::VertexAttributeFlags::HasJoints1;
    if (!bindings.weights1VB.IsValid())
        flags &= ~Rendering::VertexAttributeFlags::HasWeights1;
    return flags;
}

// A vertex stream a depth draw binds beyond Core and skinning, at its fixed slot
// (VertexLayoutBuilder.h).
struct OptionalStreamBind
{
    Rendering::BufferHandle Buffer{};
    uint32_t Slot = 0;
};

// The tangent, colour and UV1..7 streams `drawVertexFlags` carries: exactly the
// bindings the draw's pipeline declares beyond Core and skinning, so none is left
// empty (VUID-vkCmdDrawIndexed-04007) and none is bound unread.
struct OptionalStreamBinds
{
    std::array<OptionalStreamBind, 9> Items{};
    uint32_t Count = 0;

    std::span<const OptionalStreamBind> View() const { return {Items.data(), Count}; }
};

OptionalStreamBinds CollectOptionalStreamBinds(Rendering::VertexAttributeFlags drawVertexFlags,
                                               const Rendering::MeshGPUEntryBindings& bindings)
{
    OptionalStreamBinds binds;
    if (Rendering::HasFlag(drawVertexFlags, Rendering::VertexAttributeFlags::HasTangent))
        binds.Items[binds.Count++] = {bindings.tangentVB, 1u};
    if (Rendering::HasFlag(drawVertexFlags, Rendering::VertexAttributeFlags::HasColor))
        binds.Items[binds.Count++] = {bindings.colorVB, 2u};
    if (Rendering::HasFlag(drawVertexFlags, Rendering::VertexAttributeFlags::HasUV1))
        binds.Items[binds.Count++] = {bindings.uv1VB, 3u};
    for (uint32_t i = 0; i < bindings.extraUvVB.size(); ++i)
    {
        const auto uvFlag = static_cast<Rendering::VertexAttributeFlags>(
            static_cast<uint32_t>(Rendering::VertexAttributeFlags::HasUV2) << i);
        if (Rendering::HasFlag(drawVertexFlags, uvFlag))
            binds.Items[binds.Count++] = {bindings.extraUvVB[i], 8u + i};
    }
    return binds;
}

} // namespace

std::span<const DrawCommand> ContributorDepthCommands(const ViewRegistry& views, Rendering::ViewId viewId,
                                                      DepthPassType passType,
                                                      Rendering::GPUDrawStreamBuilder::SlicePhase phase)
{
    if (passType == DepthPassType::DeformationMotion || phase != Rendering::GPUDrawStreamBuilder::SlicePhase::A)
        return {};
    const auto* pv = views.FindPerView(viewId);
    if (!pv)
        return {};
    const auto& cmds = pv->DepthCommands[static_cast<size_t>(passType)];
    return {cmds.data(), cmds.size()};
}

std::span<const DrawCommand> NonOccludingPrepassHeads(const ViewRegistry& views, Rendering::ViewId viewId)
{
    const auto* pv = views.FindPerView(viewId);
    if (!pv)
        return {};
    return {pv->NonOccludingPrepassHeads.data(), pv->NonOccludingPrepassHeads.size()};
}

// Prepass shared-depth collapse (prepass-collapse-2026-07-24): eligible
// camera-prepass draws bind the shared position-only depth PSO instead of the
// per-material full-fat variant. The kill switch restores the pre-change
// selection exactly for triage; the draw set and ranges are identical either
// way.
bool PrepassSharedDepthEnabled()
{
    static const bool kEnabled = Platform::EnvironmentSwitchEnabled("GE_PREPASS_SHARED_DEPTH", true);
    return kEnabled;
}

bool ParallaxDepthOffsetEnabled()
{
    static const bool kEnabled = [] {
        const char* v = std::getenv("GE_PARALLAX_DEPTH_OFFSET");
        return !(v && v[0] == '0');
    }();
    return kEnabled;
}

struct PreparedOwnerCompaction
{
    std::optional<IndexedDepthDraw> Input;
    IndexedOwnerReplay Output;
};

struct PreparedDepthOnlyPass
{
    struct Draw
    {
        DrawCommand Command;
        PreparedDrawBindings Bindings;
        Rendering::MeshGPUEntryBindings Geometry;
        Rendering::VertexAttributeFlags VertexFlags = Rendering::VertexAttributeFlags::None;
        size_t PushConstantOffset = 0;
        size_t PushConstantSize = 0;
        std::optional<uint32_t> ServedVersion;
        uint32_t MaterialIndex = 0;
        uint32_t DiagnosticMaterialIndex = 0;
        bool Entity = false;
        uint32_t MeshIndex = 0;
        uint32_t MaxDrawCount = 0;
        size_t RangeCount = 0;
        Rendering::GraphicsPipelineId ConcretePipeline{};
    };
    explicit PreparedDepthOnlyPass(DepthOnlyPassParamsRG params) : Params(std::move(params))
    {
        Inputs.PassResources = std::move(Params.Resources);
    }
    DepthOnlyPassParamsRG Params;
    PassBindingContext Inputs;
    std::vector<Draw> Draws;
    std::vector<std::byte> PushConstants;
    struct IndexedSource
    {
        IndexedDepthDraw Draw;
        const Material* MaterialOwner;
        uint32_t MaterialRevision;
        Rendering::FrontFace Winding;
        bool VertexAlpha;
        bool Crossfading;
        Rendering::GraphicsPipelineId Pipeline;
        size_t DrawIndex;
        Rendering::MaterialKeyword OwnerKeywords;
    };
    std::vector<IndexedSource> IndexedSources;
    RenderGraph::RGFrameStamp IndexedFrame;
    struct OwnerPacket
    {
        size_t DrawIndex = 0;
        PipelineHandle Pipeline;
        RenderGraph::RGBuffer State;
        RenderGraph::RGBuffer Draws;
        PreparedOwnerCompaction* Compaction = nullptr;
        bool UsesMeshReplay = false;
    };
    std::array<OwnerPacket, 2> Owners{};
    MaterialSystem* Materials = nullptr;
    DepthUnderDrawTracker* UnderDraw = nullptr;
    bool UnderDrew = false;
    DepthUnderDrawReason UnderDrawReason = DepthUnderDrawReason::Count; // the first reason this frame
    bool Walked = false;
    size_t EntityBatchCount = 0;
    bool Consolidated = false;
    DepthProducerState AllProducer;
    DepthProducerState RigidProducer;
};

PreparedDepthOnlyPass* AllocatePreparedDepthOnlyPass(Rendering::RenderGraph::RGFrame& frame,
                                                    DepthOnlyPassParamsRG params)
{
    auto* pass = frame.AllocatePassData<PreparedDepthOnlyPass>(std::move(params));
    pass->IndexedFrame.Stamp(frame);
    return pass;
}

namespace
{
struct DepthBatchPreparation
{
    const WorldDrawBuilder::BatchKey& Key;
    const Engine::Renderer::Material* Material;
    const Rendering::MeshGPUEntry* Mesh;
    Rendering::MeshGPUEntryBindings Geometry;
    Rendering::VertexAttributeFlags DepthVertexFlags;
    Rendering::VertexAttributeFlags DrawVertexFlags;
    DepthSegmentPipelineChoice Head;
    Rendering::CullModeFlags CullMode;
    bool IsSkinned;
    bool TexturesReady;
};

class DepthPassPreparation
{
  public:
    DepthPassPreparation(RenderGraph::RGPrepareContext& context, PreparedDepthOnlyPass& data,
                         const DepthDrawServices& services)
        : m_Context(context), m_Data(data), m_Services(services)
    {
    }

    bool Prepare()
    {
        m_Data.Draws.clear();
        m_Data.PushConstants.clear();
        m_Data.IndexedSources.clear();
        for (auto& owner : m_Data.Owners)
            owner.Pipeline = {};
        m_Data.AllProducer = {};
        m_Data.RigidProducer = {};
        const bool virtualPage = m_Data.Params.VirtualPageTile.has_value();
        if (virtualPage)
        {
            const uint8_t casterSlice = m_Data.Params.VirtualCasterSlice;
            const bool pageSlice = casterSlice >= kVirtualShadowPageCullingIndexBase &&
                                   casterSlice < kVirtualShadowCasterCullingIndex;
            if (*m_Data.Params.VirtualPageTile >= kVirtualShadowMaxPhysicalPages ||
                (!pageSlice && casterSlice != kVirtualShadowCasterCullingIndex) ||
                m_Data.Params.Casters != CascadeCasters::All ||
                m_Data.Params.Phase != Rendering::GPUDrawStreamBuilder::SlicePhase::A ||
                (m_Data.Params.PassType != DepthPassType::ShadowCascade &&
                 m_Data.Params.PassType != DepthPassType::SpotShadow &&
                 m_Data.Params.PassType != DepthPassType::PointShadow) ||
                !m_Context.IsFor(m_Data.Params.VirtualFrame) ||
                !m_Data.Params.VirtualCasterOrdering.IsValid() || !m_Services.Streams)
                return false;
            const auto ordering = m_Context.GetBuffer(m_Data.Params.VirtualCasterOrdering);
            const auto expected = m_Services.Streams->GetSentinelBuffer();
            if (!ordering.IsValid() || !expected.IsValid() || ordering != expected)
                return false;
        }
        m_Data.Materials = &m_Services.Materials;
        m_Data.UnderDraw = m_Services.UnderDraw;
        m_Data.UnderDrew = false;
        m_Data.UnderDrawReason = DepthUnderDrawReason::Count;
        m_Data.Walked = false;
        m_Data.EntityBatchCount = 0;
        m_Data.Consolidated = false;
        const auto submissions = m_Services.DrawBuilder.GetShadowSubmissionStates(m_Data.Params.ViewId);
        m_Data.AllProducer = SeedProducer(submissions.All);
        m_Data.RigidProducer = SeedProducer(submissions.Rigid);
        if (m_Data.Params.Casters != CascadeCasters::All)
            m_Data.AllProducer.Complete = false;
        if (m_Data.Params.Casters == CascadeCasters::Skinned)
            m_Data.RigidProducer.Complete = false;
        // The deforming-motion producer draws one subset of the same keys: the
        // materials whose vertex modifier the composed motion variant can evaluate
        // at two endpoints. Every other pass type draws the view's whole set.
        const bool motionPass = m_Data.Params.PassType == DepthPassType::DeformationMotion;
        // The non-occluding prepass draws its forward heads alone: the entity batches are in the camera
        // prepass already, so the key list stays empty and every entity path below is skipped.
        const bool nonOccludingPrepass = m_Data.Params.PassType == DepthPassType::Prepass &&
                                         m_Data.Params.Phase == Rendering::GPUDrawStreamBuilder::SlicePhase::A &&
                                         m_Data.Params.Heads == PrepassHeads::NonOccluding;
        std::span<const WorldDrawBuilder::BatchKey> entityKeys =
            nonOccludingPrepass ? std::span<const WorldDrawBuilder::BatchKey>{}
            : motionPass        ? m_Services.DrawBuilder.GetDeformingBatchKeys(m_Data.Params.ViewId)
                                : m_Services.DrawBuilder.GetBatchKeys(m_Data.Params.ViewId);

        const auto* passView = m_Services.Views.FindPerView(m_Data.Params.ViewId);
        m_PassKeywords =
            DepthPassKeywords(passView ? passView->WorldPassKeywords : std::nullopt);
        m_Data.AllProducer.ServedContentKey =
            HashUtils::HashValue(m_Data.AllProducer.ServedContentKey, m_PassKeywords);
        m_Data.RigidProducer.ServedContentKey =
            HashUtils::HashValue(m_Data.RigidProducer.ServedContentKey, m_PassKeywords);

        // bits so the prepass doesn't collide with cascade 0 in MaterialBinder's
        // per-pass descriptor cache (the terrain-flickers-black bug class).
        const uint32_t conventionalPassInstanceIndex =
            (static_cast<uint32_t>(m_Data.Params.PassType) << 16) |
            ((m_Data.Params.PassType == DepthPassType::ShadowCascade ||
              m_Data.Params.PassType == DepthPassType::PointShadow ||
              m_Data.Params.PassType == DepthPassType::TransmittanceCascade)
                 ? m_Data.Params.CascadeIndex
                 : 0u);
        constexpr uint32_t kVirtualPagePassTag =
            (static_cast<uint32_t>(DepthPassType::Count) + 1u) << 16;
        const uint32_t passInstanceIndex = virtualPage
            ? kVirtualPagePassTag | *m_Data.Params.VirtualPageTile : conventionalPassInstanceIndex;
        // Contributor draws (CBT terrain in the camera prepass, terrain grass in the non-occluding
        // prepass, registered depth producers in the light-space passes) carry their own pinned
        // depth pipelines.
        const auto depthCommands =
            nonOccludingPrepass ? NonOccludingPrepassHeads(m_Services.Views, m_Data.Params.ViewId)
                                : ContributorDepthCommands(m_Services.Views, m_Data.Params.ViewId, m_Data.Params.PassType, m_Data.Params.Phase);
        // The camera prepass also draws the non-occluding heads when no non-occluding prepass was
        // declared for the view (a pipeline without DepthResolve, or a resolve that did not run).
        const auto fallbackHeads =
            (m_Data.Params.PassType == DepthPassType::Prepass && !nonOccludingPrepass &&
             m_Data.Params.Phase == Rendering::GPUDrawStreamBuilder::SlicePhase::A && passView != nullptr &&
             !passView->NonOccludingPrepassDeclared)
                ? NonOccludingPrepassHeads(m_Services.Views, m_Data.Params.ViewId)
                : std::span<const DrawCommand>{};
        m_CascadeIndex = Rendering::GPUDrawStreamBuilder::kCascadeIndexNone;
        if (virtualPage)
            m_CascadeIndex = m_Data.Params.VirtualCasterSlice;
        else if (m_Data.Params.PassType == DepthPassType::ShadowCascade ||
            m_Data.Params.PassType == DepthPassType::TransmittanceCascade)
            m_CascadeIndex = static_cast<uint8_t>(m_Data.Params.CascadeIndex);
        else if (m_Data.Params.PassType == DepthPassType::AreaShadow)
            m_CascadeIndex = kAreaShadowCullingIndex;
        else if (m_Data.Params.PassType == DepthPassType::SpotShadow)
            m_CascadeIndex = kSpotShadowCullingIndex;
        else if (m_Data.Params.PassType == DepthPassType::PointShadow)
            m_CascadeIndex = PointShadowCullingIndex(m_Data.Params.CascadeIndex);

        const bool virtualRoutePublished = virtualPage && m_Services.Streams &&
            m_Services.Streams->HasPublishedRangesForPhase(static_cast<uint32_t>(m_Data.Params.ViewId),
                m_CascadeIndex, Rendering::GPUDrawStreamBuilder::SlicePhase::A);
        const bool virtualPunctualProducer = virtualRoutePublished &&
            (m_Data.Params.PassType == DepthPassType::SpotShadow ||
             m_Data.Params.PassType == DepthPassType::PointShadow);
        if (!depthCommands.empty() || !fallbackHeads.empty() || m_Services.CpuDrawStream ||
            (virtualPage && !virtualRoutePublished) ||
            (m_Data.Params.PassType != DepthPassType::ShadowCascade && !virtualPunctualProducer))
        {
            m_Data.AllProducer.Complete = false;
            m_Data.RigidProducer.Complete = false;
        }
        LogMeshPassDiagnostic(
            "depth", "pass-enter", m_Data.Params.ViewId,
            entityKeys.size(), depthCommands.size(),
            m_Services.Streams ? m_Services.Streams->GetRangeCount() : 0u);

        const bool runEntityDraws = !entityKeys.empty();
        const bool hasAnyDraws = runEntityDraws || !depthCommands.empty() || !fallbackHeads.empty();
        if (!hasAnyDraws)
        {
            LogMeshPassDiagnostic(
                "depth", "pass-no-draws", m_Data.Params.ViewId,
                entityKeys.size(), depthCommands.size(),
                m_Services.Streams ? m_Services.Streams->GetRangeCount() : 0u);
            return true;
        }

        m_TrackUnderDraw =
            m_Services.UnderDraw != nullptr && !virtualPage &&
            (m_Data.Params.PassType == DepthPassType::ShadowCascade ||
             m_Data.Params.PassType == DepthPassType::TransmittanceCascade);

        static const bool kShadowWalkEarlyOut = Platform::EnvironmentSwitchEnabled("GE_SHADOW_WALK_EARLYOUT", true);
        const bool isOpaqueShadowSlice =
            m_Data.Params.PassType == DepthPassType::ShadowCascade ||
            m_Data.Params.PassType == DepthPassType::AreaShadow ||
            m_Data.Params.PassType == DepthPassType::SpotShadow ||
            m_Data.Params.PassType == DepthPassType::PointShadow;
        // The survivor stat is produced by the GPU scatter; the compat profile never
        // runs it, so an unguarded early-out would skip every shadow slice forever.
        if (kShadowWalkEarlyOut && !virtualPage && !m_Services.CpuDrawStream && isOpaqueShadowSlice && depthCommands.empty() && m_Services.Streams)
        {
            const uint32_t survivors = m_Services.Streams->PreviousFrameShadowSurvivors(
                static_cast<uint32_t>(m_Data.Params.ViewId), m_CascadeIndex, m_Data.Params.Phase);
            if (survivors == 0u)
            {
                LogMeshPassDiagnostic("depth", "walk-skip-empty-slice", m_Data.Params.ViewId,
                                      entityKeys.size(), depthCommands.size(),
                                      m_Services.Streams->GetRangeCount());
                MarkUnderDraw(DepthUnderDrawReason::EmptySlice);
                return true;
            }
        }

        Rendering::GPUScene* gpuScene = m_Services.Scene;

        m_InstanceAddress =
            (m_Services.Device && gpuScene && gpuScene->GetInstanceBuffer().IsValid())
                ? m_Services.Device->GetBufferDeviceAddress(gpuScene->GetInstanceBuffer())
                : 0ull;

        // Declaration resolves the camera and contributor resources for this pass.
        m_UploadedCompatListBytes =
            UploadedCompatInstanceListBytes(m_Data.Inputs.PassResources);
        m_Data.Inputs.View = m_Data.Params.ViewId;
        m_Data.Inputs.PassKeywords = m_PassKeywords;
        m_Data.Inputs.PassInstanceIndex = passInstanceIndex;
        m_Data.Walked = true;
        m_Data.EntityBatchCount = entityKeys.size();
        m_Data.Consolidated = !m_Services.MeshPoolGroups.empty();
        m_ScatterReady = m_Services.Streams && m_Services.Streams->IntrospectScatter().PipelineReady;

        if (!m_Services.MeshPoolGroups.empty())
            m_DrawnStreamRanges.reserve(entityKeys.size());

        const bool collapseEligibleCasters =
            !m_Services.CpuDrawStream &&
            (m_Data.Params.PassType == DepthPassType::ShadowCascade ||
             m_Data.Params.PassType == DepthPassType::AreaShadow ||
             m_Data.Params.PassType == DepthPassType::SpotShadow ||
             m_Data.Params.PassType == DepthPassType::PointShadow);

        if (runEntityDraws && !collapseEligibleCasters)
        {
            for (const auto& key : entityKeys)
            {
                // The prepass never remaps to sentinels (cascadeIdx None); the
                // TransmittanceCascade walk resolves the class like the cascades.
                const DrawStreamLookupKey lookup = Lookup(
                    key, m_CascadeIndex == Rendering::GPUDrawStreamBuilder::kCascadeIndexNone
                             ? Rendering::MaterialDepthClass::MaterialDependent
                             : m_Services.Materials.GetMaterialDepthClass(key.materialIndex));
                if (!m_Services.MeshPoolGroups.empty() && lookup.meshKey == Rendering::MeshPoolGroupPlan::kAbsentGroup)
                {
                    MarkUnderDraw(DepthUnderDrawReason::MissingGeometryGroup, m_Services.Meshes.Find(key.mesh));
                    continue;
                }
                if (!ClaimStreamRange(lookup))
                    continue;
                PrepareEntityBatch(key, lookup);
            }
        }
        else if (runEntityDraws)
        {
            std::unordered_set<uint64_t> drawnEligible; // (meshIndex << 1) | doubleSidedBit
            for (const auto& key : entityKeys)
            {
                if (m_Data.Params.Casters != CascadeCasters::All)
                {
                    // Consolidated ranges share the mesh stream layout, so
                    // skinned and rigid batches never claim the same range.
                    if (const auto* entry = m_Services.Meshes.Find(key.mesh))
                    {
                        if (Rendering::IsSkinned(entry->vertexFlags) !=
                            (m_Data.Params.Casters == CascadeCasters::Skinned))
                            continue;
                    }
                }
                const Rendering::MaterialDepthClass depthClass = m_Services.Materials.GetMaterialDepthClass(key.materialIndex);
                const DrawStreamLookupKey lookup = Lookup(key, depthClass);
                if (!m_Services.MeshPoolGroups.empty() && lookup.meshKey == Rendering::MeshPoolGroupPlan::kAbsentGroup)
                {
                    MarkUnderDraw(DepthUnderDrawReason::MissingGeometryGroup, m_Services.Meshes.Find(key.mesh));
                    continue;
                }
                if (depthClass == Rendering::MaterialDepthClass::MaterialDependent)
                {
                    if (!ClaimStreamRange(lookup))
                        continue;
                    PrepareEntityBatch(key, lookup);
                    continue;
                }
                const uint64_t doubleSidedBit =
                    (depthClass == Rendering::MaterialDepthClass::EligibleDoubleSided) ? 1u : 0u;
                const uint64_t dedupKey = (static_cast<uint64_t>(key.meshIndex) << 1) | doubleSidedBit;
                if (!drawnEligible.insert(dedupKey).second)
                    continue; // this (mesh, side) class group already drew its sentinel range
                // Under consolidation the walk dedup additionally collapses
                // (sentinel, group) across meshes.
                if (!ClaimStreamRange(lookup))
                    continue;
                PrepareEntityBatch(key, lookup);
            }
        }
        for (const auto& command : depthCommands)
            PrepareContributor(command);
        for (const auto& command : fallbackHeads)
            PrepareContributor(command);

        for (auto& draw : m_Data.Draws)
            draw.Command.Bindings.PushConstants = draw.PushConstantSize == 0
                                                      ? std::span<const std::byte>{}
                                                      : std::span<const std::byte>(m_Data.PushConstants.data() + draw.PushConstantOffset,
                                                                                   draw.PushConstantSize);
        return true;
    }

  private:
    DepthProducerState SeedProducer(const WorldDrawBuilder::ShadowSubmissionState& submissions) const
    {
        const auto& params = m_Data.Params;
        uint64_t content = HashUtils::HashValue(submissions.MaterialContentKey, params.PassType);
        // The page cache checks a virtual page's tile and projection, so the key of a page
        // describes only the drawn content, the same for every page of one pass type.
        if (params.VirtualPageTile)
            content = HashUtils::HashValue(content, true);
        else
            content = HashUtils::HashValue(content, params.CascadeIndex);
        content = HashUtils::HashValue(content, params.DepthBiasEnable);
        content = HashUtils::HashValue(content, params.DepthClampEnable);
        content = HashUtils::HashValue(content, params.RasterizationSamples);
        content = HashUtils::HashValue(content, m_Services.CullMode);
        content = HashUtils::HashValue(content, m_Services.FrontFace);
        return {submissions.MembershipKey, submissions.MembershipCount, content, submissions.Complete};
    }

    struct ResolvedDepthPipeline
    {
        PipelineHandle Pipeline{};
        Rendering::GraphicsPipelineId ConcreteId{};
        const Rendering::ShaderMeta* Metadata = nullptr;
        std::span<const Rendering::DescriptorSetLayoutId> SetLayouts;
        // 0 = derive the count from the variant meta (a fragment stage
        // brings the material's own sets); otherwise the vertex-only
        // pipeline's own set count.
        uint32_t SetCount = 1u;
        std::optional<uint32_t> ServedVersion;
    };

    void MarkUnderDraw(DepthUnderDrawReason reason, const Rendering::MeshGPUEntry* mesh = nullptr)
    {
        m_Data.AllProducer.Complete = false;
        if (!mesh || !Rendering::IsSkinned(mesh->vertexFlags))
            m_Data.RigidProducer.Complete = false;
        if (m_TrackUnderDraw && !m_Data.UnderDrew)
        {
            m_Data.UnderDrew = true;
            m_Data.UnderDrawReason = reason;
        }
    }

    void Append(const DrawCommand& command, const PreparedDrawBindings& bindings,
                const Rendering::MeshGPUEntryBindings& geometry, Rendering::VertexAttributeFlags vertexFlags,
                std::optional<uint32_t> version = {}, uint32_t materialIndex = 0,
                uint32_t diagnosticMaterialIndex = 0, bool entity = false, uint32_t meshIndex = 0,
                uint32_t maxDrawCount = 0, size_t rangeCount = 0)
    {
        const auto bytes = command.Bindings.PushConstants;
        const size_t offset = m_Data.PushConstants.size();
        m_Data.PushConstants.insert(m_Data.PushConstants.end(), bytes.begin(), bytes.end());
        m_Data.Draws.push_back({command, bindings, geometry, vertexFlags, offset, bytes.size(),
                                version, materialIndex, diagnosticMaterialIndex, entity, meshIndex, maxDrawCount, rangeCount});
        uint64_t served = HashUtils::HashValue(0u, static_cast<uint64_t>(bindings.Pipeline));
        served = HashUtils::HashValue(served, vertexFlags);
        served = HashUtils::HashValue(served, materialIndex);
        served = HashUtils::HashValue(served, version.has_value());
        if (version)
            served = HashUtils::HashValue(served, *version);
        m_Data.AllProducer.ServedContentKey =
            HashUtils::HashCombine(m_Data.AllProducer.ServedContentKey, served);
        if (!Rendering::IsSkinned(vertexFlags))
            m_Data.RigidProducer.ServedContentKey =
                HashUtils::HashCombine(m_Data.RigidProducer.ServedContentKey, served);
    }

    void PrepareEntityBatch(const WorldDrawBuilder::BatchKey& key, const DrawStreamLookupKey& lookup)
    {
        const auto* entry = m_Services.Meshes.Find(key.mesh);
        ComposedPCInstanced instanceConstants{};

        const Material* material = key.material;
        if (!material)
        {
            LogMeshDrawSkipDiagnostic(
                "depth", "material-null", m_Data.Params.ViewId,
                key.materialIndex, key.meshIndex,
                m_ScatterReady,
                false, 0ull, m_InstanceAddress,
                m_Services.Device ? m_Services.Device->GetCapabilities().supportsBufferDeviceAddress : false,
                m_Services.Streams ? m_Services.Streams->GetRangeCount() : 0u);
            MarkUnderDraw(DepthUnderDrawReason::MaterialPending, entry);
            return;
        }
        if (!entry)
        {
            LogMeshDrawSkipDiagnostic(
                "depth", "mesh-entry-missing", m_Data.Params.ViewId,
                key.materialIndex, key.meshIndex,
                m_ScatterReady,
                false, 0ull, m_InstanceAddress,
                m_Services.Device ? m_Services.Device->GetCapabilities().supportsBufferDeviceAddress : false,
                m_Services.Streams ? m_Services.Streams->GetRangeCount() : 0u);
            MarkUnderDraw(DepthUnderDrawReason::MeshPending, entry);
            return;
        }
        Rendering::MeshGPUEntryBindings entryBindings{};
        const bool drawable = m_Services.Meshes.TryGetDrawableBindings(*entry, entryBindings);
        if (!drawable || entry->indexCount == 0)
        {
            LogMeshDrawSkipDiagnostic(
                "depth", !drawable ? kMeshNotDrawableReason : "index-count-zero",
                m_Data.Params.ViewId, key.materialIndex, key.meshIndex,
                m_ScatterReady,
                false, 0ull, m_InstanceAddress,
                m_Services.Device ? m_Services.Device->GetCapabilities().supportsBufferDeviceAddress : false,
                m_Services.Streams ? m_Services.Streams->GetRangeCount() : 0u);
            MarkUnderDraw(DepthUnderDrawReason::MeshNotDrawable, entry);
            return;
        }
        const DepthPassMaterialFilter filter = FilterDepthPassMaterial(
            m_Data.Params.PassType, material->GetAlphaMode(),
            Rendering::HasKeyword(material->GetVariantKey().materialKeywords,
                                  Rendering::MaterialKeyword::Transmission));
        if (filter != DepthPassMaterialFilter::Draws)
        {
            LogMeshDrawSkipDiagnostic(
                "depth",
                filter == DepthPassMaterialFilter::AlphaBlend ? "alpha-blend-filter"
                                                              : "transmission-depth-filter",
                m_Data.Params.ViewId, key.materialIndex, key.meshIndex,
                m_ScatterReady,
                false, 0ull, m_InstanceAddress,
                m_Services.Device ? m_Services.Device->GetCapabilities().supportsBufferDeviceAddress : false,
                m_Services.Streams ? m_Services.Streams->GetRangeCount() : 0u);
            return;
        }

        const auto depthVertexFlags = EffectiveVertexFlagsForDepthPass(*entry, entryBindings);
        const bool isSkinned8Depth =
            Rendering::HasFlag(depthVertexFlags,
                               Rendering::VertexAttributeFlags::HasJoints1) ||
            Rendering::HasFlag(depthVertexFlags,
                               Rendering::VertexAttributeFlags::HasWeights1);
        const bool sharedDepthOffered =
            !m_Services.CpuDrawStream &&
            (m_Data.Params.PassType == DepthPassType::ShadowCascade ||
             m_Data.Params.PassType == DepthPassType::AreaShadow ||
             m_Data.Params.PassType == DepthPassType::SpotShadow ||
             m_Data.Params.PassType == DepthPassType::PointShadow ||
             (m_Data.Params.PassType == DepthPassType::Prepass && PrepassSharedDepthEnabled() &&
              !isSkinned8Depth));
        const bool vertexModified =
            Rendering::HasKeyword(material->GetVariantKey().materialKeywords,
                                  Rendering::MaterialKeyword::HasVertexMod) ||
            Rendering::HasKeyword(material->GetVariantKey().materialKeywords,
                                  Rendering::MaterialKeyword::HasVertexOutputMod);
        const bool needsTextures = material->GetAlphaMode() == MaterialAlphaMode::Mask || vertexModified;
        const bool texturesReady = !needsTextures ||
            (m_Services.Textures && m_Services.Textures->IsMaterialTextureBindingComplete(material->GetGuid()));
        if (!texturesReady)
            MarkUnderDraw(DepthUnderDrawReason::TexturesPending, entry);
        const DepthSegmentPipelineChoice head = ChooseDepthHeadPipeline(
            m_PassKeywords, m_Data.Params.PassType,
            material->GetAlphaMode() == MaterialAlphaMode::Mask, vertexModified,
            sharedDepthOffered, WritesReliefDepth(material->GetVariantKey().materialKeywords));
        const Rendering::VertexAttributeFlags drawVertexFlags = DepthHeadVertexFlags(
            depthVertexFlags,
            Rendering::BoundStreamVertexFlags(*entry, entryBindings, material->IgnoresVertexColor()),
            vertexModified,
            DepthPassReadsVertexColor(m_Data.Params.PassType, material->GetAlphaMode()),
            head.Keywords);
        const Rendering::CullModeFlags depthCullMode = material->IsDoubleSided()
                                                           ? static_cast<Rendering::CullModeFlags>(Rendering::CullModeFlagBits::None)
                                                           : m_Services.CullMode;
        const bool isSkinnedDepth = Rendering::HasFlag(
            depthVertexFlags,
            Rendering::VertexAttributeFlags::HasJoints | Rendering::VertexAttributeFlags::HasWeights);

        entryBindings.indexType = entry->indexType;
        const DepthBatchPreparation batch{key, material, entry, entryBindings, depthVertexFlags,
                                          drawVertexFlags, head, depthCullMode, isSkinnedDepth, texturesReady};

        if (m_Services.CpuDrawStream)
        {
            const bool shadowFamily = m_Data.Params.PassType == DepthPassType::ShadowCascade || m_Data.Params.PassType == DepthPassType::AreaShadow || m_Data.Params.PassType == DepthPassType::SpotShadow || m_Data.Params.PassType == DepthPassType::PointShadow;
            const CpuDrawStreamBuilder::InstanceList instances =
                m_Services.CpuDrawStream->GetInstances(
                    m_Data.Params.ViewId, key.materialIndex, key.meshIndex,
                    shadowFamily ? CpuDrawStreamBuilder::InstanceSet::ShadowCasters
                                 : CpuDrawStreamBuilder::InstanceSet::Camera);
            if (instances.Count == 0u)
                return;
            assert(m_UploadedCompatListBytes ==
                       m_Services.CpuDrawStream->GetIndexList(m_Data.Params.ViewId).size_bytes() &&
                   "the view's instance list was rebuilt after its first pass declared");

            const ResolvedDepthPipeline resolved = ResolvePipeline(batch, m_Services.FrontFace,
                                                                   /*crossfading=*/false);
            if (!resolved.Pipeline.IsValid())
            {
                LogMeshDrawSkipDiagnostic(
                    "depth", "depth-pipeline-invalid", m_Data.Params.ViewId,
                    key.materialIndex, key.meshIndex, false, false, 0ull,
                    m_InstanceAddress, false, 0u);
                MarkUnderDraw(DepthUnderDrawReason::DepthVariantPending, entry);
                return;
            }

            const uint32_t compatListFirst = instances.First;
            Engine::Renderer::DrawBindings compatBindings{};
            compatBindings.PushConstants = std::span<const std::byte>(
                reinterpret_cast<const std::byte*>(&compatListFirst),
                sizeof(compatListFirst));

            const auto preparedBindings = m_Services.Materials.Binder().PrepareMaterialForDraw(
                m_Data.Inputs, *material, resolved.Pipeline, resolved.Metadata, compatBindings, resolved.SetCount, resolved.SetLayouts);
            if (!preparedBindings.Pipeline.IsValid())
            {
                LogMeshDrawSkipDiagnostic(
                    "depth", "material-bind-invalid", m_Data.Params.ViewId,
                    key.materialIndex, key.meshIndex, false, false, 0ull,
                    m_InstanceAddress, false, 0u);
                MarkUnderDraw(DepthUnderDrawReason::BindPending, entry);
                return;
            }

            DrawCommand command{};
            command.Bindings = compatBindings;
            command.IndexCount = entry->indexCount;
            command.InstanceCount = instances.Count;
            command.FirstIndex = entry->firstIndex;
            command.VertexOffset = static_cast<int32_t>(entry->vertexOffset);
            entryBindings.indexType = entry->indexType;
            Append(command, preparedBindings, entryBindings, drawVertexFlags, resolved.ServedVersion,
                   material->GetGpuSceneMaterialIndex(), key.materialIndex, true, key.meshIndex, 1u, 0u);
            return;
        }

        if (!m_ScatterReady)
        {
            LogMeshDrawSkipDiagnostic(
                "depth", "drawstream-pipeline-invalid", m_Data.Params.ViewId,
                key.materialIndex, key.meshIndex,
                false, false, 0ull, m_InstanceAddress,
                m_Services.Device ? m_Services.Device->GetCapabilities().supportsBufferDeviceAddress : false,
                m_Services.Streams ? m_Services.Streams->GetRangeCount() : 0u);
            MarkUnderDraw(DepthUnderDrawReason::ScatterPending, entry);
            return;
        }
        const Rendering::GPUDrawStreamBuilder::BatchDrawRange drawRange =
            m_Services.Streams->FindBatchDrawRange(
                static_cast<uint32_t>(m_Data.Params.ViewId), m_CascadeIndex,
                lookup.classKey, lookup.meshKey, m_Data.Params.Phase);
        if (!drawRange.IsValid() || !m_Services.Device)
        {
            LogMeshDrawSkipDiagnostic(
                "depth", !m_Services.Device ? "device-null" : "draw-range-invalid", m_Data.Params.ViewId,
                key.materialIndex, key.meshIndex,
                true, drawRange.IsValid(), 0ull, m_InstanceAddress,
                m_Services.Device ? m_Services.Device->GetCapabilities().supportsBufferDeviceAddress : false,
                m_Services.Streams->GetRangeCount());
            MarkUnderDraw(DepthUnderDrawReason::DrawRangeInvalid, entry);
            return;
        }
        // The range's records name their instance words by firstInstance in the range's own
        // list buffer, or in the shared indirection buffer when the range names none.
        const uint64_t slotIndirectionBda = drawRange.indirectionAddress != 0
            ? drawRange.indirectionAddress
            : m_Services.Streams->GetSharedIndirectionAddress();
        if (slotIndirectionBda == 0 || m_InstanceAddress == 0)
        {
            LogMeshDrawSkipDiagnostic(
                "depth", slotIndirectionBda == 0 ? "indirection-bda-zero" : "instance-bda-zero",
                m_Data.Params.ViewId, key.materialIndex, key.meshIndex,
                true, true, slotIndirectionBda, m_InstanceAddress,
                m_Services.Device->GetCapabilities().supportsBufferDeviceAddress,
                m_Services.Streams->GetRangeCount());
            MarkUnderDraw(DepthUnderDrawReason::AddressMissing, entry);
            return;
        }

        instanceConstants.indirectionAddr = slotIndirectionBda;
        instanceConstants.gpuInstancesAddr = m_InstanceAddress;
        instanceConstants.materialOverride = DrawStreamRangeMaterial(key, lookup);
        std::span<const std::byte> constantBytes(
            reinterpret_cast<const std::byte*>(&instanceConstants), sizeof(instanceConstants));

        Engine::Renderer::DrawBindings drawBindings{};
        drawBindings.PushConstants = constantBytes;

        // Draw one parity segment with the depth pipeline compiled for its
        // winding. The mesh registry's index buffer + push constants are shared;
        // only the pipeline (front face) and the indirect offsets differ.

        const Rendering::FrontFace baseWinding = m_Services.FrontFace;
        const bool withoutHexTiling = CanSkipDepthHexTiling(batch, lookup);
        for (const auto& drawSegment : drawRange.Segments())
        {
            assert(!drawSegment.crossfading || DepthPassDrawsCrossfadeTails(m_Data.Params.PassType));
            PrepareSegment(batch, drawRange, drawBindings, slotIndirectionBda, drawSegment.mirrored ? Rendering::FlipWinding(baseWinding) : baseWinding,
                           drawSegment.crossfading, withoutHexTiling, drawSegment.segment);
        }
    }

    bool CanSkipDepthHexTiling(const DepthBatchPreparation& batch, const DrawStreamLookupKey& lookup) const
    {
        const auto pass = m_Data.Params.PassType;
        const auto& material = *batch.Material;
        const auto& spec = material.GetCompileSpec();
        const uint32_t row = material.GetGpuSceneMaterialIndex();
        if ((pass != DepthPassType::Prepass && pass != DepthPassType::ShadowCascade) ||
            m_Data.Params.VirtualPageTile ||
            m_Services.Device->GetAPI() != Rendering::GraphicsAPI::Vulkan ||
            material.GetAlphaMode() != MaterialAlphaMode::Mask ||
            spec.surfaceShaderPath != "Surfaces/standard_pbr.glsl" || spec.parallax ||
            spec.customVertexShader || !spec.vertexModifierPath.empty() || !spec.userKeywords.empty() ||
            row != batch.Key.materialIndex)
            return false;

        if (lookup.classKey != row)
            return false;

        if (!m_Services.Materials.IsCurrentPackedMaterialRow(material, BoundMaterialParams(m_Data.Inputs)))
            return false;

        static const auto* const kHexTiling = Rendering::FindLegacyMaterialLane("hexTiling");
        if (!kHexTiling)
            return false;
        const auto offset = Rendering::LegacyLaneByteOffset(*kHexTiling);
        float hexTiling;
        if (material.GetCacheSize() < offset + sizeof(hexTiling))
            return false;
        std::memcpy(&hexTiling, material.GetCacheData() + offset, sizeof(hexTiling));
        return hexTiling < 0.5f;
    }

    ResolvedDepthPipeline ResolvePipeline(const DepthBatchPreparation& batch, Rendering::FrontFace winding,
                                          bool crossfading, bool withoutHexTiling = false)
    {
        ResolvedDepthPipeline resolved{};
        const DepthSegmentPipelineChoice choice = ChooseDepthSegmentPipeline(
            batch.Head.Keywords, batch.Head.UseSharedDepth, batch.Head.ComposesFragment, crossfading);
        if (choice.UseSharedDepth)
        {
            const auto sharedId = m_Services.Materials.Variants().GetSharedDepthPipelineId(
                batch.DepthVertexFlags, batch.Mesh->topology, batch.CullMode, m_Data.Params.DepthBiasEnable,
                m_Data.Params.DepthClampEnable, winding);
            if (sharedId.IsValid())
            {
                resolved.Pipeline = m_Context.PreparePipeline(sharedId);
                resolved.ConcreteId = m_Context.ResolveGraphicsPipelineId(sharedId);
                resolved.Metadata = m_Services.Materials.Variants().GetSharedDepthShaderMeta(batch.IsSkinned);
                resolved.SetLayouts = m_Services.Materials.Variants().GetSharedDepthSetLayouts(batch.IsSkinned);
                return resolved;
            }
        }
        const auto [pipeline, variantEntry, concreteId] = m_Services.Materials.Variants().PrepareDepthInstancedVariant(
            *batch.Material, batch.DrawVertexFlags, batch.Mesh->topology, choice.Keywords,
            m_Data.Params.RasterizationSamples, m_Data.Params.DepthBiasEnable, m_Data.Params.DepthClampEnable,
            winding, m_Context, withoutHexTiling);
        resolved.Pipeline = pipeline;
        resolved.ConcreteId = concreteId;
        if (variantEntry)
        {
            resolved.Metadata = variantEntry->VariantMeta.get();
            resolved.SetLayouts = variantEntry->SetLayouts;
            resolved.ServedVersion = variantEntry->MaterialVersion;
        }
        resolved.SetCount = choice.ComposesFragment
                                ? 0u
                                : std::max<uint32_t>(1u, variantEntry ? static_cast<uint32_t>(variantEntry->SetLayouts.size()) : 1u);
        return resolved;
    }

    void PrepareSegment(const DepthBatchPreparation& batch,
                        const Rendering::GPUDrawStreamBuilder::BatchDrawRange& drawRange,
                        const DrawBindings& drawBindings, uint64_t slotIndirectionBda,
                        Rendering::FrontFace winding, bool crossfading, bool withoutHexTiling,
                        const Rendering::GPUDrawStreamBuilder::BatchDrawSegment& segment)
    {
        const size_t commandOffset = segment.cmdByteOffset;
        const size_t countOffset = segment.countByteOffset;
        const uint32_t maxDrawCount = segment.maxDrawCount;
        const ResolvedDepthPipeline resolved = ResolvePipeline(batch, winding, crossfading, withoutHexTiling);
        if (!resolved.Pipeline.IsValid())
        {
            LogMeshDrawSkipDiagnostic(
                "depth", "depth-pipeline-invalid", m_Data.Params.ViewId,
                batch.Key.materialIndex, batch.Key.meshIndex,
                true, true, slotIndirectionBda, m_InstanceAddress,
                m_Services.Device->GetCapabilities().supportsBufferDeviceAddress,
                m_Services.Streams->GetRangeCount());
            MarkUnderDraw(DepthUnderDrawReason::DepthVariantPending, batch.Mesh);
            return;
        }
        const auto preparedBindings = m_Services.Materials.Binder().PrepareMaterialForDraw(
            m_Data.Inputs, *batch.Material, resolved.Pipeline, resolved.Metadata, drawBindings, resolved.SetCount, resolved.SetLayouts);
        if (!preparedBindings.Pipeline.IsValid())
        {
            LogMeshDrawSkipDiagnostic(
                "depth", "material-bind-invalid", m_Data.Params.ViewId,
                batch.Key.materialIndex, batch.Key.meshIndex,
                true, true, slotIndirectionBda, m_InstanceAddress,
                m_Services.Device->GetCapabilities().supportsBufferDeviceAddress,
                m_Services.Streams->GetRangeCount());
            MarkUnderDraw(DepthUnderDrawReason::BindPending, batch.Mesh);
            return;
        }
        DrawCommand command{};
        command.Bindings = drawBindings;
        command.UseIndirect = true;
        command.IndirectCommandBuffer = drawRange.recordBuffer;
        command.IndirectCountBuffer = drawRange.countBuffer;
        command.IndirectMaxDrawCount = maxDrawCount;
        command.IndirectStride = 5u * sizeof(uint32_t);
        command.IndirectCommandOffset = commandOffset;
        command.IndirectCountOffset = countOffset;
        Append(command, preparedBindings, batch.Geometry, batch.DrawVertexFlags, resolved.ServedVersion,
               batch.Material->GetGpuSceneMaterialIndex(), batch.Key.materialIndex, true, batch.Key.meshIndex, maxDrawCount,
               m_Services.Streams->GetRangeCount());
        m_Data.Draws.back().ConcretePipeline = resolved.ConcreteId;
        if (IndexedDepthReuseEnabled(*m_Services.Device) && batch.TexturesReady &&
            m_Data.Params.PassType == DepthPassType::Prepass &&
            m_Data.Params.Phase == Rendering::GPUDrawStreamBuilder::SlicePhase::A &&
            m_Data.Params.Heads == PrepassHeads::Occluding &&
            batch.Material->GetAlphaMode() == MaterialAlphaMode::Mask &&
            batch.Key.colorClassId == batch.Key.materialIndex &&
            batch.Key.materialIndex == batch.Material->GetGpuSceneMaterialIndex() &&
            resolved.ServedVersion == batch.Material->GetVersion() && m_Services.Textures &&
            resolved.ConcreteId.IsValid() &&
            HasPublishedReplayAlbedo(*batch.Material, *m_Services.Textures, preparedBindings, m_Context) &&
            m_Services.Materials.IsCurrentPackedMaterialRow(*batch.Material, BoundMaterialParams(m_Data.Inputs)))
        {
            const auto vertexAlpha = StockMaskVertexAlpha(resolved.Metadata, true, crossfading);
            if (auto source = GetIndexedDepthDraw(command, batch.Geometry);
                source && vertexAlpha && HasBoundIndexedMaskSets(*resolved.Metadata, preparedBindings))
                m_Data.IndexedSources.push_back(
                    {*source, batch.Material, batch.Material->GetContentRevision(), winding, *vertexAlpha, crossfading,
                     resolved.ConcreteId, m_Data.Draws.size() - 1,
                     ChooseDepthSegmentPipeline(batch.Head.Keywords, batch.Head.UseSharedDepth,
                         batch.Head.ComposesFragment, crossfading).Keywords |
                         Rendering::MaterialKeyword::IndexedDepthOwner});
        }
    }

    void PrepareContributor(const DrawCommand& command)
    {
        if (m_Services.Meshes.IsBufferInStorageDomain(command.Geometry.AltGeom.VB, Rendering::MeshGPUStorageDomain::ClusterFine) ||
            m_Services.Meshes.IsBufferInStorageDomain(command.Geometry.AltGeom.IB, Rendering::MeshGPUStorageDomain::ClusterFine))
            return;
        Rendering::PipelineHandle pipeline{};
        if (command.InternedPipeline.IsValid())
            pipeline = m_Context.PreparePipeline(command.InternedPipeline);
        if (!pipeline.IsValid())
            return;
        PreparedDrawBindings bindings;
        if (command.Material)
            bindings = m_Services.Materials.Binder().PrepareMaterialForDraw(m_Data.Inputs, *command.Material, pipeline, command.PipelineMeta,
                                                                            command.Bindings, command.PipelineSetCount);
        else if (command.PipelineMeta)
            bindings = m_Services.Materials.Binder().PreparePipelineForDraw(m_Data.Inputs, pipeline, *command.PipelineMeta,
                                                                            command.Bindings, command.PipelineSetCount);
        if (!bindings.Pipeline.IsValid())
            return;
        Rendering::MeshGPUEntryBindings geometry{};
        geometry.coreVB = command.Geometry.AltGeom.VB;
        geometry.indexBuffer = command.Geometry.AltGeom.IB;
        geometry.indexType = static_cast<uint32_t>(command.Geometry.AltGeom.IndexType);
        Append(command, bindings, geometry, Rendering::VertexAttributeFlags::None);
        m_Data.Draws.back().ConcretePipeline = m_Context.ResolveGraphicsPipelineId(command.InternedPipeline);
    }

    DrawStreamLookupKey Lookup(const WorldDrawBuilder::BatchKey& key,
                               Rendering::MaterialDepthClass depthClass) const
    {
        return ResolveDrawStreamLookupKey(key, m_CascadeIndex, depthClass, m_Services.MeshPoolGroups);
    }

    bool ClaimStreamRange(const DrawStreamLookupKey& lookup)
    {
        if (m_Services.MeshPoolGroups.empty())
            return true; // per-bucket path: keys are already unique per range
        const uint64_t drawKey = (static_cast<uint64_t>(lookup.classKey) << 24) | lookup.meshKey;
        return m_DrawnStreamRanges.insert(drawKey).second;
    }

    RenderGraph::RGPrepareContext& m_Context;
    PreparedDepthOnlyPass& m_Data;
    const DepthDrawServices& m_Services;
    MaterialKeyword m_PassKeywords = MaterialKeyword::None;
    uint8_t m_CascadeIndex = Rendering::GPUDrawStreamBuilder::kCascadeIndexNone;
    bool m_TrackUnderDraw = false;
    uint64_t m_InstanceAddress = 0;
    uint64_t m_UploadedCompatListBytes = 0;
    bool m_ScatterReady = false;
    std::unordered_set<uint64_t> m_DrawnStreamRanges;
};
} // namespace

const char* DepthUnderDrawReasonName(DepthUnderDrawReason reason)
{
    switch (reason)
    {
    case DepthUnderDrawReason::EmptySlice: return "empty-slice";
    case DepthUnderDrawReason::MissingGeometryGroup: return "geometry-group";
    case DepthUnderDrawReason::MaterialPending: return "material";
    case DepthUnderDrawReason::MeshPending: return "mesh";
    case DepthUnderDrawReason::MeshNotDrawable: return "mesh-buffers";
    case DepthUnderDrawReason::TexturesPending: return "textures";
    case DepthUnderDrawReason::DepthVariantPending: return "depth-variant";
    case DepthUnderDrawReason::BindPending: return "bind";
    case DepthUnderDrawReason::ScatterPending: return "scatter";
    case DepthUnderDrawReason::DrawRangeInvalid: return "draw-range";
    case DepthUnderDrawReason::AddressMissing: return "address";
    case DepthUnderDrawReason::Count: break;
    }
    return "unknown";
}

std::string FormatDepthUnderDrawReasons(const DepthUnderDrawReasonCounts& counts)
{
    std::string text;
    for (size_t reason = 0; reason < counts.size(); ++reason)
    {
        if (counts[reason] == 0)
            continue;
        if (!text.empty())
            text += ' ';
        text += std::format("{} {}", DepthUnderDrawReasonName(static_cast<DepthUnderDrawReason>(reason)),
                            counts[reason]);
    }
    return text.empty() ? std::string("none") : text;
}

bool PrepareDepthOnlyPass(RenderGraph::RGPrepareContext& context,
                          PreparedDepthOnlyPass& data, const DepthDrawServices& services)
{
    return DepthPassPreparation(context, data, services).Prepare();
}

bool HasMatchingIndexedDepthViewParams(const PreparedDepthOnlyPass& pass, const PassBindingContext& color)
{
    constexpr auto kViewParams = HashStringId("ViewParams");
    const auto* left = FindReplayPassBuffer(pass.Inputs, kViewParams);
    const auto* right = FindReplayPassBuffer(color, kViewParams);
    return left && right && left->Buffer.IsValid() && left->Buffer == right->Buffer &&
           left->Offset == right->Offset && left->Range == right->Range;
}

Rendering::GraphicsPipelineId FindPreparedIndexedDepthPipeline(
    const PreparedDepthOnlyPass& pass, const IndexedDepthDraw& source, const Material& material,
    Rendering::FrontFace winding, bool vertexAlpha, const RenderGraph::RGPrepareContext& context)
{
    if (!context.IsFor(pass.IndexedFrame))
        return {};
    const auto found = std::find_if(pass.IndexedSources.begin(), pass.IndexedSources.end(), [&](const auto& entry) {
        return entry.MaterialOwner == &material && entry.MaterialRevision == material.GetContentRevision() &&
               entry.Winding == winding && entry.VertexAlpha == vertexAlpha && entry.Draw == source;
    });
    return found == pass.IndexedSources.end() ? Rendering::GraphicsPipelineId{} : found->Pipeline;
}

DepthProducerState GetPreparedDepthProducerState(const PreparedDepthOnlyPass& data,
                                                CascadeCasters casters)
{
    if (casters == CascadeCasters::Rigid)
        return data.RigidProducer;
    return casters == CascadeCasters::All ? data.AllProducer : DepthProducerState{};
}

namespace
{
constexpr uint32_t kOwnerMaxDraws = 65535u; // Vulkan minimum indirect compute group capacity.
constexpr size_t kOwnerReplayDrawStride = 5u * sizeof(uint32_t);

struct OwnerComputeProgram
{
    ComputePipelineId Id;
    DescriptorSetLayoutDesc Layout;
};

DescriptorWrite OwnerStorage(uint32_t binding, BufferHandle buffer, size_t bytes)
{
    return {.Binding = binding, .Type = DescriptorType::StorageBuffer, .Buffer = buffer, .Range = bytes};
}
} // namespace

using OwnerReplayShaderBytes = std::array<std::shared_ptr<const std::vector<uint8_t>>, 3>;

static OwnerReplayShaderBytes LoadOwnerReplayModules(const std::array<const char*, 3>& packages)
{
    OwnerReplayShaderBytes binaries{};
    for (size_t index = 0; index < packages.size(); ++index)
    {
        ShaderPackage package{};
        if (!LoadShaderPkg(packages[index], ShaderSourceKind::SpirV, package))
            continue;
        auto stage = package.stageBytes.find("cs");
        if (stage != package.stageBytes.end() && !stage->second.empty())
            binaries[index] = std::make_shared<const std::vector<uint8_t>>(std::move(stage->second));
    }
    return binaries;
}

static const OwnerReplayShaderBytes& OwnerReplayModules(
    bool mesh, const std::array<const char*, 3>& rangePackages, const std::array<const char*, 3>& meshPackages)
{
    if (mesh)
    {
        static const auto s_MeshModules = LoadOwnerReplayModules(meshPackages);
        return s_MeshModules;
    }
    static const auto s_RangeModules = LoadOwnerReplayModules(rangePackages);
    return s_RangeModules;
}

bool PrepareIndexedDepthOwners(RenderGraph::RGPrepareContext& context, PreparedDepthOnlyPass& data, size_t count)
{
    if (!data.Materials || !context.IsFor(data.IndexedFrame))
        return true;

    // Draw the prefix, owner and suffix in the original order. Pruning also
    // requires that no later draw can make a hidden fragment visible by lowering depth.
    auto& device = *context.GetDevice();
    size_t monotonicBegin = data.Draws.size();
    while (monotonicBegin != 0)
    {
        const auto& draw = data.Draws[monotonicBegin - 1];
        GraphicsPipelineDesc later;
        if (!draw.ConcretePipeline.IsValid() || !device.CopyGraphicsPipeline(draw.ConcretePipeline, later))
            break;
        const auto& depth = later.DepthStencil;
        if (depth.depthWriteEnable &&
            (!depth.depthTestEnable ||
             (depth.depthCompareOp != ReverseZ::kNearerOrEqual &&
              depth.depthCompareOp != CompareOp::Greater &&
              depth.depthCompareOp != CompareOp::Equal &&
              depth.depthCompareOp != CompareOp::Never)))
            break;
        --monotonicBegin;
    }
    struct Candidate
    {
        const PreparedDepthOnlyPass::IndexedSource* Source = nullptr;
        GraphicsPipelineDesc Pipeline;
    };
    std::array<Candidate, 2> ranked{};
    for (const auto& source : data.IndexedSources)
    {
        if (source.DrawIndex < monotonicBegin || source.Draw.MaxDrawCount < 512u ||
            source.Draw.MaxDrawCount > kOwnerMaxDraws)
            continue;
        auto position = std::find_if(ranked.begin(), ranked.begin() + count, [&](const Candidate& candidate) {
            return !candidate.Source || source.Draw.MaxDrawCount > candidate.Source->Draw.MaxDrawCount;
        });
        if (position == ranked.begin() + count)
            continue;

        GraphicsPipelineDesc desc;
        if (!device.CopyGraphicsPipeline(source.Pipeline, desc) || !desc.VertexShader || !desc.PixelShader ||
            desc.Topology != PrimitiveTopology::TriangleList ||
            !desc.DepthStencil.depthTestEnable || !desc.DepthStencil.depthWriteEnable ||
            desc.DepthStencil.depthCompareOp != ReverseZ::kNearerOrEqual ||
            desc.DepthStencil.stencilTestEnable || desc.DepthStencil.depthBoundsTestEnable ||
            desc.Rasterization.depthBiasEnable || desc.ColorBlend.alphaToCoverageEnable ||
            desc.ColorBlend.logicOpEnable)
            continue;

        std::move_backward(position, ranked.begin() + count - 1u, ranked.begin() + count);
        *position = {&source, std::move(desc)};
    }

    const auto preparePacket = [&](Candidate& candidate, PreparedDepthOnlyPass::OwnerPacket& packet)
    {
        const auto* selected = candidate.Source;
        if (!selected)
            return;
        auto& baseline = candidate.Pipeline;
        const auto& material = *selected->MaterialOwner;
        const auto& draw = data.Draws[selected->DrawIndex];
        if (selected->MaterialRevision != material.GetContentRevision() ||
            draw.MaterialIndex != material.GetGpuSceneMaterialIndex())
            return;
        const auto* owner = data.Materials->Variants().FindOrRequestPrepassVariant(
            material, draw.VertexFlags, baseline.Topology, selected->OwnerKeywords, selected->Winding);
        if (!owner || owner->MaterialVersion != material.GetVersion() || !owner->VariantMeta)
            return;
        const auto& defines = owner->VariantMeta->RequiredDefines;
        if (std::find(defines.begin(), defines.end(), "GE_INDEXED_DEPTH_OWNER") == defines.end() ||
            StockMaskVertexAlpha(owner->VariantMeta.get(), true, selected->Crossfading) !=
                std::optional{selected->VertexAlpha})
            return;

        GraphicsPipelineDesc variant;
        if (!device.CopyGraphicsPipeline(owner->PipelineId, variant) || !variant.VertexShader || !variant.PixelShader ||
            variant.DescriptorSetLayouts != baseline.DescriptorSetLayouts ||
            variant.PushConstants != baseline.PushConstants)
            return;

        // Inherit the actually served depth donor's raster state and constants.
        baseline.VertexShader = variant.VertexShader;
        baseline.PixelShader = variant.PixelShader;
        baseline.ColorBlend.attachments.assign(1, ColorBlendAttachmentState{.colorWriteMask = 1u});
        constexpr uint32_t kOwnerMeshEncodingConstant = 307;
        const auto& constants = owner->VariantMeta->SpecConstants;
        if (std::none_of(constants.begin(), constants.end(), [](const auto& constant) {
                return constant.Id == kOwnerMeshEncodingConstant;
            }))
            return;
        if (!baseline.Specialization)
            baseline.Specialization.emplace();
        baseline.Specialization->AddConstant(kOwnerMeshEncodingConstant, packet.UsesMeshReplay ? 1u : 0u);
        const auto materialRow = device.GetCapabilities().prefersForwardShaderSpecialization
            ? std::optional{draw.MaterialIndex} : std::nullopt;
        if (!BindIndexedOwnerMaterialRow(baseline, *owner->VariantMeta, materialRow))
            return;
        baseline.DebugName = "IndexedDepthOwner";
        const auto id = device.InternGraphicsPipeline(baseline);
        if (id.IsValid() && context.ResolveGraphicsPipelineId(id) == id)
        {
            const auto pipeline = context.PreparePipeline(id);
            if (pipeline.IsValid())
            {
                packet.DrawIndex = selected->DrawIndex;
                packet.Pipeline = pipeline;
            }
        }
    };
    if (count == 2u && ranked[0].Source && ranked[1].Source &&
        ranked[0].Source->DrawIndex > ranked[1].Source->DrawIndex)
        std::swap(ranked[0], ranked[1]);

    preparePacket(ranked[0], data.Owners[0]);
    if (count == 2u)
    {
        const size_t slot = data.Owners[0].Pipeline.IsValid() ? 1u : 0u;
        preparePacket(ranked[1], data.Owners[slot]);
    }
    return true;
}

void DeclareIndexedOwnerCompaction(RenderGraph::RGFrame& frame, IDevice& device,
    PreparedDepthOnlyPass& data, const GPUDrawStreamBuilder& streams, RenderGraph::RGTexture owners,
    RenderGraph::RGBuffer ordering, size_t slot)
{
    auto& owner = data.Owners[slot];
    static const bool s_MeshReplayEnabled = Platform::EnvironmentSwitchEnabled("GE_INDEXED_DEPTH_MESH_REPLAY", true);
    const bool useMesh = slot == 0u && s_MeshReplayEnabled && device.GetCapabilities().supportsIndexedDepthMeshReplay;
    owner.UsesMeshReplay = useMesh;
    constexpr uint32_t shift = 1u; // 64 triangles from two adjacent 32-triangle owner blocks.
    if (!owners.IsValid() || !device.GetCapabilities().supportsDrawIndirectCountNative ||
        !streams.GetSharedIndirectionAddress())
        return;

    struct BlockPush
    {
        uint32_t RecordBase;
        uint32_t CursorIndex;
        uint32_t MaxDrawCount;
        uint32_t CollectStats;
        uint32_t MergeShift;
        uint32_t Emit;
    };
    struct Prepared
    {
        PreparedOwnerCompaction* Match;
        std::array<PipelineHandle, 3> Pipelines{};
        std::array<DescriptorSetHandle, 3> Sets{};
        BlockPush Constants{};
        BufferHandle Bitset{};
    };
    const size_t stateBytes = (useMesh ? 17u : 14u) * sizeof(uint32_t);
    const size_t drawBytes = kOwnerMaxDraws * (useMesh ? 4u * sizeof(uint32_t) : kOwnerReplayDrawStride);
    constexpr size_t offsetBytes = kOwnerMaxDraws * sizeof(uint32_t);
    const size_t maskBytes = offsetBytes * (useMesh ? 256u : (32u >> shift));
    const auto records = streams.GetSharedRecordBuffer();
    const auto cursors = streams.GetSharedCursorBuffer();
    const uint32_t recordCapacity = streams.GetSharedRecordCapacity();
    const uint32_t cursorCapacity = streams.GetSharedCursorCapacity();
    const uint64_t sourceRemap = streams.GetSharedIndirectionAddress();
    if (!records.IsValid() || !cursors.IsValid() || recordCapacity == 0 || cursorCapacity == 0)
        return;

    constexpr std::array kRangePrograms{
        "Shaders/indexed_depth_replay_classify.shaderpkg",
        "Shaders/indexed_depth_replay_ranges.shaderpkg",
        "Shaders/indexed_depth_replay_offsets.shaderpkg"};
    constexpr std::array kMeshPrograms{
        "Shaders/indexed_depth_replay_mesh_classify.shaderpkg",
        "Shaders/indexed_depth_replay_mesh_ranges.shaderpkg",
        "Shaders/indexed_depth_replay_mesh_offsets.shaderpkg"};
    const auto& shaderModules = OwnerReplayModules(useMesh, kRangePrograms, kMeshPrograms);
    std::array<OwnerComputeProgram, 3> programs{};
    static constexpr std::array programNames{"IndexedOwner.ClassifyBlocks", "IndexedOwner.Ranges", "IndexedOwner.RangeOffsets"};
    constexpr std::array bindingCounts{4u, 5u, 3u};
    for (uint32_t i = 0; i < programs.size(); ++i)
    {
        if (!shaderModules[i])
            return;
        auto& program = programs[i];
        for (uint32_t slot = 0; slot < bindingCounts[i]; ++slot)
        {
            const bool image = i == 0 && slot == 0;
            DescriptorBinding binding{slot, image ? DescriptorType::StorageImage : DescriptorType::StorageBuffer,
                                       1, kShaderStageCompute};
            if (image)
            {
                binding.storageTexelFormat = static_cast<uint32_t>(TextureFormat::R32_UINT);
                binding.storageReadOnly = true;
            }
            program.Layout.bindings.push_back(binding);
        }
        ComputePipelineDesc desc;
        desc.ComputeShader = shaderModules[i];
        desc.DescriptorSetLayouts.push_back(device.InternDescriptorSetLayout(program.Layout));
        desc.PushConstants = {0, sizeof(BlockPush), kShaderStageCompute};
        desc.DebugName = programNames[i];
        program.Id = device.InternComputePipeline(desc);
        if (!program.Id.IsValid())
            return;
    }

    const auto create = [&frame](const char* name, size_t bytes, BufferUsage usage)
    {
        BufferDesc desc{};
        desc.size = bytes;
        desc.memoryUsage = BufferMemoryUsage::DeviceLocal;
        desc.usage = static_cast<uint32_t>(BufferUsage::Storage | usage);
        return frame.CreateBuffer(name, desc);
    };
    const auto masks = create("IndexedOwner.BlockMasks", maskBytes, BufferUsage::TransferDst);
    const auto offsets = create("IndexedOwner.BlockOffsets", offsetBytes, BufferUsage::Storage);
    const auto countUsage = BufferUsage::Indirect | BufferUsage::TransferDst | BufferUsage::TransferSrc;
    owner.Draws = create("IndexedOwner.BlockDraws", drawBytes,
        useMesh ? BufferUsage::ShaderDeviceAddress : BufferUsage::Indirect);
    owner.State = create("IndexedOwner.BlockState", stateBytes,
        useMesh ? (countUsage | BufferUsage::ShaderDeviceAddress) : countUsage);
    owner.Compaction = frame.AllocatePassData<PreparedOwnerCompaction>();
    auto* work = frame.AllocatePassData<Prepared>();
    work->Match = owner.Compaction;
    const size_t recordsBytes = static_cast<size_t>(recordCapacity) * kOwnerReplayDrawStride;
    const size_t cursorsBytes = static_cast<size_t>(cursorCapacity) * sizeof(uint32_t);
    const auto inputRecords = frame.ImportExternalBuffer("IndexedOwner.BlockSourceDraws", records, recordsBytes);
    const auto inputCursors = frame.ImportExternalBuffer("IndexedOwner.BlockSourceCounts", cursors, cursorsBytes);
    const auto& extent = frame.Graph().ResourceDesc(owners.Id);
    const uint32_t groupsX = (extent.Width + 7u) / 8u;
    const uint32_t groupsY = (extent.Height + 7u) / 8u;

    frame.AddPass(slot == 0u ? "IndexedOwner.FilterBlocks" : "IndexedOwner.FilterSecond", PassPhase::kWorldRender,
        [&](RenderGraph::RGPassBuilder& pass)
        {
            pass.Read(ordering);
            pass.Read(inputRecords);
            pass.Read(inputCursors);
            pass.Read(owners, RenderGraph::RGTextureRead::Storage);
            pass.Write(masks, RenderGraph::RGBufferWrite::CopyDst);
            pass.Write(masks);
            pass.Write(offsets);
            pass.Write(owner.State, RenderGraph::RGBufferWrite::CopyDst);
            pass.Write(owner.State);
            pass.Write(owner.Draws);
            pass.Prepare([&data, &owner, useMesh, programs = std::move(programs), work, owners, masks, offsets,
                records, cursors, sourceRemap, recordCapacity, cursorCapacity, recordsBytes, cursorsBytes, maskBytes, stateBytes, drawBytes](RenderGraph::RGPrepareContext& context)
            {
                if (!owner.Pipeline.IsValid())
                    return true;
                const auto& draw = data.Draws[owner.DrawIndex];
                const auto source = GetIndexedDepthDraw(draw.Command, draw.Geometry);
                if (!source || source->Commands != records || source->Count != cursors ||
                    source->IndirectionAddress != sourceRemap || source->MaxDrawCount > kOwnerMaxDraws ||
                    source->CommandOffset % kOwnerReplayDrawStride != 0 ||
                    source->Stride != kOwnerReplayDrawStride || source->CountOffset % sizeof(uint32_t) != 0 ||
                    source->CommandOffset / kOwnerReplayDrawStride >= recordCapacity ||
                    source->CountOffset / sizeof(uint32_t) >= cursorCapacity ||
                    source->MaxDrawCount > recordCapacity - source->CommandOffset / kOwnerReplayDrawStride)
                    return true;
                const auto state = context.GetBuffer(owner.State);
                const auto commands = context.GetBuffer(owner.Draws);
                const auto marks = context.GetBuffer(masks);
                const auto ranges = context.GetBuffer(offsets);
                const auto image = context.GetTexture(owners);
                if (!state.IsValid() || !commands.IsValid() || !marks.IsValid() || !ranges.IsValid() || !image.IsValid())
                    return true;

                const DescriptorWrite classifier[]{
                    {.Binding = 0, .Type = DescriptorType::StorageImage, .Texture = image},
                    OwnerStorage(1, marks, maskBytes), OwnerStorage(2, cursors, cursorsBytes),
                    OwnerStorage(3, state, stateBytes)};
                const DescriptorWrite emit[]{
                    OwnerStorage(0, records, recordsBytes), OwnerStorage(1, marks, maskBytes),
                    OwnerStorage(2, ranges, offsetBytes), OwnerStorage(3, commands, drawBytes),
                    OwnerStorage(4, state, stateBytes)};
                const DescriptorWrite scan[]{
                    OwnerStorage(0, ranges, offsetBytes), OwnerStorage(1, state, stateBytes),
                    OwnerStorage(2, records, recordsBytes)};
                const std::array<std::span<const DescriptorWrite>, 3> writes{classifier, emit, scan};
                auto& device = *context.GetDevice();
                for (uint32_t i = 0; i < programs.size(); ++i)
                {
                    work->Pipelines[i] = context.PreparePipeline(programs[i].Id);
                    work->Sets[i] = device.GetOrCreateDescriptorSet(programs[i].Layout, writes[i], programNames[i]);
                    if (!work->Pipelines[i].IsValid() || !work->Sets[i].IsValid())
                        return true;
                }
                work->Constants = {static_cast<uint32_t>(source->CommandOffset / kOwnerReplayDrawStride),
                                    static_cast<uint32_t>(source->CountOffset / sizeof(uint32_t)),
                                    source->MaxDrawCount, 0u, shift, 0u};
                work->Bitset = marks;
                IndexedOwnerReplay replay{commands, state, kOwnerMaxDraws};
                if (useMesh)
                {
                    replay.MeshWorkAddress = device.GetBufferDeviceAddress(commands);
                    replay.MeshStateAddress = device.GetBufferDeviceAddress(state);
                    if (!replay.MeshWorkAddress || !replay.MeshStateAddress)
                        return true;
                }
                work->Match->Input = source;
                work->Match->Output = replay;
                return true;
            });
        },
        [work, groupsX, groupsY, stateBytes, maskBytes, useMesh](RenderGraph::RGContext& context)
        {
            const auto& output = work->Match->Output;
            if (!output.Counts.IsValid() || !context.Cmd)
                return;
            auto& cmd = *context.Cmd;
            const auto barrier = [&cmd](PipelineStageMask source, PipelineStageMask destination,
                                        ResourceAccessMask writes, ResourceAccessMask reads)
            {
                cmd.Barrier(ResourceBarrier::CreateMemoryBarrier(static_cast<uint64_t>(source),
                    static_cast<uint64_t>(destination), static_cast<uint64_t>(writes), static_cast<uint64_t>(reads)));
            };
            const auto bind = [&](uint32_t stage, BlockPush push)
            {
                const auto pipeline = work->Pipelines[stage];
                cmd.SetPipeline(pipeline);
                cmd.BindDescriptorSet(0, work->Sets[stage], pipeline);
                cmd.SetPushConstants(push);
            };
            const auto sync = [&]
            {
                barrier(PipelineStageMask::ComputeShader, PipelineStageMask::ComputeShader | PipelineStageMask::DrawIndirect,
                        ResourceAccessMask::ShaderWrite,
                        ResourceAccessMask::ShaderRead | ResourceAccessMask::ShaderWrite | ResourceAccessMask::IndirectCommandRead);
            };
            const size_t clearBytes = useMesh ? 8u * kOwnerMaxDraws * sizeof(uint32_t) : maskBytes;
            cmd.FillBuffer(work->Bitset, 0, clearBytes, 0u);
            cmd.FillBuffer(output.Counts, 0, stateBytes, 0u);
            barrier(PipelineStageMask::Transfer | PipelineStageMask::ComputeShader, PipelineStageMask::ComputeShader,
                    ResourceAccessMask::TransferWrite | ResourceAccessMask::ShaderWrite,
                    ResourceAccessMask::ShaderRead | ResourceAccessMask::ShaderWrite);
            auto push = work->Constants;
            push.Emit = 2u;
            bind(0, push);
            cmd.Dispatch(1, 1, 1);
            sync();
            bind(2, push);
            cmd.Dispatch(1, 1, 1);
            sync();
            if (useMesh)
            {
                push.Emit = 3u;
                bind(0, push);
                cmd.Dispatch(groupsX, groupsY, 1);
                sync();
                bind(1, push);
                cmd.Dispatch((kOwnerMaxDraws + 63u) / 64u, 1, 1);
                sync();
            }
            push.Emit = 0u;
            bind(0, push);
            cmd.Dispatch(groupsX, groupsY, 1);
            sync();
            bind(1, push);
            cmd.DispatchIndirect(output.Counts, 2u * sizeof(uint32_t));
            sync();
            bind(2, push);
            cmd.Dispatch(1, 1, 1);
            sync();
            push.Emit = 1u;
            bind(1, push);
            cmd.DispatchIndirect(output.Counts, 2u * sizeof(uint32_t));
            auto readers = PipelineStageMask::DrawIndirect | PipelineStageMask::Transfer;
            auto access = ResourceAccessMask::IndirectCommandRead | ResourceAccessMask::TransferRead;
            if (useMesh)
            {
                readers = readers | PipelineStageMask::GraphicsMesh;
                access = access | ResourceAccessMask::ShaderRead;
            }
            barrier(PipelineStageMask::ComputeShader, readers, ResourceAccessMask::ShaderWrite, access);
        });
}


void ReadIndexedOwnerCompaction(RenderGraph::RGPassBuilder& pass, const PreparedDepthOnlyPass& data)
{
    for (const auto& owner : data.Owners)
    {
        if (!owner.State.IsValid() || !owner.Draws.IsValid())
            continue;
        pass.Read(owner.State, RenderGraph::RGBufferRead::Indirect);
        if (owner.UsesMeshReplay)
        {
            pass.Read(owner.Draws, RenderGraph::RGBufferRead::MeshStorage);
            pass.Read(owner.State, RenderGraph::RGBufferRead::MeshStorage);
        }
        else
            pass.Read(owner.Draws, RenderGraph::RGBufferRead::Indirect);
    }
}

IndexedOwnerReplay MatchIndexedOwnerCompaction(const PreparedDepthOnlyPass& data, const IndexedDepthDraw& source)
{
    for (const auto& owner : data.Owners)
        if (owner.Compaction && owner.Compaction->Input == source)
            return owner.Compaction->Output;
    return {};
}

namespace
{
enum class DepthDrawSlice { Original, Owner, Between, Remainder };

void RecordPreparedDepthDraws(Rendering::RenderGraph::RGContext& context,
                              const PreparedDepthOnlyPass& data, CascadeCasters casters,
                              DepthDrawSlice slice = DepthDrawSlice::Original, size_t slot = 0u)
{
    const auto& primary = data.Owners[0];
    const auto& secondary = data.Owners[1];
    const bool split = primary.Pipeline.IsValid();
    const bool hasSecond = secondary.Pipeline.IsValid();
    const bool ownerOnly = slice == DepthDrawSlice::Owner;
    if ((slice != DepthDrawSlice::Original && !split) ||
        (ownerOnly && !data.Owners[slot].Pipeline.IsValid()) ||
        (slice == DepthDrawSlice::Between && !hasSecond))
        return;
    auto* commands = context.Cmd;
    if (!commands)
        return;
    const auto& params = data.Params;
    if (params.ViewportWidth != 0 && params.ViewportHeight != 0)
    {
        commands->SetViewport(static_cast<float>(params.ViewportX),
                              static_cast<float>(params.ViewportY),
                              static_cast<float>(params.ViewportWidth),
                              static_cast<float>(params.ViewportHeight));
        commands->SetScissor(params.ViewportX, params.ViewportY,
                             params.ViewportWidth, params.ViewportHeight);
    }

    if (slice == DepthDrawSlice::Original && data.UnderDrew && data.UnderDraw && casters != CascadeCasters::Rigid)
        data.UnderDraw->Mark(static_cast<uint32_t>(params.ViewId),
                             static_cast<uint8_t>(params.PassType), params.CascadeIndex, data.UnderDrawReason);
    if (!data.Walked)
        return;
    auto& binder = data.Materials->Binder();
    PassBindingContext pass{};
    pass.Cmd = commands;
    uint32_t drawsIssued = 0;
    DrawGeometryBindings geometryBindings(*commands);

    const auto& producerNames = Rendering::DrawProducerRegistry::ScopeNames();
    size_t begin = 0u;
    size_t end = data.Draws.size();
    if (split)
    {
        if (ownerOnly)
        {
            begin = data.Owners[slot].DrawIndex;
            end = begin + 1u;
        }
        else if (slice == DepthDrawSlice::Between)
        {
            begin = primary.DrawIndex + 1u;
            end = secondary.DrawIndex;
        }
        else if (slice == DepthDrawSlice::Remainder)
            begin = (hasSecond ? secondary.DrawIndex : primary.DrawIndex) + 1u;
        else
            end = primary.DrawIndex;
    }
    for (size_t i = begin; i < end; ++i)
    {
        const auto& draw = data.Draws[i];
        if (casters != CascadeCasters::All &&
            Rendering::IsSkinned(draw.VertexFlags) != (casters == CascadeCasters::Skinned))
            continue;
        const auto producer = draw.Entity ? Rendering::DrawProducerRegistry::kInstances : draw.Command.Producer;
        context.SetProfilerScope(producerNames[producer].data());
        const auto& geometry = draw.Geometry;
        geometryBindings.BindVertexBuffer(geometry.coreVB, 0);
        const auto optionalStreams = CollectOptionalStreamBinds(draw.VertexFlags, geometry);
        for (const auto& stream : optionalStreams.View())
            geometryBindings.BindVertexBuffer(stream.Buffer, stream.Slot);
        geometryBindings.BindVertexBuffer(geometry.jointsVB, 4);
        geometryBindings.BindVertexBuffer(geometry.weightsVB, 5);
        geometryBindings.BindVertexBuffer(geometry.joints1VB, 6);
        geometryBindings.BindVertexBuffer(geometry.weights1VB, 7);
        geometryBindings.BindIndexBuffer(geometry.indexBuffer, static_cast<Rendering::IndexType>(geometry.indexType));
        if (ownerOnly)
        {
            auto bindings = draw.Bindings;
            bindings.Pipeline = data.Owners[slot].Pipeline;
            binder.BindPreparedForDraw(pass, bindings, draw.Command.Bindings.PushConstants);
        }
        else
            binder.BindPreparedForDraw(pass, draw.Bindings, draw.Command.Bindings.PushConstants);
        const auto& command = draw.Command;
        if (command.UseIndirect && command.IndirectCommandBuffer.IsValid())
            command.RecordIndirect(*commands);
        else if (command.SubDraws.empty())
            commands->DrawIndexed(command.IndexCount, command.InstanceCount, command.FirstIndex, command.VertexOffset, command.FirstInstance);
        else
            for (const auto& sub : command.SubDraws)
                commands->DrawIndexed(sub.IndexCount, sub.InstanceCount, sub.FirstIndex, sub.VertexOffset, sub.FirstInstance);
        if (draw.ServedVersion)
            data.Materials->Variants().RecordServedGeneration(draw.MaterialIndex, ServedPassClass::Depth,
                                                              *draw.ServedVersion);
        if (draw.Entity)
        {
            ++drawsIssued;
            PushMeshDrawIssuedDiagnostic("depth", params.ViewId, draw.DiagnosticMaterialIndex, draw.MeshIndex,
                                         draw.MaxDrawCount, draw.RangeCount);
        }
    }
    if (slice == DepthDrawSlice::Original && DrawAttributionEnabled())
    {
        const char* attributionTag = "depth-prepass";
        switch (params.PassType)
        {
        case DepthPassType::ShadowCascade:
            attributionTag = "depth-cascade";
            break;
        case DepthPassType::AreaShadow:
            attributionTag = "depth-area";
            break;
        case DepthPassType::SpotShadow:
            attributionTag = "depth-spot";
            break;
        case DepthPassType::PointShadow:
            attributionTag = "depth-point";
            break;
        case DepthPassType::TransmittanceCascade:
            attributionTag = "depth-tint";
            break;
        case DepthPassType::DeformationMotion:
            attributionTag = "deformation-motion";
            break;
        default:
            break;
        }
        LogDrawAttribution(attributionTag, params.ViewId, data.EntityBatchCount, drawsIssued,
                           pass.PipelineBinds, pass.DescriptorBinds, data.Consolidated,
                           0u /* group census logged by the color pass */);
    }
    context.SetProfilerScope(nullptr);

    binder.EndPass(pass);
}
} // namespace

void RecordIndexedDepthOwner(RenderGraph::RGContext& context, const PreparedDepthOnlyPass& data, size_t slot)
{
    RecordPreparedDepthDraws(context, data, data.Params.Casters, DepthDrawSlice::Owner, slot);
}

void RecordIndexedDepthBetweenOwners(RenderGraph::RGContext& context, const PreparedDepthOnlyPass& data)
{
    RecordPreparedDepthDraws(context, data, data.Params.Casters, DepthDrawSlice::Between);
}

void RecordIndexedDepthRemainder(RenderGraph::RGContext& context, const PreparedDepthOnlyPass& data)
{
    RecordPreparedDepthDraws(context, data, data.Params.Casters, DepthDrawSlice::Remainder);
}

void RecordPreparedDepthOnlyPass(Rendering::RenderGraph::RGContext& context,
                                 const PreparedDepthOnlyPass& data)
{
    RecordPreparedDepthDraws(context, data, data.Params.Casters);
}

namespace
{
void ClearDepthLayer(Rendering::RenderGraph::RGContext& context,
                     Rendering::TextureHandle target, uint32_t layer)
{
    constexpr float kReverseZFarDepth = 0.0f;
    context.Cmd->EndRenderPass();
    context.Cmd->Barrier(Rendering::ResourceBarrier::CreateTextureBarrier(
        target, Rendering::ResourceState::DepthWrite, Rendering::ResourceState::DepthWrite,
        0u, 1u, layer, 1u));
    Rendering::RenderPassDesc pass{};
    pass.depthTarget = target;
    pass.clearColor[0] = false;
    pass.clearDepth = true;
    pass.clearDepthValue = kReverseZFarDepth;
    pass.depthLoadOp = Rendering::RenderPassDesc::LoadOp::Clear;
    pass.depthStoreOp = Rendering::RenderPassDesc::StoreOp::Store;
    pass.useDepthView = true;
    pass.depthViewDesc.aspect = Rendering::TextureAspect::Depth;
    pass.depthViewDesc.viewType = Rendering::TextureViewType::View2D;
    pass.depthViewDesc.baseLayer = layer;
    pass.depthViewDesc.layerCount = 1u;
    pass.depthViewDesc.levelCount = 1u;
    context.Cmd->BeginRenderPass(pass);
}
} // namespace

void RecordPreparedCascadeDepth(Rendering::RenderGraph::RGContext& context,
                                const PreparedDepthOnlyPass& data,
                                Rendering::TextureHandle target, uint32_t layer,
                                bool restoredRigidDepth)
{
    if (!context.Cmd)
        return;
    if (!restoredRigidDepth)
        ClearDepthLayer(context, target, layer);
    RecordPreparedDepthDraws(context, data,
        restoredRigidDepth ? CascadeCasters::Skinned : CascadeCasters::All);
}

void RecordPreparedRigidCascadeDepth(Rendering::RenderGraph::RGContext& context,
                                     const PreparedDepthOnlyPass& data,
                                     Rendering::TextureHandle target, uint32_t layer)
{
    if (!context.Cmd)
        return;
    ClearDepthLayer(context, target, layer);
    RecordPreparedDepthDraws(context, data, CascadeCasters::Rigid);
}

} // namespace Engine::Renderer
} // namespace GameEngine
