#include "Engine/Rendering/CpuDrawStreamBuilder.h"

#include "Engine/Rendering/Material.h"
#include "Engine/Rendering/MeshLODThresholds.h"
#include "Engine/Rendering/MeshGPURegistry.h"
#include "Engine/Rendering/RenderOrigin.h"
#include "Engine/Rendering/ViewRegistry.h"
#include "Engine/Rendering/WorldDrawBuilder.h"
#include "Engine/Rendering/WorldDrawTypes.h"
#include "JobSystem/ParallelAlgorithms.h"
#include "Rendering/Camera/CameraDerivation.h"
#include "Rendering/Common/Frustum.h"
#include "Rendering/Core/GPUCulling.h"
#include "Rendering/Core/GPUInstanceDepthClass.h"
#include "Rendering/Core/GPUScene.h"

#include <algorithm>
#include <cmath>

namespace GameEngine
{
namespace Engine::Renderer
{
namespace
{
// Candidate::State bits. State 0 (no kResolvedBit) covers every reason a
// submission produces no draw at all — no material, no pipeline, mesh not
// resident, instance not in GPUScene yet. That is the same filter
// WorldDrawBuilder applies when it derives the batch keys, so the lists and the
// keys can never disagree about which batches exist.
enum CandidateState : uint8_t
{
    kRejected = 0,
    kResolvedBit = 1u << 0,
    kVisibleBit = 1u << 1, // inside the view frustum and drawn in color (not shadows only)
    kCasterBit = 1u << 2,  // castShadows submission flag
};

// Below this, the fan-out costs more than the work. The per-instance body is a
// registry lookup plus a 6-plane sphere test, so the batch has to be large
// enough to dwarf a task publish.
constexpr size_t kCullMinBatchSize = 512;
// Sorting is the other O(n log n) term; the same reasoning sets its threshold.
constexpr size_t kSortThreshold = 4096;

uint64_t MakeBatchKey(uint32_t materialIndex, uint32_t meshIndex)
{
    return (static_cast<uint64_t>(materialIndex) << 32) | meshIndex;
}

uint8_t SelectCameraLod(const Rendering::GPUMesh& mesh, float coverage,
                        const CpuDrawStreamBuilder::LodPolicy& policy)
{
    const uint32_t lodCount = std::clamp(mesh.lodCount, 1u, Rendering::kMaxMeshLODs);
    if (policy.ForceLevel != 0xFFFFFFFFu)
        return static_cast<uint8_t>(std::min(policy.ForceLevel, lodCount - 1u));
    const float sseScale = (mesh.lodFlags & Rendering::kGPUMeshLodTightClassBit) != 0
        ? policy.TightSseScale : policy.DefaultSseScale;
    float previousThreshold = Rendering::kLodEffNone;
    for (uint32_t level = 0; level + 1u < lodCount; ++level)
    {
        const float threshold = Rendering::LodEffectiveThreshold(mesh.lodThreshold[level],
            (mesh.lodFlags & (1u << level)) != 0, sseScale, mesh.lodSseScaleCeil, previousThreshold);
        if (coverage >= threshold)
            return static_cast<uint8_t>(level);
        previousThreshold = threshold;
    }
    return static_cast<uint8_t>(lodCount - 1u);
}
} // namespace

// ParallelFor hands every worker a pointer to the caller's functor, so a range
// body that captures by reference reads the calling thread's stack on every
// iteration, while the calling thread runs a range of its own and writes its
// per-call temporaries (the sphere centre TestSphereFrustum takes by reference)
// a few bytes away. When the two share a cache line, which depends only on the
// stack depth at the call, each sphere test on the calling thread invalidates
// the line every worker reads. Each range therefore copies this block once and
// reads only that copy and the objects it points at.
struct CpuDrawStreamBuilder::CullInputs
{
    const WorldSubmissionRecord* Records = nullptr;
    const Rendering::MeshGPURegistry* MeshRegistry = nullptr;
    const Rendering::GPUInstance* Instances = nullptr;
    size_t InstanceCount = 0;
    const Rendering::GPUScene* Scene = nullptr;
    std::span<const Rendering::GPUMesh> Meshes;
    std::span<const uint32_t> AvailableCardMaterials;
    std::span<const uint64_t> PreviousCards;
    uint32_t SelectCardPrimary(uint32_t instanceIndex, uint32_t meshIndex, float coverage) const;
    LodPolicy Lod;
    Rendering::Vector3 Eye;
    float ProjectionScale = 0.0f;
    // The view's render origin sector; Planes take points relative to its origin.
    Mathematics::Vector3Int OriginSector{0, 0, 0};
    Rendering::Vector4 Planes[6];
    // The view is the editor Scene View: its camera and caster sets leave out what the eye hides.
    bool DropEditorHidden = false;
};

uint32_t CpuDrawStreamBuilder::CullInputs::SelectCardPrimary(uint32_t instanceIndex,
                                                               uint32_t meshIndex, float coverage) const
{
    constexpr uint32_t kNoCard = 0xFFFFFFFFu;
    const uint32_t link = Meshes[meshIndex].impostorCard;
    const uint32_t materialPlusOne = link & Rendering::kGPUMeshImpostorMaterialMask;
    if (materialPlusOne == 0u || !std::binary_search(AvailableCardMaterials.begin(),
        AvailableCardMaterials.end(), materialPlusOne - 1u))
        return kNoCard;

    const auto& partInstance = Instances[instanceIndex];
    const bool grouped = (link & Rendering::kGPUMeshImpostorPlacementBit) != 0u;
    const uint32_t primary = grouped ? partInstance.impostorPlacementPrimaryPlusOne - 1u : instanceIndex;
    if (primary >= InstanceCount)
        return kNoCard;
    const auto& primaryInstance = Instances[primary];
    const uint32_t firstMesh = grouped ? primaryInstance.meshIndex : meshIndex;
    if (firstMesh >= Meshes.size())
        return kNoCard;
    const auto& base = Meshes[firstMesh];
    if (grouped)
    {
        constexpr uint32_t primaryFlags = Rendering::kGPUMeshImpostorPrimaryBit |
                                          Rendering::kGPUMeshImpostorPlacementBit;
        if ((base.impostorCard & primaryFlags) != primaryFlags ||
            (base.impostorCard & Rendering::kGPUMeshImpostorMaterialMask) != materialPlusOne ||
            primaryInstance.impostorPlacementPrimaryPlusOne != primary + 1u ||
            (primaryInstance.flags >> 16u) != (partInstance.flags >> 16u) ||
            primaryInstance.boundingRadius != partInstance.boundingRadius ||
            primaryInstance.lodBias != partInstance.lodBias)
            return kNoCard;
        const auto center = Rendering::SectorLocalPointRelativeTo(
            primaryInstance.boundingCenter, primaryInstance.sectorPacked[0], primaryInstance.sectorPacked[1], OriginSector);
        const auto otherCenter = Rendering::SectorLocalPointRelativeTo(
            partInstance.boundingCenter, partInstance.sectorPacked[0], partInstance.sectorPacked[1], OriginSector);
        if (center.x != otherCenter.x || center.y != otherCenter.y || center.z != otherCenter.z)
            return kNoCard;
    }

    const uint64_t identity = MakeBatchKey(primary, Scene->GetInstanceContinuityStamp(primary));
    const bool wasCard = std::binary_search(PreviousCards.begin(), PreviousCards.end(), identity);
    const float enter = base.impostorSwitchCoverage > 0.0f ? base.impostorSwitchCoverage : Lod.CardCoverage;
    const float margin = 1.0f + Lod.CardHysteresis;
    if (coverage >= (wasCard ? enter * margin : enter))
        return kNoCard;
    const float meshCoverage = wasCard ? coverage : coverage * margin;
    uint32_t part = firstMesh;
    for (size_t checked = 0; checked < Meshes.size(); ++checked)
    {
        if (part >= Meshes.size() ||
            (Meshes[part].impostorCard & Rendering::kGPUMeshImpostorMaterialMask) != materialPlusOne)
            return kNoCard;
        const auto& mesh = Meshes[part];
        if (mesh.lodCount > 1u &&
            SelectCameraLod(mesh, meshCoverage * mesh.lodCoverageScale, Lod) != mesh.lodCount - 1u)
            return kNoCard;
        part = mesh.nextImpostorPart;
        if (part == firstMesh)
            return primary;
    }
    return kNoCard;
}

void CpuDrawStreamBuilder::Clear()
{
    m_ByView.clear();
    m_Stats = {};
}

void CpuDrawStreamBuilder::ViewLists::Reset()
{
    Indices.clear();
    for (size_t set = 0; set < 2; ++set)
    {
        Runs[set].clear();
        Index[set].clear();
    }
}

void CpuDrawStreamBuilder::Build(const WorldDrawBuilder& drawBuilder,
                                 const Rendering::MeshGPURegistry& meshRegistry,
                                 const Rendering::GPUScene& scene, const ViewRegistry& views,
                                 JobSystem::WorkStealingThreadPool* pool, const ViewLodPolicy& lodPolicy)
{
    m_Stats = {};
    // Keep the per-view slots (their vectors are the frame-to-frame scratch);
    // only their contents go stale.
    for (auto& [viewId, viewLists] : m_ByView)
        viewLists.Reset();

    for (const Rendering::ViewDesc& view : views.GetViews())
        BuildView(view.id, drawBuilder, meshRegistry, scene, views, pool, lodPolicy);
}

void CpuDrawStreamBuilder::BuildForView(Rendering::ViewId viewId,
                                        const WorldDrawBuilder& drawBuilder,
                                        const Rendering::MeshGPURegistry& meshRegistry,
                                        const Rendering::GPUScene& scene, const ViewRegistry& views,
                                        JobSystem::WorkStealingThreadPool* pool, const ViewLodPolicy& lodPolicy)
{
    auto it = m_ByView.find(viewId);
    if (it != m_ByView.end())
        it->second.Reset();
    BuildView(viewId, drawBuilder, meshRegistry, scene, views, pool, lodPolicy);
}

void CpuDrawStreamBuilder::BuildView(Rendering::ViewId viewId, const WorldDrawBuilder& drawBuilder,
                                     const Rendering::MeshGPURegistry& meshRegistry,
                                     const Rendering::GPUScene& scene, const ViewRegistry& views,
                                     JobSystem::WorkStealingThreadPool* pool, const ViewLodPolicy& lodPolicy)
{
    const std::span<const WorldSubmissionRecord> submissions = drawBuilder.GetSubmissions(viewId);
    if (submissions.empty())
    {
        if (auto it = m_ByView.find(viewId); it != m_ByView.end())
            it->second.PreviousCards.clear();
        return;
    }

    const Rendering::CameraData cam = views.ResolveCameraData(viewId);
    const std::vector<Rendering::GPUInstance>& instances = scene.GetInstances();

    CullInputs inputs;
    inputs.Records = submissions.data();
    inputs.MeshRegistry = &meshRegistry;
    inputs.Instances = instances.data();
    inputs.InstanceCount = instances.size();
    inputs.Scene = &scene;
    inputs.Meshes = scene.GetMeshes();
    inputs.Eye = CameraEyeRelativeToOrigin(cam);
    inputs.ProjectionScale = cam.cameraPos.w == 1.0f ? 0.0f : std::abs(cam.proj[1][1]);
    if (lodPolicy)
        inputs.Lod = lodPolicy(viewId);
    else
        inputs.Lod.ForceLevel = 0u;
    const Rendering::ViewDesc* view = views.FindViewDesc(viewId);
    inputs.DropEditorHidden = view != nullptr && view->DropsEditorHidden();
    // Cull relative to the view's render origin, in the arithmetic the GPU frustum
    // cull uses: planes from the rebased view-projection, each center composed
    // against the origin sector (SectorLocalPointRelativeTo).
    Rendering::ExtractFrustumPlanes(cam.viewProjRel, inputs.Planes);
    inputs.OriginSector = CameraRenderOriginSector(cam);

    ViewLists& viewLists = m_ByView[viewId];
    m_AvailableCardMaterials.clear();
    if (inputs.Lod.CardMesh < inputs.Meshes.size() && inputs.ProjectionScale > 0.0f &&
        inputs.Lod.ForceLevel == 0xFFFFFFFFu)
    {
        // The world pass reads these same keys. A missing batch must keep
        // the source instances drawable even if their mesh rows still link a card.
        for (const auto& key : drawBuilder.GetBatchKeys(viewId))
        {
            if (key.meshIndex != inputs.Lod.CardMesh ||
                key.material->GetCompileSpec().surfaceShaderPath != "Surfaces/impostor.glsl")
                continue;
            const auto* entry = meshRegistry.Find(key.mesh);
            if (entry && meshRegistry.IsEntryDrawable(*entry))
                m_AvailableCardMaterials.push_back(key.materialIndex);
        }
        std::sort(m_AvailableCardMaterials.begin(), m_AvailableCardMaterials.end());
    }
    inputs.AvailableCardMaterials = m_AvailableCardMaterials;
    inputs.PreviousCards = viewLists.PreviousCards;

    m_Candidates.resize(submissions.size());
    Candidate* candidates = m_Candidates.data();

    // Resolve + cull every submission independently: no shared writes, one
    // slot per input, so the fan-out needs no synchronisation and the
    // compaction below stays deterministic regardless of chunk order.
    const auto resolveRange = [&inputs, candidates](size_t begin, size_t end) {
        ResolveAndCullRange(inputs, candidates, begin, end);
    };
    JobSystem::ParallelFor(pool, submissions.size(), resolveRange, kCullMinBatchSize);

    m_CameraEntries.clear();
    m_ShadowEntries.clear();
    m_NextCards.clear();
    m_CameraEntries.reserve(submissions.size());
    m_ShadowEntries.reserve(submissions.size());
    for (size_t i = 0; i < submissions.size(); ++i)
    {
        const Candidate& c = candidates[i];
        if ((c.State & kResolvedBit) == 0u)
            continue;
        ++m_Stats.Candidates;
        if ((c.State & kVisibleBit) != 0u)
        {
            m_CameraEntries.push_back(KeyedInstance{c.CameraKey, c.Instance, c.Lod});
            if (c.CameraKey != c.Key)
                m_NextCards.push_back(MakeBatchKey(c.Instance, scene.GetInstanceContinuityStamp(c.Instance)));
        }
        if ((c.State & kCasterBit) != 0u)
            m_ShadowEntries.push_back(KeyedInstance{c.Key, c.Instance, 0u});
    }
    m_Stats.CameraVisible += static_cast<uint32_t>(m_CameraEntries.size());
    m_Stats.ShadowCasters += static_cast<uint32_t>(m_ShadowEntries.size());

    std::sort(m_NextCards.begin(), m_NextCards.end());
    viewLists.PreviousCards.swap(m_NextCards);
    if (m_CameraEntries.empty() && m_ShadowEntries.empty())
        return;

    m_Stats.CameraBatches += EmitLists(viewLists, InstanceSet::Camera, m_CameraEntries, pool);
    m_Stats.ShadowBatches += EmitLists(viewLists, InstanceSet::ShadowCasters, m_ShadowEntries, pool);
}

void CpuDrawStreamBuilder::ResolveAndCullRange(CullInputs inputs, Candidate* candidates,
                                               size_t begin, size_t end)
{
    for (size_t i = begin; i < end; ++i)
    {
        const WorldSubmissionRecord& rec = inputs.Records[i];
        Candidate& out = candidates[i];
        out.State = kRejected;

        if (!rec.material || !rec.material->GetGraphicsPipelineId().IsValid())
            continue;
        const Rendering::MeshGPUEntry* entry = inputs.MeshRegistry->Find(rec.meshHandle);
        if (!entry || entry->gpuMeshIndex == ~0u || entry->indexCount == 0u)
            continue;
        if (rec.instanceIndex >= inputs.InstanceCount)
            continue; // not resident in GPUScene yet (first frame after spawn)

        out.Key = MakeBatchKey(rec.material->GetGpuSceneMaterialIndex(), entry->gpuMeshIndex);
        out.CameraKey = out.Key;
        out.Instance = rec.instanceIndex;
        out.Lod = 0;

        const Rendering::GPUInstance& inst = inputs.Instances[rec.instanceIndex];
        const Rendering::Vector3 center = Rendering::SectorLocalPointRelativeTo(
            inst.boundingCenter, inst.sectorPacked[0], inst.sectorPacked[1], inputs.OriginSector);
        out.State = kResolvedBit;
        if (Rendering::InstanceInColorPass(rec.flags, inputs.DropEditorHidden) &&
            Rendering::TestSphereFrustum(center, inst.boundingRadius, inputs.Planes))
        {
            out.State |= kVisibleBit;
            const auto* mesh = entry->gpuMeshIndex < inputs.Meshes.size()
                ? &inputs.Meshes[entry->gpuMeshIndex] : nullptr;
            const bool selectsLod = mesh && mesh->lodCount > 1u && inputs.Lod.ForceLevel != 0u &&
                (inputs.ProjectionScale > 0.0f || inputs.Lod.ForceLevel != 0xFFFFFFFFu);
            const bool selectsCard = mesh && mesh->impostorCard != 0u && !inputs.AvailableCardMaterials.empty();
            float coverage = 0.0f;
            if (inputs.ProjectionScale > 0.0f && (inputs.Lod.SmallObjectCoverage > 0.0f ||
                selectsCard || (selectsLod && inputs.Lod.ForceLevel == 0xFFFFFFFFu)))
            {
                const float distance = std::max((center - inputs.Eye).Length(), 1.0e-4f);
                coverage = inst.boundingRadius * inputs.ProjectionScale / distance;
                coverage *= std::exp2(inst.lodBias + inputs.Lod.GlobalBias);
                if (coverage < inputs.Lod.SmallObjectCoverage)
                    out.State &= ~kVisibleBit;
            }
            if ((out.State & kVisibleBit) != 0u && selectsLod)
                out.Lod = SelectCameraLod(*mesh, coverage * mesh->lodCoverageScale, inputs.Lod);
            if ((out.State & kVisibleBit) != 0u && selectsCard)
            {
                const uint32_t primary = inputs.SelectCardPrimary(rec.instanceIndex, entry->gpuMeshIndex, coverage);
                if (primary != 0xFFFFFFFFu)
                {
                    if (primary != rec.instanceIndex ||
                        (mesh->impostorCard & Rendering::kGPUMeshImpostorPrimaryBit) == 0u)
                        out.State &= ~kVisibleBit;
                    else
                    {
                        out.CameraKey = MakeBatchKey((mesh->impostorCard &
                            Rendering::kGPUMeshImpostorMaterialMask) - 1u, inputs.Lod.CardMesh);
                        out.Lod = 0;
                    }
                }
            }
        }
        if (Rendering::InstanceInShadowPass(rec.flags, inputs.DropEditorHidden))
            out.State |= kCasterBit;
    }
}

uint32_t CpuDrawStreamBuilder::EmitLists(ViewLists& view, InstanceSet set,
                                         std::vector<KeyedInstance>& entries,
                                         JobSystem::WorkStealingThreadPool* pool)
{
    if (entries.empty())
        return 0u;

    // Instance order inside a mesh LOD run is the GPUScene order, so neighbouring
    // vertex-stage invocations read neighbouring GPUInstances.
    const auto byKeyThenInstance = [](const KeyedInstance& a, const KeyedInstance& b) {
        if (a.Key != b.Key)
            return a.Key < b.Key;
        if (a.Lod != b.Lod)
            return a.Lod < b.Lod;
        return a.Instance < b.Instance;
    };
    if (entries.size() >= kSortThreshold)
        JobSystem::ParallelSort(pool, entries.begin(), entries.end(), byKeyThenInstance);
    else
        std::sort(entries.begin(), entries.end(), byKeyThenInstance);

    auto& index = view.Index[static_cast<size_t>(set)];
    auto& runs = view.Runs[static_cast<size_t>(set)];
    const size_t initialRunCount = runs.size();
    size_t keyBegin = 0;
    while (keyBegin < entries.size())
    {
        const uint64_t key = entries[keyBegin].Key;
        const uint32_t firstRun = static_cast<uint32_t>(runs.size());
        while (keyBegin < entries.size() && entries[keyBegin].Key == key)
        {
            const uint32_t first = static_cast<uint32_t>(view.Indices.size());
            const uint8_t lod = entries[keyBegin].Lod;
            view.Indices.push_back(entries[keyBegin++].Instance);
            while (keyBegin < entries.size() && entries[keyBegin].Key == key && entries[keyBegin].Lod == lod)
            {
                if (entries[keyBegin].Instance != entries[keyBegin - 1].Instance)
                    view.Indices.push_back(entries[keyBegin].Instance);
                ++keyBegin;
            }
            runs.push_back({{first, static_cast<uint32_t>(view.Indices.size()) - first}, lod});
        }
        index.emplace(key, InstanceList{firstRun, static_cast<uint32_t>(runs.size()) - firstRun});
    }
    return static_cast<uint32_t>(runs.size() - initialRunCount);
}

CpuDrawStreamBuilder::InstanceList CpuDrawStreamBuilder::GetInstances(
    Rendering::ViewId viewId, uint32_t materialIndex, uint32_t meshIndex, InstanceSet set) const
{
    const auto runs = GetLodRuns(viewId, materialIndex, meshIndex, set);
    if (runs.empty())
        return {};
    const uint32_t first = runs.front().Instances.First;
    const auto& last = runs.back().Instances;
    return {first, last.First + last.Count - first};
}

std::span<const CpuDrawStreamBuilder::LodRun> CpuDrawStreamBuilder::GetLodRuns(
    Rendering::ViewId viewId, uint32_t materialIndex, uint32_t meshIndex, InstanceSet set) const
{
    auto itView = m_ByView.find(viewId);
    if (itView == m_ByView.end())
        return {};
    const auto& index = itView->second.Index[static_cast<size_t>(set)];
    auto itKey = index.find(MakeBatchKey(materialIndex, meshIndex));
    if (itKey == index.end())
        return {};
    return std::span(itView->second.Runs[static_cast<size_t>(set)]).subspan(itKey->second.First, itKey->second.Count);
}

std::span<const uint32_t> CpuDrawStreamBuilder::GetIndexList(Rendering::ViewId viewId) const
{
    auto itView = m_ByView.find(viewId);
    if (itView == m_ByView.end())
        return {};
    return itView->second.Indices;
}

} // namespace Engine::Renderer
} // namespace GameEngine
