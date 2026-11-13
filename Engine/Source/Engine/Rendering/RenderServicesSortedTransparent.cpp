// The main view's sorted transparent pass draws artwork in authored order and
// back-to-front by view depth. Only adjacent compatible meshes share a draw.

#include "Engine/Rendering/RenderServices.h"

#include "Engine/Rendering/Material.h"
#include "Engine/Rendering/RenderOrigin.h"
#include "Engine/Rendering/SortedTransparentCull.h"
#include "Engine/Rendering/WorldDrawTypes.h"
#include "Rendering/Camera/CameraDerivation.h"
#include "Rendering/Common/Frustum.h"
#include "Rendering/Core/GPUScene.h"

#include "Logger/Logger.h"
#include "Memory/ScratchScope.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <limits>
#include <string>
#include <vector>
#include "RenderServicesDetail.h"

namespace GameEngine
{
namespace Engine::Renderer
{
using namespace ::GameEngine::Rendering;

namespace
{
constexpr uint32_t kIndirectCommandStride = 5u * sizeof(uint32_t); // VkDrawIndexedIndirectCommand

// firstInstance selects the matching entry in the sorted instance-index list.
inline void WriteIndirectCommand(uint32_t* dst, const SortedTransparentRecord& rec, uint32_t slot)
{
    dst[0] = rec.IndexCount;
    dst[1] = 1u; // instanceCount: one record = one instance
    dst[2] = rec.FirstIndex;
    dst[3] = static_cast<uint32_t>(rec.VertexOffset);
    dst[4] = slot; // firstInstance -> gl_InstanceIndex -> indirection slot
}
} // namespace

void RenderServices::BuildSortedTransparentForView(Rendering::ViewId viewId)
{
    const uint64_t frame = m_FrameOrchestrator.WorldFrameEpoch();
    SortedTransparentView& holder = m_SortedTransparentByView[viewId];
    if (holder.Frame == frame)
        return; // already built this frame

    // Captured before the reset: a false->true edge (the path engaging after
    // any build that staged nothing) is a loggable transition even when the
    // composition matches the last one logged.
    const bool stagedLastBuild = holder.StagedLastBuild;

    holder.Frame = frame;
    const auto& contributors = m_ViewRegistry.PerView(viewId).SortedTransparentCommands;
    holder.Active = !contributors.empty();
    holder.StagedLastBuild = false;
    holder.LoggedBuildTransition = false;
    holder.Records.clear();
    holder.Runs.clear();
    holder.Partition.Clear();
    holder.Commands.clear();
    holder.CompositionMeshOrder.clear();
    holder.CompositionSlots.clear();
    holder.Stats = {};

    // Empty-set early-out: a view with no submissions pays nothing.
    const auto submissions = m_WorldDrawBuilder.GetSubmissions(viewId);
    if (submissions.empty())
        return;

    Rendering::GPUScene* scene = GetGPUScene();
    if (!scene)
        return;
    // Full-profile meshes use the buffer-reference path. Compatibility meshes
    // instead use the existing CPU instance list and direct indexed draws.
    if (!m_Device || (!m_Profile.IsCompat() && !m_Device->GetCapabilities().supportsBufferDeviceAddress))
        return;
    const std::vector<Rendering::GPUInstance>& instances = scene->GetInstances();
    const auto& meshRegistry = GetMeshGPURegistry();

    // Collect ORDER-DEPENDENT Blend instances. Transmission glass is coerced
    // to Opaque alpha mode (MaterialRegistry), so filtering on Blend naturally
    // leaves the transmissive pass untouched; order-independent blends
    // (additive/multiply) stay on the batched path. The filter MUST match the
    // peel predicate in recordEntityBatch and the extraction class bit exactly
    // (Risk 3) — all three use IsOrderDependentBlendMaterial. Candidate Key
    // indexes `pending`.
    Memory::ScratchScope scratch;
    struct PendingRecord
    {
        uint32_t InstanceIndex;
        const Material* Mat;
        Rendering::MeshGPUHandle Mesh;
        const Rendering::MeshGPUEntry* Entry;
        RenderSortingData Sorting;
        ECS::EntityHandle Entity;
    };
    // The cull runs in the frame of the view's render origin sector: the planes come from
    // viewProjRel and each sector-local center is composed against the sector, as the GPU cull does.
    const Rendering::CameraData cam = m_ViewRegistry.ResolveCameraData(viewId);
    const Mathematics::Vector3Int originSector = CameraRenderOriginSector(cam);
    std::pmr::vector<PendingRecord> pending{&scratch.Resource()};
    std::pmr::vector<TransparentSphere> candidates{&scratch.Resource()};
    pending.reserve(submissions.size());
    candidates.reserve(submissions.size());
    for (const WorldSubmissionRecord& sub : submissions)
    {
        if (!sub.material || !IsOrderDependentBlendMaterial(*sub.material))
            continue;
        if (sub.instanceIndex >= instances.size())
            continue; // not resident in GPUScene yet (first frame)
        // Mesh entry must be resident to stage draw args; a not-yet-uploaded
        // mesh is skipped exactly as the batched path skips it (recordEntityBatch's
        // mesh-entry-missing guard) — a one-frame absence, not a vanish.
        const Rendering::MeshGPUEntry* entry = meshRegistry.Find(sub.meshHandle);
        if (!entry || entry->indexCount == 0)
            continue;
        const Rendering::GPUInstance& inst = instances[sub.instanceIndex];
        candidates.push_back({static_cast<uint32_t>(pending.size()),
                              Rendering::SectorLocalPointRelativeTo(inst.boundingCenter, inst.sectorPacked[0],
                                                                    inst.sectorPacked[1], originSector),
                              inst.boundingRadius});
        pending.push_back({sub.instanceIndex, sub.material, sub.meshHandle, entry, sub.Sorting, sub.SortingOwner});
    }
    if (candidates.empty())
        return;

    const Rendering::CameraDerivedData camDerived =
        Rendering::DeriveCameraData(cam, m_ViewRegistry.ResolveCameraRanges(viewId));
    Vector4 frustumPlanes[6];
    Rendering::ExtractFrustumPlanes(cam.viewProjRel, frustumPlanes);

    std::pmr::vector<TransparentCentroid> visible{&scratch.Resource()};
    visible.reserve(candidates.size());
    const uint32_t culledCount =
        CullTransparentsToFrustum(candidates, frustumPlanes, visible);
    if (visible.empty())
        return; // everything off-screen; the batched path GPU-culls it identically

    // Stage records + discover runs. A run groups records whose PSO identity
    // (SortedTransparentRunMaterialKey) AND geometry-pool identity
    // (SortedTransparentRunGeometryKey) match — one representative then binds
    // for the whole run. Bindless instances fetch their own material; compatibility
    // also requires the same bound material texture set. Group count is small, so
    // linear key scans beat any map.
    struct RunKeyEntry
    {
        SortedTransparentRunMaterialKey MatKey;
        SortedTransparentRunGeometryKey GeoKey;
    };
    std::vector<RunKeyEntry> runKeys;
    // Per-unique-material key cache (materials repeat heavily across records).
    std::vector<std::pair<const Material*, SortedTransparentRunMaterialKey>> matKeyCache;


    std::pmr::vector<RenderSortKey> sortingKeys{&scratch.Resource()};
    std::pmr::vector<ECS::EntityHandle> sortingEntities{&scratch.Resource()};
    sortingKeys.reserve(visible.size());
    sortingEntities.reserve(visible.size());
    holder.Records.reserve(visible.size());
    const Rendering::Vector3 camPos = CameraEyeRelativeToOrigin(cam);
    const Rendering::Vector3 camFwd = camDerived.Forward;
    const Mathematics::WorldPosition cameraPosition = CameraEye(cam);
    const float cameraForward[]{camFwd.x, camFwd.y, camFwd.z};
    for (const TransparentCentroid& vis : visible)
    {
        const PendingRecord& p = pending[vis.Key];

        const SortedTransparentRunMaterialKey* matKey = nullptr;
        for (const auto& [mat, key] : matKeyCache)
        {
            if (mat == p.Mat)
            {
                matKey = &key;
                break;
            }
        }
        if (!matKey)
        {
            matKeyCache.emplace_back(p.Mat, MakeSortedTransparentRunMaterialKey(*p.Mat, m_Profile.IsCompat()));
            matKey = &matKeyCache.back().second;
        }

        SortedTransparentRunGeometryKey geoKey{};
        // Refused before the run key is built, never after: a run key made from
        // all-invalid bindings would compare EQUAL across two different
        // non-drawable meshes and collapse them into one run.
        if (!meshRegistry.TryGetDrawableBindings(*p.Entry, geoKey.Bindings))
            continue;
        geoKey.Topology = p.Entry->topology;
        geoKey.VertexFlags = p.Entry->vertexFlags;

        uint32_t group = static_cast<uint32_t>(runKeys.size());
        for (uint32_t g = 0; g < runKeys.size(); ++g)
        {
            if (runKeys[g].MatKey == *matKey && runKeys[g].GeoKey == geoKey)
            {
                group = g;
                break;
            }
        }
        if (group == runKeys.size())
        {
            runKeys.push_back({*matKey, geoKey});
            holder.Runs.push_back({p.Mat, p.Mesh});
        }

        const float viewDepth = (vis.Center.x - camPos.x) * camFwd.x +
                                (vis.Center.y - camPos.y) * camFwd.y +
                                (vis.Center.z - camPos.z) * camFwd.z;
        holder.Records.push_back({p.InstanceIndex, p.Entry->indexCount, p.Entry->firstIndex,
                                  static_cast<int32_t>(p.Entry->vertexOffset), group, viewDepth});
        sortingEntities.push_back(p.Entity);
        sortingKeys.push_back(MakeRenderSortKey(p.Sorting, viewDepth, cameraPosition, cameraForward));
    }

    BuildTransparentComposition(holder.Records, sortingKeys, sortingEntities, contributors,
                                scratch.Resource(), holder.Partition, holder.CompositionMeshOrder,
                                holder.CompositionSlots, m_Profile.IsCompat());
    holder.CameraPos = camPos;
    holder.CameraForward = camFwd;
    holder.OriginSector = originSector;
    holder.Stats.Collected = static_cast<uint32_t>(candidates.size());
    holder.Stats.Culled = culledCount;
    holder.Stats.Records = static_cast<uint32_t>(holder.Records.size());
    holder.Stats.Runs = static_cast<uint32_t>(holder.Partition.RunCount.size());
    holder.Active = true;
    holder.StagedLastBuild = true;

    // Log on TRANSITION only — engagement or a composition change — never per
    // steady-state frame; that convention is what keeps the editor console
    // (Debug level) readable with Blend water/glass permanently on screen.
    // The level check first so shipped builds (GlobalMinLevel=Info) and
    // steady-state frames both skip every byte of string work below.
    if (Logger::ShouldLog(Logger::LogLevel::Debug, Logger::Log::GetLogLevel()) &&
        (!stagedLastBuild || holder.Stats != holder.LastLoggedStats))
    {
        holder.LastLoggedStats = holder.Stats;
        holder.LoggedBuildTransition = true;

        // The collected / culled / staged / run composition that shows how the
        // sorted path engaged.
        Logger::Log::Debug(
            "SortedTransparent: view {} collected {} order-dependent Blend candidates, "
            "culled {} (frustum), staged {} records in {} runs.",
            static_cast<uint32_t>(viewId), holder.Stats.Collected, holder.Stats.Culled,
            holder.Stats.Records, holder.Stats.Runs);

        const uint32_t slotCount = static_cast<uint32_t>(holder.Partition.RunCount.size());
        // Alternating materials can produce one run per visible mesh.
        constexpr uint32_t kMaxLoggedRuns = 12;
        const uint32_t loggedRuns = std::min(slotCount, kMaxLoggedRuns);
        std::string runRanges;
        for (uint32_t slot = 0; slot < loggedRuns; ++slot)
        {
            float minDepth = std::numeric_limits<float>::max();
            float maxDepth = std::numeric_limits<float>::lowest();
            const uint32_t start = holder.Partition.RunStart[slot];
            for (uint32_t i = 0; i < holder.Partition.RunCount[slot]; ++i)
            {
                const float depth = holder.Records[holder.CompositionMeshOrder[start + i]].ViewDepth;
                minDepth = std::min(minDepth, depth);
                maxDepth = std::max(maxDepth, depth);
            }
            runRanges += slot ? " r" : "r";
            runRanges += std::to_string(slot) + "[" + std::to_string(minDepth) + "," +
                         std::to_string(maxDepth) + "]x" +
                         std::to_string(holder.Partition.RunCount[slot]);
        }
        if (slotCount > loggedRuns)
            runRanges += " ... (" + std::to_string(slotCount - loggedRuns) + " more)";
        Logger::Log::Debug("SortedTransparent: view {} run depth ranges: {}",
                           static_cast<uint32_t>(viewId), runRanges);
    }
}

bool RenderServices::IsSortedTransparentActive(Rendering::ViewId viewId) const
{
    const uint64_t frame = m_FrameOrchestrator.WorldFrameEpoch();
    const auto it = m_SortedTransparentByView.find(viewId);
    // Guard on the build frame so a stale Active never leaks to a later frame or to
    // a view the world node did not (re)build this frame.
    return it != m_SortedTransparentByView.end() && it->second.Frame == frame && it->second.Active;
}

void RenderServices::AddSortedTransparentDrainForView(Rendering::RenderGraph::RGFrame& frame,
                                                      Rendering::ViewId viewId,
                                                      const WorldPassTargetsRG& targets,
                                                      Rendering::MaterialKeyword passKeywords)
{
    namespace RenderGraph = Rendering::RenderGraph;

    float bandStart = 0.0f, bandEnd = 0.0f;
    int distanceMode = 0;
    if (GetDepthPixelationBandRange(viewId, bandStart, bandEnd, distanceMode))
        passKeywords |= Rendering::MaterialKeyword::DepthPixelationBand;

    const auto it = m_SortedTransparentByView.find(viewId);
    if (it == m_SortedTransparentByView.end() || it->second.Frame != m_FrameOrchestrator.WorldFrameEpoch() ||
        !it->second.Active)
        return;
    SortedTransparentView& holder = it->second;

    const auto& contributors = m_ViewRegistry.PerView(viewId).SortedTransparentCommands;
    const auto& viewResources = m_ViewRegistry.PerView(viewId);
    const std::span<const RenderGraph::RGTexture> contributorTextures =
        viewResources.ForwardSampledFor.IsFor(frame)
            ? std::span<const RenderGraph::RGTexture>(viewResources.ForwardSampledRG)
            : std::span<const RenderGraph::RGTexture>{};
    Memory::ScratchScope compositionScratch;
    std::pmr::vector<ForwardBufferReadRG> contributorBuffers{&compositionScratch.Resource()};
    for (const auto& read : viewResources.ForwardSampledBufferRG)
        if (read.For.IsFor(frame))
            contributorBuffers.push_back({read.Buffer, read.Access});
    if (holder.Records.empty() && !contributors.empty())
    {
        BuildTransparentComposition({}, {}, {}, contributors, compositionScratch.Resource(), holder.Partition,
                                    holder.CompositionMeshOrder, holder.CompositionSlots, m_Profile.IsCompat());
        holder.Commands.clear();
        for (const auto& slot : holder.CompositionSlots)
            holder.Commands.push_back(contributors[slot.Index].Command);
        AddForwardCommandPassForView(frame, viewId, targets, passKeywords, holder.Commands, contributorTextures,
                                     "SortedTransparent", contributorBuffers);
        return;
    }

    // Any declare-time bail below MUST hand the view's Blend draws back to the
    // batched path: Active is read by the opaque peel at EXEC time (after this
    // declaration), so flipping it here restores the unsorted-but-visible
    // batched draws in the SAME frame — a bail with Active left true would
    // vanish every sorted transparent silently (S2 review R1-F2). Warn once
    // per episode (the flag clears on the next successful drain), matching
    // S1's never-silent fallback contract.
    auto bailToBatched = [&](const char* reason)
    {
        holder.Active = false;
        if (!holder.WarnedDrainBail)
        {
            holder.WarnedDrainBail = true;
            Logger::Log::Warning(
                "SortedTransparent: view {} drain could not declare ({}); Blend draws "
                "fall back to the unsorted batched path until it recovers.",
                static_cast<uint32_t>(viewId), reason);
        }
    };

    if (holder.Records.empty() || !m_Device)
    {
        bailToBatched(!m_Device ? "no device" : "no staged records");
        return;
    }
    Rendering::GPUScene* scene = GetGPUScene();
    if (!scene || !scene->GetInstanceBuffer().IsValid())
    {
        bailToBatched("GPUScene instance buffer unavailable");
        return;
    }

    if (m_Profile.IsCompat())
    {
        const size_t bytes = holder.CompositionMeshOrder.size() * sizeof(uint32_t);
        const auto indices = frame.AllocUpload(bytes);
        if (!indices.Valid())
        {
            bailToBatched("ordered instance-list upload failed");
            return;
        }
        auto* list = static_cast<uint32_t*>(indices.Ptr);
        for (size_t index = 0; index < holder.CompositionMeshOrder.size(); ++index)
            list[index] = holder.Records[holder.CompositionMeshOrder[index]].InstanceIndex;
        holder.CompositionIndices = {HashStringId(CpuDrawStreamBuilder::kIndexListBindingName),
                                     indices.Buffer, indices.Offset, bytes};
        holder.PcBytes.resize(holder.Partition.RunCount.size() * sizeof(uint32_t));
        holder.Commands.clear();
        holder.Commands.reserve(holder.CompositionSlots.size());
        for (const auto& slot : holder.CompositionSlots)
        {
            if (slot.Contributor)
            {
                holder.Commands.push_back(contributors[slot.Index].Command);
                continue;
            }
            const uint32_t first = holder.Partition.RunStart[slot.Index];
            const auto& record = holder.Records[holder.CompositionMeshOrder[first]];
            const auto& run = holder.Runs[holder.Partition.GroupOfSlot[slot.Index]];
            auto* push = holder.PcBytes.data() + slot.Index * sizeof(uint32_t);
            std::memcpy(push, &first, sizeof(first));
            DrawCommand command{};
            command.Material = run.Representative;
            command.Geometry.Mesh = run.Mesh;
            command.IndexCount = record.IndexCount;
            command.FirstIndex = record.FirstIndex;
            command.VertexOffset = record.VertexOffset;
            command.InstanceCount = holder.Partition.RunCount[slot.Index];
            command.Bindings.Buffers = std::span(&holder.CompositionIndices, 1);
            command.Bindings.PushConstants = std::span<const std::byte>(push, sizeof(first));
            holder.Commands.push_back(command);
        }
        holder.WarnedDrainBail = false;
        AddForwardCommandPassForView(frame, viewId, targets, passKeywords, holder.Commands,
                                     contributorTextures, "SortedTransparent", contributorBuffers);
        return;
    }

    const uint32_t liveCount = static_cast<uint32_t>(holder.Records.size());
    const uint32_t runCount = static_cast<uint32_t>(holder.Partition.RunCount.size());
    // The CPU knows the final order before it can select a pipeline for each run.
    // Write the indirect commands and instance list in that same order.
    const uint64_t argsBytes = static_cast<uint64_t>(liveCount) * kIndirectCommandStride;
    const uint64_t countsBytes = static_cast<uint64_t>(runCount) * sizeof(uint32_t);
    const uint64_t indirectionBytes = static_cast<uint64_t>(liveCount) * sizeof(uint32_t);
    const uint64_t neededBytes = argsBytes + countsBytes + indirectionBytes;

    const uint32_t frameSlot = m_Device->GetFrameIndex();
    if (!holder.CpuDrainBuffers[frameSlot].IsValid() ||
        holder.CpuDrainCapacityBytes[frameSlot] < neededBytes)
    {
        if (holder.CpuDrainBuffers[frameSlot].IsValid())
            m_Device->DestroyBuffer(holder.CpuDrainBuffers[frameSlot]); // deferred by the device
        const uint64_t capacity =
            std::max<uint64_t>(neededBytes, holder.CpuDrainCapacityBytes[frameSlot] * 2u);
        BufferDesc desc{};
        desc.size = capacity;
        desc.usage = static_cast<uint32_t>(BufferUsage::Storage) |
                     static_cast<uint32_t>(BufferUsage::Indirect) |
                     static_cast<uint32_t>(BufferUsage::ShaderDeviceAddress);
        desc.memoryUsage = BufferMemoryUsage::Upload;
        // FrameSlotted: one element of a ring exactly as deep as the device paces.
        // Written only from AddSortedTransparentDrainForView, which runs behind
        // BeginFrame; a write from anywhere earlier would race a frame in flight.
        desc.flags = BufferCreateFlags::FrameSlotted;
        const std::string name =
            "SortedTransparent.CpuDrain.View" + std::to_string(static_cast<uint32_t>(viewId)) +
            ".Slot" + std::to_string(frameSlot);
        desc.debugName = name.c_str();
        holder.CpuDrainBuffers[frameSlot] = m_Device->CreateBuffer(desc);
        holder.CpuDrainCapacityBytes[frameSlot] = holder.CpuDrainBuffers[frameSlot].IsValid() ? capacity : 0;
    }
    if (!holder.CpuDrainBuffers[frameSlot].IsValid())
    {
        bailToBatched("CPU drain buffer creation failed");
        return;
    }

    auto* mapped = static_cast<uint8_t*>(m_Device->MapBuffer(holder.CpuDrainBuffers[frameSlot]));
    if (!mapped)
    {
        bailToBatched("CPU drain buffer map failed");
        return;
    }
    auto* args = reinterpret_cast<uint32_t*>(mapped);
    auto* counts = reinterpret_cast<uint32_t*>(mapped + argsBytes);
    auto* indirection = reinterpret_cast<uint32_t*>(mapped + argsBytes + countsBytes);
    for (uint32_t pos = 0; pos < liveCount; ++pos)
    {
        const SortedTransparentRecord& rec = holder.Records[holder.CompositionMeshOrder[pos]];
        WriteIndirectCommand(args + pos * 5u, rec, pos);
        indirection[pos] = rec.InstanceIndex;
    }
    for (uint32_t r = 0; r < runCount; ++r)
        counts[r] = holder.Partition.RunCount[r];
    m_Device->UnmapBuffer(holder.CpuDrainBuffers[frameSlot]);

    const Rendering::BufferHandle stream = holder.CpuDrainBuffers[frameSlot];
    uint64_t indirectionAddr = m_Device->GetBufferDeviceAddress(stream);
    if (indirectionAddr == 0)
    {
        bailToBatched("CPU drain buffer has no device address");
        return;
    }
    indirectionAddr += argsBytes + countsBytes;

    // Stage one indirect draw per run, in composition order.
    // All runs share the 16-byte GE_INSTANCED push-constant block: the drain's
    // indirection buffer + GPUScene's per-frame instance buffer, both by
    // device address.
    ComposedPCInstanced pcInst{};
    pcInst.indirectionAddr = indirectionAddr;
    pcInst.gpuInstancesAddr = m_Device->GetBufferDeviceAddress(scene->GetInstanceBuffer());
    if (pcInst.gpuInstancesAddr == 0)
    {
        bailToBatched("GPUScene instance buffer has no device address");
        return;
    }
    holder.PcBytes.resize(sizeof(ComposedPCInstanced));
    std::memcpy(holder.PcBytes.data(), &pcInst, sizeof(ComposedPCInstanced));

    holder.Commands.clear();
    holder.Commands.reserve(runCount + contributors.size());
    const uint32_t commandCount = static_cast<uint32_t>(holder.CompositionSlots.size());
    for (uint32_t commandIndex = 0; commandIndex < commandCount; ++commandIndex)
    {
        if (holder.CompositionSlots[commandIndex].Contributor)
        {
            holder.Commands.push_back(contributors[holder.CompositionSlots[commandIndex].Index].Command);
            continue;
        }
        const uint32_t slot = holder.CompositionSlots[commandIndex].Index;
        const SortedTransparentView::Run& run = holder.Runs[holder.Partition.GroupOfSlot[slot]];
        DrawCommand cmd{};
        cmd.Material = run.Representative;
        cmd.Geometry.Mesh = run.Mesh;
        cmd.UseIndirect = true;
        cmd.IndirectCommandBuffer = stream;
        cmd.IndirectCountBuffer = stream;
        cmd.IndirectMaxDrawCount = holder.Partition.RunCount[slot];
        cmd.IndirectStride = kIndirectCommandStride;
        cmd.IndirectCommandOffset =
            static_cast<size_t>(holder.Partition.RunStart[slot]) * kIndirectCommandStride;
        cmd.IndirectCountOffset = argsBytes + static_cast<size_t>(slot) * sizeof(uint32_t);
        cmd.Bindings.PushConstants =
            std::span<const std::byte>(holder.PcBytes.data(), holder.PcBytes.size());
        holder.Commands.push_back(cmd);
    }

    // Captured before the clear: recovery from a bail episode is a transition
    // the drain line announces (the episode's start already warned once).
    const bool recoveredFromBail = holder.WarnedDrainBail;
    holder.WarnedDrainBail = false; // successful declare ends any bail episode

    if (holder.LoggedBuildTransition || recoveredFromBail)
        Logger::Log::Debug("SortedTransparent: view {} issued {} mesh runs over {} records.",
                           static_cast<uint32_t>(viewId), runCount, liveCount);

    AddForwardCommandPassForView(frame, viewId, targets, passKeywords, holder.Commands, contributorTextures,
                                 "SortedTransparent", contributorBuffers);
}

} // namespace Engine::Renderer
} // namespace GameEngine
