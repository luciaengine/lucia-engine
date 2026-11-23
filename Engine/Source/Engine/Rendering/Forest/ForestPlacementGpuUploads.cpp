#include "Engine/Rendering/Forest/ForestPlacementGpuUploads.h"

#include "ForestPlacementUploadPlan.h"
#include "Rendering/Core/Device.h"

#include "Rendering/Core/PassPhase.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <optional>

namespace GameEngine::Engine::Renderer
{
std::optional<ForestPlacementUploadPlan> BuildForestPlacementUploadPlan(
    std::span<const ForestPlacementUpload> uploads, uint32 slotCount)
{
    if (uploads.size() > slotCount)
        return std::nullopt;

    ForestPlacementUploadPlan plan;
    plan.SlotOrder.reserve(uploads.size());
    for (const auto& upload : uploads)
        plan.SlotOrder.push_back(&upload);
    std::ranges::sort(plan.SlotOrder, {}, [](const ForestPlacementUpload* upload) { return upload->Ticket.Slot; });

    uint32 previous = slotCount;
    for (const auto* upload : plan.SlotOrder)
    {
        const uint32 destination = upload->Ticket.Slot;
        const uint64 bytes = upload->Bytes.size();
        if (destination >= slotCount || destination == previous || bytes == 0 || bytes > kPlacementSlotBytes)
            return std::nullopt;
        previous = destination;

        const uint64 offset = uint64{destination} * kPlacementSlotBytes;
        if (!plan.Runs.empty() && plan.Runs.back().SlotOffset + plan.Runs.back().Bytes == offset)
            plan.Runs.back().Bytes += bytes;
        else
            plan.Runs.push_back({plan.Bytes, offset, bytes});
        plan.Bytes += bytes;
    }
    return plan;
}

struct ForestPlacementGpuUploads::Batch
{
    Rendering::RenderGraph::RGFrameStamp Frame;
    std::vector<ForestPageTicket> Tickets;
    std::atomic<bool> Recorded{false};
    bool Submitted = false;
    Rendering::IDevice::GpuSyncToken Completion;
};

ForestPlacementGpuUploads::ForestPlacementGpuUploads(std::shared_ptr<ForestPlacementRuntime> placements,
    Rendering::BufferHandle destination, uint32 slots)
    : m_Placements(std::move(placements)), m_Destination(destination), m_SlotCount(slots)
{
}

ForestPlacementGpuUploads::~ForestPlacementGpuUploads() = default;

bool ForestPlacementGpuUploads::HasOutstandingCopy(ForestPageTicket ticket) const
{
    for (const auto& batch : m_Batches)
        for (const ForestPageTicket& pending : batch->Tickets)
            if (pending == ticket)
                return true;
    return false;
}

bool ForestPlacementGpuUploads::RejectUploads(std::span<const ForestPlacementUpload> uploads)
{
    for (const auto& upload : uploads)
        if (!HasOutstandingCopy(upload.Ticket))
            m_Placements->CompleteUpload(upload.Ticket, ForestUploadResult::Failed);
    return false;
}

bool ForestPlacementGpuUploads::Record(Rendering::RenderGraph::RGFrame& frame,
                                        std::span<const ForestPlacementUpload> uploads)
{
    using namespace Rendering;
    using namespace Rendering::RenderGraph;
    if (uploads.empty())
        return true;
    if (!m_Placements)
        return false;
    if (!m_Destination.IsValid())
        return RejectUploads(uploads);
    auto plan = BuildForestPlacementUploadPlan(uploads, m_SlotCount);
    if (!plan)
        return RejectUploads(uploads);

    auto batch = std::make_shared<Batch>();
    batch->Frame.Stamp(frame);
    batch->Tickets.reserve(uploads.size());
    for (const auto* upload : plan->SlotOrder)
    {
        const auto range = m_Placements->Store().ReadRange(upload->Ticket.Page);
        if (!range || range->Bytes != upload->Bytes.size() ||
            !m_Placements->CanRecordUpload(upload->Ticket) || HasOutstandingCopy(upload->Ticket))
            return RejectUploads(uploads);
        batch->Tickets.push_back(upload->Ticket);
    }

    const auto allocation = frame.AllocUpload(plan->Bytes);
    if (!allocation.Valid())
        return RejectUploads(uploads);
    auto* staging = static_cast<std::byte*>(allocation.Ptr);
    for (const auto* upload : plan->SlotOrder)
    {
        std::memcpy(staging, upload->Bytes.data(), upload->Bytes.size());
        staging += upload->Bytes.size();
    }

    const auto src = frame.ImportExternalBuffer("Forest.PlacementUpload", allocation.Buffer);
    const auto dst = frame.ImportExternalBuffer("Forest.PlacementSlots", m_Destination,
                                                static_cast<uint64>(m_SlotCount) * kPlacementSlotBytes);
    const auto pass = frame.AddPass("Forest.UploadPlacementSlots", PassPhase::kEarlySetup,
        [src, dst, batch, &frame](RGPassBuilder& builder)
        {
            builder.Read(src, RGBufferRead::CopySrc);
            builder.Write(dst, RGBufferWrite::CopyDst);
            builder.PreventCulling();
            builder.OnSubmitted([batch, &frame]
            {
                batch->Completion = frame.SubmissionToken(RGQueue::Graphics);
                batch->Submitted = true;
            });
        },
        [src, dst, batch, runs = std::move(plan->Runs), start = allocation.Offset](RGContext& ctx)
        {
            const auto source = ctx.GetBuffer(src);
            const auto destination = ctx.GetBuffer(dst);
            if (!ctx.Cmd || !source.IsValid() || !destination.IsValid())
                return;
            for (const auto& run : runs)
                ctx.Cmd->CopyBuffer(source, destination, run.Bytes, start + run.StagingOffset, run.SlotOffset);
            batch->Recorded.store(true, std::memory_order_release);
        });
    if (!pass.IsValid())
        return RejectUploads(uploads);

    m_Batches.push_back(std::move(batch));
    return true;
}

void ForestPlacementGpuUploads::FinishFrame(Rendering::RenderGraph::RGFrame& frame)
{
    std::erase_if(m_Batches, [this, &frame](const std::shared_ptr<Batch>& batch)
    {
        if (!batch->Frame.IsFor(frame) || batch->Submitted)
            return false;
        for (const auto& ticket : batch->Tickets)
            m_Placements->CompleteUpload(ticket, ForestUploadResult::Abandoned);
        return true;
    });
}

uint32 ForestPlacementGpuUploads::DrainCompleted(const Rendering::IDevice& device)
{
    uint32 published = 0;
    std::erase_if(m_Batches, [this, &device, &published](const std::shared_ptr<Batch>& batch)
    {
        if (!batch->Submitted ||
            device.QueryGpuSyncToken(batch->Completion) != Rendering::IDevice::GpuSyncStatus::Complete)
            return false;
        const ForestUploadResult result = batch->Recorded.load(std::memory_order_acquire)
                                              ? ForestUploadResult::Published
                                              : ForestUploadResult::Failed;
        for (const auto& ticket : batch->Tickets)
            published += m_Placements->CompleteUpload(ticket, result);
        return true;
    });
    return published;
}

} // namespace GameEngine::Engine::Renderer
