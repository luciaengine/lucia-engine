// WorldDrawBuilder.cpp
#include "Engine/Rendering/WorldDrawBuilder.h"

#include "Engine/Rendering/Material.h"
#include "Engine/Rendering/MaterialDeformationClassify.h"
#include "Engine/Rendering/MaterialVertexAnimation.h"
#include "Engine/Rendering/MeshGPURegistry.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Rendering/Core/HashUtils.h"
#include "Rendering/Core/ThreadingUtils.h"
#include "Types/FlatMap.h"

#include <algorithm>

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
} // namespace

WorldDrawBuilder::WorldDrawBuilder(const JobSystem::WorkStealingThreadPool* jobSystem)
    : m_JobSystem(jobSystem),
      m_OffPoolBufferIndex(jobSystem ? jobSystem->GetWorkerCount() : 0),
      m_ThreadBuffers(m_OffPoolBufferIndex + 1)
{
}

void WorldDrawBuilder::BeginFrame()
{
    for (auto& [viewId, submissions] : m_ShadowSubmissionsByView)
        submissions = {};
    // Each submitting thread owns one buffer; buffers on separate lines keep
    // the threads from invalidating each other's records.
    static_assert(alignof(decltype(m_ThreadBuffers)::value_type) == kFalseSharingSeparation);

    for (FalseSharingPadded<ThreadSubmissionBuffer>& slot : m_ThreadBuffers)
    {
        slot.Value.records.clear();
        slot.Value.drained = 0;
    }

    // Clear merged data
    for (auto& [viewId, submissions] : m_SubmissionsByView)
        submissions.clear();
    for (auto& [viewId, keys] : m_BatchKeysByView)
        keys.clear();
    for (auto& [viewId, keys] : m_DeformingKeysByView)
        keys.clear();
    for (auto& [viewId, hasCaster] : m_ShadowCasterByView)
        hasCaster = false;
    for (auto& [viewId, hasCaster] : m_TransmissiveCasterByView)
        hasCaster = false;
    for (auto& [viewId, hasDeformer] : m_AnimatedVertexModifierByView)
        hasDeformer = false;
    for (auto& [viewId, hasDeformingCaster] : m_AnimatedVertexModifierCasterByView)
        hasDeformingCaster = false;
}

void WorldDrawBuilder::Submit(std::span<const WorldSubmissionRecord> records)
{
    if (records.empty())
        return;

    // A worker owns the buffer at its id. Off the pool the id is invalid
    // (SIZE_MAX), which clamps to the off-pool buffer.
    const size_t workerId = m_JobSystem ? m_JobSystem->GetCurrentWorkerId() : SIZE_MAX;
    auto& buf = m_ThreadBuffers[std::min(workerId, m_OffPoolBufferIndex)].Value.records;
    buf.insert(buf.end(), records.begin(), records.end());
}

void WorldDrawBuilder::MergeSubmissions()
{
    // Idempotent incremental drain: each buffer tracks how many records it has
    // already been drained for this frame, so late submissions (e.g. thumbnail
    // views submitted after the main scene pipeline has already built keys)
    // still land in m_SubmissionsByView on the next MergeSubmissions() call.
    for (FalseSharingPadded<ThreadSubmissionBuffer>& slot : m_ThreadBuffers)
    {
        ThreadSubmissionBuffer& buf = slot.Value;
        const uint32 total = static_cast<uint32>(buf.records.size());
        for (uint32 r = buf.drained; r < total; ++r)
        {
            const auto& rec = buf.records[r];
            m_SubmissionsByView[rec.viewId].push_back(rec);
        }
        buf.drained = total;
    }
}

uint32_t WorldDrawBuilder::BuildBatchKeys(const Rendering::MeshGPURegistry& meshRegistry,
                                          std::span<const uint32_t> materialColorClass)
{
    MergeSubmissions();

    for (auto& [viewId, submissions] : m_SubmissionsByView)
        DeriveBatchKeysForView(viewId, submissions, meshRegistry, materialColorClass);

    return static_cast<uint32_t>(m_SubmissionsByView.size());
}

void WorldDrawBuilder::BuildBatchKeysForView(Rendering::ViewId viewId,
                                             const Rendering::MeshGPURegistry& meshRegistry,
                                             std::span<const uint32_t> materialColorClass)
{
    MergeSubmissions();
    auto itSub = m_SubmissionsByView.find(viewId);
    if (itSub == m_SubmissionsByView.end())
    {
        m_ShadowSubmissionsByView.Erase(viewId);
        m_BatchKeysByView.erase(viewId);
        m_ShadowCasterByView.erase(viewId);
        m_TransmissiveCasterByView.erase(viewId);
        m_AnimatedVertexModifierByView.erase(viewId);
        m_AnimatedVertexModifierCasterByView.erase(viewId);
        return;
    }
    DeriveBatchKeysForView(viewId, itSub->second, meshRegistry, materialColorClass);
}

std::span<const WorldDrawBuilder::BatchKey> WorldDrawBuilder::GetBatchKeys(Rendering::ViewId viewId) const
{
    auto it = m_BatchKeysByView.find(viewId);
    if (it == m_BatchKeysByView.end() || it->second.empty())
    {
        return {};
    }
    const auto& vec = it->second;
    return std::span<const BatchKey>(vec.data(), vec.size());
}

std::span<const WorldSubmissionRecord> WorldDrawBuilder::GetSubmissions(Rendering::ViewId viewId) const
{
    auto it = m_SubmissionsByView.find(viewId);
    if (it == m_SubmissionsByView.end() || it->second.empty())
        return {};
    const auto& vec = it->second;
    return std::span<const WorldSubmissionRecord>(vec.data(), vec.size());
}

