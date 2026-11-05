#pragma once

#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/RenderGraph/RGFrame.h" // pending keys use RGFrame identity

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace GameEngine::Rendering::RenderGraph
{

/// Controls whether a new write may replace an unread result.
enum class RGReadbackRetention : uint8_t
{
    SupersedeUnread,
    PreserveUnread
};

// GPU->CPU readback ring: N host-readable buffers written on rotating slots by
// a copy pass or a storage-writing dispatch, read back once the writing frame's
// submission has provably completed. Centralizes the machinery every
// hand-rolled readback re-derived (slot rotation, completion tracking, and the
// payload metadata stamped at schedule time — tile origin/size — that must
// travel WITH the data).
//
// Completion contract: each BeginWrite registers a pending keyed by the
// declaring frame's identity; the frame's post-submit stamp
// (OnFrameSubmitted) attaches the submission's timeline-semaphore token, and a
// slot becomes readable when that token has signaled. Any write cadence is
// valid — sparse, bursty, or quiescent-with-gaps — there is no warmup window
// and nothing to reset. Size the ring to frames-in-flight + 2 so a reused
// slot's previous write has always retired.
/// The owner serializes reservation, recording, submission and mapping calls.
/// Mapped data belongs to the ring and is valid until Unmap or Destroy.
template <class Payload>
class RGReadbackRing
{
  public:
    // `desc.size` is the per-slot byte size; the desc is the caller's contract
    // with how slots are WRITTEN — TransferDst for a copy-pass readback, plus
    // Storage when a compute dispatch writes slots directly. The memory class
    // is Readback either way: every slot is mapped and copied out on the CPU
    // frame path, and Readback is the class that asks for a host-cached type.
    // Upload asks for write-combined memory, which is far slower to read and
    // can resolve into BAR VRAM. The class never constrained vkUsage — that is
    // built from BufferDesc::usage — so Storage and Readback compose on the
    // native backends. WebGPU is the exception: MapRead composes with CopyDst
    // and nothing else, so a Storage slot cannot be the buffer the CPU maps.
    // Where the device reports !supportsMappableStorageBuffers the ring splits
    // each slot in two — a device-local buffer the dispatch writes, and the
    // mappable slot RecordResolve copies it into. Callers see one handle from
    // BeginWrite on every backend. The ring only owns rotation, completion,
    // and payload carriage. Creates `count` slots; false on any failure (the
    // ring is Destroyed and left uninitialized).
    bool Init(IDevice* device, const BufferDesc& desc, uint32_t count,
              RGReadbackRetention retention = RGReadbackRetention::SupersedeUnread)
    {
        Destroy(device);
        if (!device || count == 0)
            return false;
        m_Device = device;
        m_Retention = retention;

        const bool splitSlots =
            (desc.usage & static_cast<uint32_t>(BufferUsage::Storage)) != 0
            && !device->GetCapabilities().supportsMappableStorageBuffers;

        BufferDesc readDesc = desc;
        BufferDesc writeDesc = desc;
        const std::string writeName =
            std::string(desc.debugName ? desc.debugName : "RGReadbackRing") + ".Write";
        if (splitSlots)
        {
            // The mappable half only ever receives a copy.
            readDesc.usage = static_cast<uint32_t>(BufferUsage::TransferDst);
            writeDesc.usage = desc.usage | static_cast<uint32_t>(BufferUsage::TransferSrc);
            writeDesc.memoryUsage = BufferMemoryUsage::DeviceLocal;
            writeDesc.flags = BufferCreateFlags::None;
            writeDesc.debugName = writeName.c_str();
        }

        m_Buffers.resize(count);
        if (splitSlots)
            m_WriteBuffers.resize(count);
        for (uint32_t i = 0; i < count; ++i)
        {
            m_Buffers[i] = device->CreateBuffer(readDesc);
            const bool writeOk =
                !splitSlots || (m_WriteBuffers[i] = device->CreateBuffer(writeDesc)).IsValid();
            if (!m_Buffers[i].IsValid() || !writeOk)
            {
                Destroy(device);
                return false;
            }
        }
        m_Count = count;
        m_SlotBytes = desc.size;
        return true;
    }

    void Destroy(IDevice* device)
    {
        if (device)
        {
            for (auto& b : m_Buffers)
            {
                if (b.IsValid())
                    device->DestroyBuffer(b);
            }
            for (auto& b : m_WriteBuffers)
            {
                if (b.IsValid())
                    device->DestroyBuffer(b);
            }
        }
        m_Buffers.clear();
        m_WriteBuffers.clear();
        m_Pending.clear();
        m_Count = 0;
        m_SlotBytes = 0;
        m_NextSlot = 0;
        m_MappedSlot = UINT32_MAX; // a stale index must not survive re-Init
    }

    bool IsInitialized() const { return m_Count != 0; }
    // True when each slot is a written buffer plus a separate mappable one
    // (see Init). Callers never need this — RecordResolve covers both shapes —
    // but a test asserting the split happened does.
    bool SlotsAreSplit() const { return !m_WriteBuffers.empty(); }
    uint32_t Count() const { return m_Count; }
    uint64_t SlotBytes() const { return m_SlotBytes; }

    /// Registers a write that the caller guarantees will record in this frame.
    /// SupersedeUnread permits slot reuse; PreserveUnread applies backpressure.
    /// Use ReserveWrite when preparation can succeed without recording the write.
    BufferHandle BeginWrite(const RGFrame& frame, const Payload& payload)
    {
        uint64_t reservation = 0;
        const auto output = ReserveWrite(frame, payload, reservation);
        if (output.IsValid())
            MarkRecorded(output, reservation);
        return output;
    }

    /// Reserves a slot without publishing it; returns an invalid handle when full.
    /// Confirm its reservation with MarkRecorded only after the GPU write records.
    BufferHandle ReserveWrite(const RGFrame& frame, const Payload& payload, uint64_t& reservation);

    /// Confirms the matching reservation after its dispatch or copy records.
    /// Returns false for a canceled, replaced or submitted reservation.
    bool MarkRecorded(BufferHandle output, uint64_t reservation)
    {
        for (auto& pending : m_Pending)
        {
            if (!pending.Stamped && pending.Reservation == reservation &&
                (m_WriteBuffers.empty() ? m_Buffers[pending.Slot] : m_WriteBuffers[pending.Slot]) == output)
            {
                pending.Recorded = true;
                return true;
            }
        }
        return false;
    }

    /// Tests whether this frame incarnation still owns the unsubmitted reservation.
    bool HasPendingWrite(const RGFrame& frame, BufferHandle output, uint64_t reservation) const
    {
        for (const auto& pending : m_Pending)
            if (!pending.Stamped && pending.FrameKey == &frame &&
                pending.FrameIndex == frame.FrameIndex() && pending.Reservation == reservation &&
                (m_WriteBuffers.empty() ? m_Buffers[pending.Slot] : m_WriteBuffers[pending.Slot]) == output)
                return true;
        return false;
    }

    /// Cancels this frame incarnation's reservations without discarding submitted results.
    void CancelUnsubmitted(const RGFrame& frame)
    {
        std::erase_if(m_Pending, [&frame](const PendingSlot& pending)
        {
            return !pending.Stamped && pending.FrameKey == &frame &&
                   pending.FrameIndex == frame.FrameIndex();
        });
    }

    // Record the slot's GPU-side resolve into the mappable buffer, after the
    // write and in the same pass, with the handle BeginWrite returned. A no-op
    // where that handle already IS the mappable slot — every backend but
    // WebGPU — so callers record it unconditionally.
    //
    // The slot is left in CopyDest, the state the single-buffer path also
    // leaves behind, so a writer that opens with a transfer fill needs no
    // backend-specific barrier of its own.
    void RecordResolve(CommandList* cl, BufferHandle writeHandle)
    {
        if (!cl || m_WriteBuffers.empty() || !writeHandle.IsValid())
            return;
        for (uint32_t i = 0; i < m_Count; ++i)
        {
            if (!(m_WriteBuffers[i] == writeHandle))
                continue;
            cl->Barrier(ResourceBarrier::CreateBufferBarrier(
                m_WriteBuffers[i], ResourceState::UnorderedAccess, ResourceState::CopySource));
            cl->CopyBuffer(m_WriteBuffers[i], m_Buffers[i], m_SlotBytes);
            cl->Barrier(ResourceBarrier::CreateBufferBarrier(
                m_WriteBuffers[i], ResourceState::CopySource, ResourceState::CopyDest));
            return;
        }
    }

    // Stamp this frame's pendings with its submission token. A pending from
    // this stream whose declare died before submit (the frame was re-begun)
    // is dropped — its copy never recorded. Other streams' pendings are left
    // for their own submit.
    void OnFrameSubmitted(const RGFrame& frame, const IDevice::GpuSyncToken& token);

    // Drop every pending (stamped or not) without touching the buffers: the
    // owner invalidated the data contract (retarget, settings toggle), so
    // results in flight must not resolve as fresh afterwards.
    void DropPendings() { m_Pending.clear(); }

    // A frame stream is dying (window closed): purge its UNSTAMPED pendings.
    // Left alone, a heap-recycled RGFrame at the same address whose first
    // submitted index coincides would falsely stamp them with a token for a
    // submission that never recorded the write (per-window indices restart at
    // 0 and abandoned declares burn indices, so small-integer collisions are
    // reachable). Stamped pendings keep: their token came from a real
    // submission that did record, and identity is never consulted again.
    void OnFrameStreamRetired(const RGFrame& frame)
    {
        for (auto it = m_Pending.begin(); it != m_Pending.end();)
        {
            if (it->FrameKey == &frame && !it->Stamped)
                it = m_Pending.erase(it);
            else
                ++it;
        }
    }

    // Map the NEWEST slot whose token has signaled; older ready slots are
    // superseded and dropped. Null when nothing is ready. The caller copies
    // out and calls Unmap before touching the ring again.
    const void* MapNewestReady(IDevice* device, Payload* outPayload)
    {
        if (!device || m_Count == 0)
            return nullptr;
        size_t readyIdx = m_Pending.size();
        for (size_t i = m_Pending.size(); i-- > 0;)
        {
            const PendingSlot& p = m_Pending[i];
            // Complete only: a stamped slot whose token carries no completion
            // information (Unknown) is not ready — mapping it would hand back a
            // buffer the GPU may still be writing.
            if (p.Stamped && device->QueryGpuSyncToken(p.Token) == IDevice::GpuSyncStatus::Complete)
            {
                readyIdx = i;
                break;
            }
        }
        if (readyIdx == m_Pending.size())
            return nullptr;
        return MapReadySlot(device, readyIdx, outPayload, true);
    }

    /// Maps and consumes the oldest completed result, preserving other entries.
    /// The caller must Unmap before requesting another map or reusing the slot.
    const void* MapOldestReady(IDevice* device, Payload* outPayload)
    {
        if (!device || m_Count == 0)
            return nullptr;
        for (size_t index = 0; index < m_Pending.size(); ++index)
        {
            const auto& pending = m_Pending[index];
            if (pending.Stamped &&
                device->QueryGpuSyncToken(pending.Token) == IDevice::GpuSyncStatus::Complete)
                return MapReadySlot(device, index, outPayload, false);
        }
        return nullptr;
    }

    void Unmap(IDevice* device)
    {
        if (!device || m_Count == 0 || m_MappedSlot >= m_Count)
            return;
        device->UnmapBuffer(m_Buffers[m_MappedSlot]);
        m_MappedSlot = UINT32_MAX;
    }

  private:
    struct PendingSlot
    {
        uint32_t Slot = 0;
        const RGFrame* FrameKey = nullptr;
        uint64_t FrameIndex = 0;
        IDevice::GpuSyncToken Token{};
        bool Stamped = false;
        bool Recorded = false;
        Payload Data{};
        uint64_t Reservation = 0;
    };

    const void* MapReadySlot(IDevice* device, size_t index, Payload* outPayload,
                             bool supersedeOlder)
    {
        const PendingSlot ready = m_Pending[index];
        void* mapped = device->MapBuffer(m_Buffers[ready.Slot]);
        if (!mapped)
            return nullptr;
        if (outPayload)
            *outPayload = ready.Data;
        m_MappedSlot = ready.Slot;
        if (supersedeOlder)
            m_Pending.erase(m_Pending.begin(), m_Pending.begin() + index + 1);
        else
            m_Pending.erase(m_Pending.begin() + index);
        return mapped;
    }

    std::vector<BufferHandle> m_Buffers;      // mapped by the CPU
    std::vector<BufferHandle> m_WriteBuffers; // empty unless slots are split
    std::vector<PendingSlot> m_Pending;       // registration order == age order
    uint32_t m_Count = 0;
    uint64_t m_SlotBytes = 0;
    uint32_t m_NextSlot = 0;
    IDevice* m_Device = nullptr;
    uint32_t m_MappedSlot = UINT32_MAX;
    RGReadbackRetention m_Retention = RGReadbackRetention::SupersedeUnread;
    uint64_t m_NextReservation = 1;
};

template <class Payload>
BufferHandle RGReadbackRing<Payload>::ReserveWrite(const RGFrame& frame, const Payload& payload,
                                                  uint64_t& reservation)
{
    reservation = 0;
    if (m_Count == 0)
        return {};
    if (m_Retention == RGReadbackRetention::PreserveUnread)
    {
        std::erase_if(m_Pending, [&frame](const PendingSlot& pending)
        {
            return !pending.Stamped && pending.FrameKey == &frame &&
                   pending.FrameIndex != frame.FrameIndex();
        });
        for (const auto& pending : m_Pending)
            if (pending.Slot == m_NextSlot)
                return {};
    }
    const uint32_t slot = m_NextSlot;
    // A slot whose read-map is live or still resolving (web's maps are
    // asynchronous) must not take a new copy: WebGPU fails the whole submit
    // for a mapped buffer. Skipping drops one sample, which every reader of a
    // readback ring already tolerates — that is what a ring is for.
    if (m_Device != nullptr && m_Device->IsBufferMapBusy(m_Buffers[slot]))
        return {};
    m_NextSlot = (m_NextSlot + 1) % m_Count;
    // Drop a still-unread pending on the reused slot: its data is being
    // overwritten before anyone mapped it.
    for (auto it = m_Pending.begin(); it != m_Pending.end();)
    {
        if (it->Slot == slot)
            it = m_Pending.erase(it);
        else
            ++it;
    }
    PendingSlot p{};
    p.Slot = slot;
    p.FrameKey = &frame;
    p.FrameIndex = frame.FrameIndex();
    p.Data = payload;
    p.Reservation = m_NextReservation++;
    if (m_NextReservation == 0)
        ++m_NextReservation;
    reservation = p.Reservation;
    m_Pending.push_back(p);
    return m_WriteBuffers.empty() ? m_Buffers[slot] : m_WriteBuffers[slot];
}

template <class Payload>
void RGReadbackRing<Payload>::OnFrameSubmitted(const RGFrame& frame,
                                               const IDevice::GpuSyncToken& token)
{
    const uint64_t frameIndex = frame.FrameIndex();
    for (auto it = m_Pending.begin(); it != m_Pending.end();)
    {
        if (it->FrameKey != &frame || it->Stamped)
        {
            ++it; // another stream's pending (or already stamped) — leave it
            continue;
        }
        if (it->FrameIndex != frameIndex || !it->Recorded)
        {
            // Abandoned incarnations and unrecorded reservations cannot publish data.
            // Stream retirement also purges unstamped entries before address reuse.
            it = m_Pending.erase(it);
            continue;
        }
        it->Token = token;
        it->Stamped = true;
        ++it;
    }
}

} // namespace GameEngine::Rendering::RenderGraph
