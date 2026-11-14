// WorldDrawBuilder.cpp
#include "Engine/Rendering/WorldDrawBuilder.h"

#include "Core/CpuProfiler.h"
#include "Engine/Rendering/Material.h"
#include "Engine/Rendering/MaterialDeformationClassify.h"
#include "Engine/Rendering/MaterialVertexAnimation.h"
#include "Engine/Rendering/MeshGPURegistry.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Rendering/Core/ThreadingUtils.h"
#include "Rendering/Core/HashUtils.h"

#include <algorithm>
#include <cassert>
#include <thread>

namespace GameEngine
{
namespace Engine::Renderer
{
using namespace ::GameEngine::Rendering;

namespace
{
void AddShadowMembership(WorldDrawBuilder::ShadowSubmissionState& state,
                         uint64_t identity, bool drawable)
{
    // Addition preserves order independence and multiplicity, unlike XOR.
    state.MembershipKey += HashUtils::HashValue(identity, drawable);
    ++state.MembershipCount;
    state.Complete = state.Complete && drawable;
}

// The material facts the key derivation reads, one bit each.
constexpr uint32_t kPipelineValid = 1u << 0;
constexpr uint32_t kTransmission = 1u << 1;
constexpr uint32_t kDeformsVertices = 1u << 2;
constexpr uint32_t kSupportsDeformationMotion = 1u << 3;

uint32_t MaterialKeyPredicates(const Material& material)
{
    uint32_t predicates = 0;
    if (material.GetGraphicsPipelineId().IsValid())
        predicates |= kPipelineValid;
    if (HasKeyword(material.GetVariantKey().materialKeywords, MaterialKeyword::Transmission))
        predicates |= kTransmission;
    if (MaterialDeformsVertices(material))
        predicates |= kDeformsVertices;
    if (SupportsDeformationMotion(material))
        predicates |= kSupportsDeformationMotion;
    return predicates;
}

// colorClassId == materialIndex when the merge is off (empty map) or the
// material is not merge-eligible (identity entry).
uint32_t ColorClassOf(uint32_t materialIndex, std::span<const uint32_t> materialColorClass)
{
    return materialIndex < materialColorClass.size() ? materialColorClass[materialIndex] : materialIndex;
}
} // namespace

void WorldDrawBuilder::ViewBatchState::Reset()
{
    Submissions.clear();
    Keys.clear();
    DeformingKeys.clear();
    ShadowSubmissions = {};
    ShadowCaster = false;
    TransmissiveCaster = false;
    AnimatedVertexModifier = false;
    AnimatedVertexModifierCaster = false;
}

bool WorldDrawBuilder::BuildInputs::SameInputs(const BuildInputs& other) const
{
    return Certified && other.Certified && MeshContentRevision == other.MeshContentRevision &&
           MaterialIndexGeneration == other.MaterialIndexGeneration &&
           SubmissionRevisions == other.SubmissionRevisions;
}

WorldDrawBuilder::WorldDrawBuilder(const JobSystem::WorkStealingThreadPool* jobSystem)
    : m_JobSystem(jobSystem),
      m_OffPoolBufferIndex(jobSystem ? jobSystem->GetWorkerCount() : 0),
      m_ThreadBuffers(m_OffPoolBufferIndex + 1),
      m_OwnerThreadId(std::this_thread::get_id())
{
}

void WorldDrawBuilder::BeginFrame()
{
    // Each submitting thread owns one buffer; buffers on separate lines keep the
    // threads from invalidating each other's records.
    static_assert(alignof(decltype(m_ThreadBuffers)::value_type) == kFalseSharingSeparation);

    for (auto& entry : m_ThreadBuffers)
    {
        ThreadSubmissionBuffer& buffer = entry.Value;
        buffer.records.clear();
        buffer.setRevisions.clear();
        buffer.drained = 0;
    }

    // A frame with no build leaves reset views, so the next build must derive.
    if (!m_BuiltThisFrame)
        m_LastBuildInputs.Certified = false;
    m_BuiltThisFrame = false;

    // The last frame's views move aside intact: a kept build swaps them back.
    std::swap(m_Views, m_PreviousViews);
    for (auto& [viewId, view] : m_Views)
        view.Reset();
    m_MergedThisFrame = false;
}

void WorldDrawBuilder::Submit(std::span<const WorldSubmissionRecord> records, uint64_t setRevision)
{
    if (records.empty())
        return;

    // A worker of the construction pool owns the buffer at its id. Every other
    // thread, a worker of another pool included, uses the off-pool buffer.
    const size_t index = JobSystem::WorkStealingThreadPool::IsComputeWorkerOf(m_JobSystem)
                             ? m_JobSystem->GetCurrentWorkerId()
                             : m_OffPoolBufferIndex;
    assert(index < m_ThreadBuffers.size() && "Submit worker id is outside the construction pool");
    assert((index != m_OffPoolBufferIndex || std::this_thread::get_id() == m_OwnerThreadId)
           && "Submit off the pool must come from the thread that constructed the builder");
    ThreadSubmissionBuffer& buffer = m_ThreadBuffers[index].Value;
    buffer.records.insert(buffer.records.end(), records.begin(), records.end());
    buffer.setRevisions.push_back(setRevision);
}

void WorldDrawBuilder::MergeSubmissions()
{
    // Drain only the records added since the last call, so a late submit (a
    // thumbnail view after the main scene built its keys) lands on the next call.
    for (auto& entry : m_ThreadBuffers)
    {
        ThreadSubmissionBuffer& buffer = entry.Value;
        const uint32 total = static_cast<uint32>(buffer.records.size());
        if (buffer.drained == total)
            continue;
        m_MergedThisFrame = true;
        // Records of one view arrive in runs: one view lookup per run.
        ViewBatchState* view = nullptr;
        ViewId viewId = 0;
        for (uint32 recordIndex = buffer.drained; recordIndex < total; ++recordIndex)
        {
            const WorldSubmissionRecord& record = buffer.records[recordIndex];
            if (!view || record.viewId != viewId)
            {
                viewId = record.viewId;
                view = &m_Views.GetOrInsert(viewId);
            }
            view->Submissions.push_back(record);
        }
        buffer.drained = total;
    }
}

void WorldDrawBuilder::DropUnmergedSubmissions()
{
    for (auto& entry : m_ThreadBuffers)
        entry.Value.drained = static_cast<uint32>(entry.Value.records.size());
}

void WorldDrawBuilder::CaptureInputs(const Rendering::MeshGPURegistry& meshRegistry,
                                     uint64_t materialIndexGeneration)
{
    BuildInputs& inputs = m_FrameInputs;
    inputs.SubmissionRevisions.clear();
    inputs.Certified = true;
    for (const auto& entry : m_ThreadBuffers)
    {
        for (const uint64_t revision : entry.Value.setRevisions)
        {
            inputs.Certified = inputs.Certified && revision != kNoRecordSet;
            inputs.SubmissionRevisions.push_back(revision);
        }
    }
    // The worker that submits a set changes from frame to frame; the sets do not.
    std::sort(inputs.SubmissionRevisions.begin(), inputs.SubmissionRevisions.end());
    inputs.MeshContentRevision = meshRegistry.GetContentRevision();
    inputs.MaterialIndexGeneration = materialIndexGeneration;
}

bool WorldDrawBuilder::MaterialsGiveSameKeyInputs(std::span<const uint32_t> materialColorClass) const
{
    // Called only after the material index generation compared equal, so every
    // pointer here still names a live material.
    for (const MaterialKeyState& state : m_LastBuildMaterials)
    {
        const Material& material = *state.Source;
        const uint32_t materialIndex = material.GetGpuSceneMaterialIndex();
        if (materialIndex != state.MaterialIndex ||
            ColorClassOf(materialIndex, materialColorClass) != state.ColorClassId ||
            MaterialKeyPredicates(material) != state.Predicates ||
            material.GetContentRevision() != state.ContentRevision ||
            material.GetVersion() != state.ProgramVersion)
            return false;
    }
    return true;
}

void WorldDrawBuilder::CaptureMaterialKeyStates(std::span<const uint32_t> materialColorClass)
{
    m_LastBuildMaterials.clear();
    m_LastBuildMaterials.reserve(m_MaterialsRead.size());
    for (const Material* material : m_MaterialsRead)
    {
        const uint32_t materialIndex = material->GetGpuSceneMaterialIndex();
        m_MaterialReadByIndex[materialIndex] = nullptr;
        m_LastBuildMaterials.push_back(MaterialKeyState{material, materialIndex,
                                                        ColorClassOf(materialIndex, materialColorClass),
                                                        MaterialKeyPredicates(*material),
                                                        material->GetContentRevision(), material->GetVersion()});
    }
    m_MaterialsRead.clear();
}

uint32_t WorldDrawBuilder::BuildBatchKeys(const Rendering::MeshGPURegistry& meshRegistry,
                                          std::span<const uint32_t> materialColorClass,
                                          uint64_t materialIndexGeneration)
{
    CaptureInputs(meshRegistry, materialIndexGeneration);
    if (m_FrameInputs.SameInputs(m_LastBuildInputs) && MaterialsGiveSameKeyInputs(materialColorClass))
    {
        // A repeat build in the same frame already holds the views of this frame.
        if (m_BuiltThisFrame)
            return 0;
        GE_CPU_PROFILE_SCOPE("WorldDrawBuilder.KeepLastBuild");
        std::swap(m_Views, m_PreviousViews);
        DropUnmergedSubmissions();
        m_BuiltThisFrame = true;
        return 0;
    }

    {
        GE_CPU_PROFILE_SCOPE("WorldDrawBuilder.MergeSubmissions");
        MergeSubmissions();
    }

    GE_CPU_PROFILE_SCOPE("WorldDrawBuilder.DeriveBatchKeys");
    m_ReadUnindexedMaterial = false;
    for (auto& [viewId, view] : m_Views)
        DeriveBatchKeysForView(view, meshRegistry, materialColorClass, &m_MaterialsRead);
    CaptureMaterialKeyStates(materialColorClass);

    m_FrameInputs.Certified = m_FrameInputs.Certified && !m_ReadUnindexedMaterial;
    // Swapped, not copied: the next capture reuses the old vector's capacity.
    std::swap(m_LastBuildInputs, m_FrameInputs);
    // The merge above ran inside the build; a later merge this frame ends the keep.
    m_MergedThisFrame = false;
    m_BuiltThisFrame = true;
    return static_cast<uint32_t>(m_Views.Size());
}

void WorldDrawBuilder::BuildBatchKeysForView(Rendering::ViewId viewId,
                                             const Rendering::MeshGPURegistry& meshRegistry,
                                             std::span<const uint32_t> materialColorClass)
{
    MergeSubmissions();
    // A late merge adds records the kept build would not hold next frame.
    if (m_MergedThisFrame)
        m_LastBuildInputs.Certified = false;
    ViewBatchState* view = m_Views.Find(viewId);
    if (!view)
        return;
    DeriveBatchKeysForView(*view, meshRegistry, materialColorClass, nullptr);
}

std::span<const WorldDrawBuilder::BatchKey> WorldDrawBuilder::GetBatchKeys(Rendering::ViewId viewId) const
{
    const ViewBatchState* view = m_Views.Find(viewId);
    if (!view)
        return {};
    return view->Keys;
}

std::span<const WorldSubmissionRecord> WorldDrawBuilder::GetSubmissions(Rendering::ViewId viewId) const
{
    const ViewBatchState* view = m_Views.Find(viewId);
    if (!view)
        return {};
    return view->Submissions;
}

std::span<const WorldDrawBuilder::BatchKey> WorldDrawBuilder::GetDeformingBatchKeys(
    Rendering::ViewId viewId) const
{
    const ViewBatchState* view = m_Views.Find(viewId);
    if (!view)
        return {};
    return view->DeformingKeys;
}

bool WorldDrawBuilder::HasShadowCastingSubmissions(Rendering::ViewId viewId) const
{
    const ViewBatchState* view = m_Views.Find(viewId);
    return view && view->ShadowCaster;
}

bool WorldDrawBuilder::HasTransmissiveCasterSubmissions(Rendering::ViewId viewId) const
{
    const ViewBatchState* view = m_Views.Find(viewId);
    return view && view->TransmissiveCaster;
}

bool WorldDrawBuilder::HasAnimatedVertexModifierSubmissions(Rendering::ViewId viewId) const
{
    const ViewBatchState* view = m_Views.Find(viewId);
    return view && view->AnimatedVertexModifier;
}

bool WorldDrawBuilder::HasAnimatedVertexModifierCasterSubmissions(Rendering::ViewId viewId) const
{
    const ViewBatchState* view = m_Views.Find(viewId);
    return view && view->AnimatedVertexModifierCaster;
}

WorldDrawBuilder::ShadowSubmissionStates WorldDrawBuilder::GetShadowSubmissionStates(
    Rendering::ViewId viewId) const
{
    const auto* view = m_Views.Find(viewId);
    return view ? view->ShadowSubmissions : ShadowSubmissionStates{};
}

void WorldDrawBuilder::DeriveBatchKeysForView(ViewBatchState& view,
                                              const Rendering::MeshGPURegistry& meshRegistry,
                                              std::span<const uint32_t> materialColorClass,
                                              std::vector<const Material*>* materialsRead)
{
    auto& keys = view.Keys;
    keys.clear();
    keys.reserve(view.Submissions.size());
    bool shadowCaster = false;
    bool transmissiveCaster = false;
    bool animatedVertexModifier = false;
    bool animatedVertexModifierCaster = false;
    ShadowSubmissionStates shadowSubmissions{};
    shadowSubmissions.All.Complete = true;
    shadowSubmissions.Rigid.Complete = true;
    FlatMap<const Material*, bool> shadowMaterialKinds;

    for (const auto& rec : view.Submissions)
    {
        const auto* entry = meshRegistry.Find(rec.meshHandle);
        const bool drawable = rec.material && rec.material->GetGraphicsPipelineId().IsValid() &&
                              entry && entry->gpuMeshIndex != ~0u && entry->indexCount != 0u;
        // A census of submitted casters, ahead of every drawability filter.
        shadowCaster = shadowCaster || (rec.flags & kSubmissionFlagCastShadows) != 0u;
        if ((rec.flags & kSubmissionFlagCastShadows) != 0u)
        {
            uint64_t identity = HashUtils::HashValue(0u, static_cast<uint64_t>(rec.meshHandle));
            identity = HashUtils::HashValue(identity, rec.instanceIndex);
            identity = HashUtils::HashValue(identity, rec.renderLayerMask);
            if (rec.material)
            {
                const auto& guid = rec.material->GetGuid().GetData();
                identity = HashUtils::Fnv1a64(guid.data(), guid.size(), identity);
            }
            const bool rigid = !entry || !Rendering::IsSkinned(entry->vertexFlags);
            AddShadowMembership(shadowSubmissions.All, identity, drawable);
            if (rigid)
                AddShadowMembership(shadowSubmissions.Rigid, identity, drawable);
            if (rec.material)
                shadowMaterialKinds.GetOrInsert(rec.material) |= rigid;
        }
        if (!rec.material)
            continue;
        const uint32_t materialIndex = rec.material->GetGpuSceneMaterialIndex();
        // Every material read below, once per build: the kept build re-checks them.
        if (materialsRead)
        {
            if (materialIndex == Material::kInvalidSSBOIndex)
                m_ReadUnindexedMaterial = true;
            else
            {
                if (materialIndex >= m_MaterialReadByIndex.size())
                    m_MaterialReadByIndex.resize(static_cast<size_t>(materialIndex) + 1u, nullptr);
                if (m_MaterialReadByIndex[materialIndex] != rec.material)
                {
                    // Two materials on one index cannot be told apart by index.
                    if (m_MaterialReadByIndex[materialIndex] != nullptr)
                        m_ReadUnindexedMaterial = true;
                    else
                        materialsRead->push_back(rec.material);
                    m_MaterialReadByIndex[materialIndex] = rec.material;
                }
            }
        }
        // Ahead of the pipeline and mesh-residency filters on purpose: this is a
        // census of what was submitted, not of what is drawable this frame. A
        // deforming caster whose mesh or pipeline is still pending invalidates
        // the caches from its first submitted frame, a re-render on frames that
        // already pay for the upload, rather than from the frame it first draws.
        // Either way the caches never hold a layer rasterized without a drawn
        // caster, because this derivation precedes every reader in the frame.
        // A caster implies a renderable, so the first one found ends the search.
        if (!animatedVertexModifierCaster && MaterialDeformsVertices(*rec.material))
        {
            animatedVertexModifier = true;
            animatedVertexModifierCaster = (rec.flags & kSubmissionFlagCastShadows) != 0u;
        }
        if (!rec.material->GetGraphicsPipelineId().IsValid())
            continue;
        if (!entry || entry->gpuMeshIndex == ~0u || entry->indexCount == 0u)
            continue;
        // bit 0 == castsShadows (ComputeInstanceFlags), the same bit the shadow
        // cull tests before an instance can reach a cascade stream.
        if (!transmissiveCaster && (rec.flags & 1u) != 0u &&
            Rendering::HasKeyword(rec.material->GetVariantKey().materialKeywords,
                                  Rendering::MaterialKeyword::Transmission))
            transmissiveCaster = true;
        keys.push_back(BatchKey{
            rec.material,
            rec.meshHandle,
            materialIndex,
            entry->gpuMeshIndex,
            ColorClassOf(materialIndex, materialColorClass)});
    }

    for (const auto& [material, rigid] : shadowMaterialKinds)
    {
        const auto& identity = material->GetGuid().GetData();
        uint64_t stamp = HashUtils::Fnv1a64(identity.data(), identity.size());
        stamp = HashUtils::HashValue(stamp, material->GetContentRevision());
        stamp = HashUtils::HashValue(stamp, material->GetVersion());
        shadowSubmissions.All.MaterialContentKey =
            HashUtils::HashCombine(shadowSubmissions.All.MaterialContentKey, stamp);
        if (rigid)
            shadowSubmissions.Rigid.MaterialContentKey =
                HashUtils::HashCombine(shadowSubmissions.Rigid.MaterialContentKey, stamp);
    }
    view.ShadowSubmissions = shadowSubmissions;

    // Sort by (colorClassId, meshIndex, materialIndex): grouping by
    // (colorClassId, meshIndex) makes the merge members contiguous for the
    // std::unique dedup below, and ordering materialIndex last makes the FIRST
    // survivor of each group the lowest-materialIndex member -- the
    // deterministic class representative (P2-d). With the merge off,
    // colorClassId == materialIndex, so this reduces to the old
    // (materialIndex, meshIndex) order exactly.
    std::sort(keys.begin(), keys.end(),
              [](const BatchKey& a, const BatchKey& b) {
                  if (a.colorClassId != b.colorClassId)
                      return a.colorClassId < b.colorClassId;
                  if (a.meshIndex != b.meshIndex)
                      return a.meshIndex < b.meshIndex;
                  return a.materialIndex < b.materialIndex;
              });
#if !defined(NDEBUG)
    for (size_t i = 1; i < keys.size(); ++i)
    {
        const auto& prev = keys[i - 1];
        const auto& cur = keys[i];
        // The sort must group by colorClassId so the (colorClassId, meshIndex)
        // dedup collapses ALL members of a class -- a non-monotonic sequence
        // would leave members non-adjacent and double-draw the merged range.
        assert(prev.colorClassId <= cur.colorClassId
               && "batch keys must be grouped by color class for the merge dedup");
        // Identity invariant preserved: a (materialIndex, meshIndex) pair still
        // maps to exactly one (Material*, mesh) -- materialIndex is owned by
        // Material, meshIndex by MeshGPURegistry -- and a material maps to a
        // single color class. Different materials sharing (colorClassId,
        // meshIndex) is the merge, NOT a violation.
        if (prev.materialIndex == cur.materialIndex && prev.meshIndex == cur.meshIndex)
        {
            assert((prev.material == cur.material && prev.mesh == cur.mesh
                    && prev.colorClassId == cur.colorClassId)
                   && "BatchKey (matIdx, meshIdx) maps to multiple (Material*/mesh/class)");
        }
    }
#endif
    // Collapse to one representative per (colorClassId, meshIndex): merged
    // opaque materials of a class draw the shared range once; identity keys
    // (merge off / material-dependent) keep their per-material row because
    // colorClassId == materialIndex there.
    keys.erase(std::unique(keys.begin(), keys.end(),
                           [](const BatchKey& a, const BatchKey& b) {
                               return a.colorClassId == b.colorClassId
                                   && a.meshIndex == b.meshIndex;
                           }),
               keys.end());

    view.ShadowCaster = shadowCaster;
    view.TransmissiveCaster = transmissiveCaster;
    view.AnimatedVertexModifier = animatedVertexModifier;
    view.AnimatedVertexModifierCaster = animatedVertexModifierCaster;

    // The deforming subset, taken after the dedup so it names the keys the
    // producer will actually record. A deforming material is never
    // colour-merge eligible (its depth class is MaterialDependent), so its
    // colorClassId equals its materialIndex and its row survives the dedup as
    // itself — the representative here is always the deforming material, never
    // a merge partner standing in for it.
    auto& deforming = view.DeformingKeys;
    deforming.clear();
    for (const auto& key : keys)
    {
        if (key.material && SupportsDeformationMotion(*key.material))
            deforming.push_back(key);
    }
}

void WorldDrawBuilder::ClearView(Rendering::ViewId viewId)
{
    m_Views.Erase(viewId);
    m_PreviousViews.Erase(viewId);
    m_LastBuildInputs.Certified = false;
}

void WorldDrawBuilder::Clear()
{
    m_Views.Clear();
    m_PreviousViews.Clear();
    m_LastBuildInputs.Certified = false;
}

} // namespace Engine::Renderer
} // namespace GameEngine