std::span<const WorldDrawBuilder::BatchKey> WorldDrawBuilder::GetDeformingBatchKeys(
    Rendering::ViewId viewId) const
{
    auto it = m_DeformingKeysByView.find(viewId);
    if (it == m_DeformingKeysByView.end() || it->second.empty())
        return {};
    const auto& vec = it->second;
    return std::span<const BatchKey>(vec.data(), vec.size());
}

bool WorldDrawBuilder::HasShadowCastingSubmissions(Rendering::ViewId viewId) const
{
    const auto it = m_ShadowCasterByView.find(viewId);
    return it != m_ShadowCasterByView.end() && it->second;
}

bool WorldDrawBuilder::HasTransmissiveCasterSubmissions(Rendering::ViewId viewId) const
{
    const auto it = m_TransmissiveCasterByView.find(viewId);
    return it != m_TransmissiveCasterByView.end() && it->second;
}

bool WorldDrawBuilder::HasAnimatedVertexModifierSubmissions(Rendering::ViewId viewId) const
{
    const auto it = m_AnimatedVertexModifierByView.find(viewId);
    return it != m_AnimatedVertexModifierByView.end() && it->second;
}

bool WorldDrawBuilder::HasAnimatedVertexModifierCasterSubmissions(Rendering::ViewId viewId) const
{
    const auto it = m_AnimatedVertexModifierCasterByView.find(viewId);
    return it != m_AnimatedVertexModifierCasterByView.end() && it->second;
}

void WorldDrawBuilder::DeriveBatchKeysForView(
    Rendering::ViewId viewId,
    const std::vector<WorldSubmissionRecord>& submissions,
    const Rendering::MeshGPURegistry& meshRegistry,
    std::span<const uint32_t> materialColorClass)
{
    auto& keys = m_BatchKeysByView[viewId];
    keys.clear();
    keys.reserve(submissions.size());
    bool shadowCaster = false;
    bool transmissiveCaster = false;
    bool animatedVertexModifier = false;
    bool animatedVertexModifierCaster = false;
    ShadowSubmissionStates shadowSubmissions{};
    shadowSubmissions.All.Complete = true;
    shadowSubmissions.Rigid.Complete = true;
    FlatMap<const Material*, bool> shadowMaterialKinds;

    for (const auto& rec : submissions)
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
        // Deformation is a property of submitted content, including pending draws.
        // The prepared depth producer separately proves that every caster drew.
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
        const uint32_t materialIndex = rec.material->GetGpuSceneMaterialIndex();
        // colorClassId == materialIndex when the merge is off (empty map) or the
        // material is not merge-eligible (identity entry) -- so the OFF path is
        // exactly the old (materialIndex, meshIndex) key.
        const uint32_t colorClassId = materialIndex < materialColorClass.size()
                                          ? materialColorClass[materialIndex]
                                          : materialIndex;
        keys.push_back(BatchKey{
            rec.material,
            rec.meshHandle,
            materialIndex,
            entry->gpuMeshIndex,
            colorClassId});
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
    m_ShadowSubmissionsByView.GetOrInsert(viewId) = shadowSubmissions;

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

    m_ShadowCasterByView[viewId] = shadowCaster;
    m_TransmissiveCasterByView[viewId] = transmissiveCaster;
    m_AnimatedVertexModifierByView[viewId] = animatedVertexModifier;
    m_AnimatedVertexModifierCasterByView[viewId] = animatedVertexModifierCaster;

    // The deforming subset, taken after the dedup so it names the keys the
    // producer will actually record. A deforming material is never
    // colour-merge eligible (its depth class is MaterialDependent), so its
    // colorClassId equals its materialIndex and its row survives the dedup as
    // itself — the representative here is always the deforming material, never
    // a merge partner standing in for it.
    auto& deforming = m_DeformingKeysByView[viewId];
    deforming.clear();
    for (const auto& key : keys)
    {
        if (key.material && SupportsDeformationMotion(*key.material))
            deforming.push_back(key);
    }
}

void WorldDrawBuilder::ClearView(Rendering::ViewId viewId)
{
    m_ShadowSubmissionsByView.Erase(viewId);
    m_SubmissionsByView.erase(viewId);
    m_BatchKeysByView.erase(viewId);
    m_DeformingKeysByView.erase(viewId);
    m_ShadowCasterByView.erase(viewId);
    m_TransmissiveCasterByView.erase(viewId);
    m_AnimatedVertexModifierByView.erase(viewId);
    m_AnimatedVertexModifierCasterByView.erase(viewId);
}

void WorldDrawBuilder::Clear()
{
    m_ShadowSubmissionsByView.Clear();
    m_SubmissionsByView.clear();
    m_BatchKeysByView.clear();
    m_DeformingKeysByView.clear();
    m_ShadowCasterByView.clear();
    m_TransmissiveCasterByView.clear();
    m_AnimatedVertexModifierByView.clear();
    m_AnimatedVertexModifierCasterByView.clear();
}

WorldDrawBuilder::ShadowSubmissionStates WorldDrawBuilder::GetShadowSubmissionStates(
    Rendering::ViewId viewId) const
{
    const auto* found = m_ShadowSubmissionsByView.Find(viewId);
    return found ? *found : ShadowSubmissionStates{};
}

} // namespace Engine::Renderer
} // namespace GameEngine
