// Prepares depth/shadow draws before recording and replays frame-owned packets.
#include "Engine/Rendering/DepthDrawRecorder.h"
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

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
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
    };
    explicit PreparedDepthOnlyPass(DepthOnlyPassParamsRG params) : Params(std::move(params))
    {
        Inputs.PassResources = std::move(Params.Resources);
    }
    DepthOnlyPassParamsRG Params;
    PassBindingContext Inputs;
    std::vector<Draw> Draws;
    std::vector<std::byte> PushConstants;
    MaterialSystem* Materials = nullptr;
    DepthUnderDrawTracker* UnderDraw = nullptr;
    bool UnderDrew = false;
    bool Walked = false;
    size_t EntityBatchCount = 0;
    bool Consolidated = false;
    DepthProducerState AllProducer;
    DepthProducerState RigidProducer;
};

PreparedDepthOnlyPass* AllocatePreparedDepthOnlyPass(Rendering::RenderGraph::RGFrame& frame,
                                                    DepthOnlyPassParamsRG params)
{
    return frame.AllocatePassData<PreparedDepthOnlyPass>(std::move(params));
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
        m_Data.Materials = &m_Services.Materials;
        m_Data.UnderDraw = m_Services.UnderDraw;
        m_Data.UnderDrew = false;
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
        const uint32_t passInstanceIndex =
            (static_cast<uint32_t>(m_Data.Params.PassType) << 16) |
            ((m_Data.Params.PassType == DepthPassType::ShadowCascade ||
              m_Data.Params.PassType == DepthPassType::PointShadow ||
              m_Data.Params.PassType == DepthPassType::TransmittanceCascade)
                 ? m_Data.Params.CascadeIndex
                 : 0u);
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
        if (!depthCommands.empty() || !fallbackHeads.empty() || m_Services.CpuDrawStream ||
            m_Data.Params.PassType != DepthPassType::ShadowCascade)
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

        m_CascadeIndex = Rendering::GPUDrawStreamBuilder::kCascadeIndexNone;
        if (m_Data.Params.PassType == DepthPassType::ShadowCascade ||
            m_Data.Params.PassType == DepthPassType::TransmittanceCascade)
            m_CascadeIndex = static_cast<uint8_t>(m_Data.Params.CascadeIndex);
        else if (m_Data.Params.PassType == DepthPassType::AreaShadow)
            m_CascadeIndex = kAreaShadowCullingIndex;
        else if (m_Data.Params.PassType == DepthPassType::SpotShadow)
            m_CascadeIndex = kSpotShadowCullingIndex;
        else if (m_Data.Params.PassType == DepthPassType::PointShadow)
            m_CascadeIndex = PointShadowCullingIndex(m_Data.Params.CascadeIndex);

        m_TrackUnderDraw =
            m_Services.UnderDraw != nullptr &&
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
        if (kShadowWalkEarlyOut && !m_Services.CpuDrawStream && isOpaqueShadowSlice && depthCommands.empty() && m_Services.Streams)
        {
            const uint32_t survivors = m_Services.Streams->PreviousFrameShadowSurvivors(
                static_cast<uint32_t>(m_Data.Params.ViewId), m_CascadeIndex, m_Data.Params.Phase);
            if (survivors == 0u)
            {
                LogMeshPassDiagnostic("depth", "walk-skip-empty-slice", m_Data.Params.ViewId,
                                      entityKeys.size(), depthCommands.size(),
                                      m_Services.Streams->GetRangeCount());
                MarkUnderDraw();
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
                    MarkUnderDraw(m_Services.Meshes.Find(key.mesh)); // missing current geometry group
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
                    MarkUnderDraw(m_Services.Meshes.Find(key.mesh)); // missing current geometry group
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
        const Rendering::ShaderMeta* Metadata = nullptr;
        std::span<const Rendering::DescriptorSetLayoutId> SetLayouts;
        // 0 = derive the count from the variant meta (a fragment stage
        // brings the material's own sets); otherwise the vertex-only
        // pipeline's own set count.
        uint32_t SetCount = 1u;
        std::optional<uint32_t> ServedVersion;
    };

    void MarkUnderDraw(const Rendering::MeshGPUEntry* mesh = nullptr)
    {
        m_Data.AllProducer.Complete = false;
        if (!mesh || !Rendering::IsSkinned(mesh->vertexFlags))
            m_Data.RigidProducer.Complete = false;
        if (m_TrackUnderDraw)
            m_Data.UnderDrew = true;
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
            MarkUnderDraw(entry); // async material publish gap — heals next frames
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
            MarkUnderDraw(entry); // mesh upload in flight — heals next frames
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
            MarkUnderDraw(entry); // buffer not resident yet — heals next frames
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
        if ((material->GetAlphaMode() == MaterialAlphaMode::Mask || vertexModified) &&
            (!m_Services.Textures || !m_Services.Textures->IsMaterialTextureBindingComplete(material->GetGuid())))
            MarkUnderDraw(entry);
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
                                          drawVertexFlags, head, depthCullMode, isSkinnedDepth};

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
                MarkUnderDraw(entry); // async depth-variant publish gate — heals on publish
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
                MarkUnderDraw(entry); // bind-time publish gate — heals on publish
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
            MarkUnderDraw(entry); // scatter pipeline still compiling — heals next frames
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
            MarkUnderDraw(entry);
            return;
        }
        // Shared indirection buffer: arena-global firstInstance → one BDA for
        // every batch, slice, and cascade.
        const uint64_t slotIndirectionBda =
            m_Services.Streams->GetSharedIndirectionAddress();
        if (slotIndirectionBda == 0 || m_InstanceAddress == 0)
        {
            LogMeshDrawSkipDiagnostic(
                "depth", slotIndirectionBda == 0 ? "indirection-bda-zero" : "instance-bda-zero",
                m_Data.Params.ViewId, key.materialIndex, key.meshIndex,
                true, true, slotIndirectionBda, m_InstanceAddress,
                m_Services.Device->GetCapabilities().supportsBufferDeviceAddress,
                m_Services.Streams->GetRangeCount());
            MarkUnderDraw(entry);
            return;
        }

        instanceConstants.indirectionAddr = slotIndirectionBda;
        instanceConstants.gpuInstancesAddr = m_InstanceAddress;
        std::span<const std::byte> constantBytes(
            reinterpret_cast<const std::byte*>(&instanceConstants), sizeof(instanceConstants));

        Engine::Renderer::DrawBindings drawBindings{};
        drawBindings.PushConstants = constantBytes;

        // Draw one parity segment with the depth pipeline compiled for its
        // winding. The mesh registry's index buffer + push constants are shared;
        // only the pipeline (front face) and the indirect offsets differ.

        const Rendering::FrontFace baseWinding = m_Services.FrontFace;
        for (const auto& drawSegment : drawRange.Segments())
        {
            assert(!drawSegment.crossfading || DepthPassDrawsCrossfadeTails(m_Data.Params.PassType));
            PrepareSegment(batch, drawRange, drawBindings, slotIndirectionBda, drawSegment.mirrored ? Rendering::FlipWinding(baseWinding) : baseWinding,
                           drawSegment.crossfading, drawSegment.segment);
        }
    }

    ResolvedDepthPipeline ResolvePipeline(const DepthBatchPreparation& batch,
                                          Rendering::FrontFace winding, bool crossfading)
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
                resolved.Metadata = m_Services.Materials.Variants().GetSharedDepthShaderMeta(batch.IsSkinned);
                resolved.SetLayouts = m_Services.Materials.Variants().GetSharedDepthSetLayouts(batch.IsSkinned);
                return resolved;
            }
        }
        auto [pipeline, variantEntry] = m_Services.Materials.Variants().PrepareDepthInstancedVariant(
            *batch.Material, batch.DrawVertexFlags, batch.Mesh->topology, choice.Keywords,
            m_Data.Params.RasterizationSamples, m_Data.Params.DepthBiasEnable, m_Data.Params.DepthClampEnable,
            winding, m_Context);
        resolved.Pipeline = pipeline;
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
                        Rendering::FrontFace winding, bool crossfading,
                        const Rendering::GPUDrawStreamBuilder::BatchDrawSegment& segment)
    {
        const size_t commandOffset = segment.cmdByteOffset;
        const size_t countOffset = segment.countByteOffset;
        const uint32_t maxDrawCount = segment.maxDrawCount;
        const ResolvedDepthPipeline resolved = ResolvePipeline(batch, winding, crossfading);
        if (!resolved.Pipeline.IsValid())
        {
            LogMeshDrawSkipDiagnostic(
                "depth", "depth-pipeline-invalid", m_Data.Params.ViewId,
                batch.Key.materialIndex, batch.Key.meshIndex,
                true, true, slotIndirectionBda, m_InstanceAddress,
                m_Services.Device->GetCapabilities().supportsBufferDeviceAddress,
                m_Services.Streams->GetRangeCount());
            MarkUnderDraw(batch.Mesh); // async depth-variant publish gate — heals on publish
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
            MarkUnderDraw(batch.Mesh); // bind-time publish gate — heals on publish
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
    }

    void PrepareContributor(const DrawCommand& command)
    {
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

bool PrepareDepthOnlyPass(RenderGraph::RGPrepareContext& context,
                          PreparedDepthOnlyPass& data, const DepthDrawServices& services)
{
    return DepthPassPreparation(context, data, services).Prepare();
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
void RecordPreparedDepthDraws(Rendering::RenderGraph::RGContext& context,
                              const PreparedDepthOnlyPass& data, CascadeCasters casters)
{
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

    if (data.UnderDrew && data.UnderDraw && casters != CascadeCasters::Rigid)
        data.UnderDraw->Mark(static_cast<uint32_t>(params.ViewId),
                             static_cast<uint8_t>(params.PassType), params.CascadeIndex);
    if (!data.Walked)
        return;
    auto& binder = data.Materials->Binder();
    PassBindingContext pass{};
    pass.Cmd = commands;
    uint32_t drawsIssued = 0;
    DrawGeometryBindings geometryBindings(*commands);

    for (const auto& draw : data.Draws)
    {
        if (casters != CascadeCasters::All &&
            Rendering::IsSkinned(draw.VertexFlags) != (casters == CascadeCasters::Skinned))
            continue;
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
        binder.BindPreparedForDraw(pass, draw.Bindings, draw.Command.Bindings.PushConstants);
        const auto& command = draw.Command;
        if (command.UseIndirect && command.IndirectCommandBuffer.IsValid())
            commands->DrawIndexedIndirectCount(command.IndirectCommandBuffer, command.IndirectCountBuffer,
                                               command.IndirectMaxDrawCount, command.IndirectStride,
                                               command.IndirectCommandOffset, command.IndirectCountOffset);
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
    if (DrawAttributionEnabled())
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

    binder.EndPass(pass);
}
} // namespace

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
