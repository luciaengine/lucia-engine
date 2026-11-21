#include "Engine/Rendering/Forest/ForestPlacementStream.h"

#include "Assets/AssetIOService.h"
#include "Assets/AssetManager.h"

#include <algorithm>
#include <chrono>

namespace GameEngine::Engine::Renderer
{

ForestPlacementStream::ForestPlacementStream(std::shared_ptr<ForestPlacementRuntime> placements)
    : m_Placements(std::move(placements))
{
}

ForestPlacementStream::~ForestPlacementStream()
{
    for (const auto& read : m_Reads)
    {
        read.Cancel->store(true, std::memory_order_release);
        // Retire planner state as well if the owner keeps it for stand rendering.
        for (const auto& page : read.Pages)
            m_Placements->CompleteRead(page.Ticket, {});
    }
}

void ForestPlacementStream::Submit(AssetManager& assets, std::span<const ReadPage> pages)
{
    const uint64 first = pages.front().Range.Offset;
    const auto& last = pages.back().Range;
    const uint64 bytes = last.Offset + last.Bytes - first;
    auto result = std::make_shared<std::promise<ByteBuffer>>();
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    m_Reads.push_back({std::vector<ReadPage>(pages.begin(), pages.end()), cancel, result->get_future()});

    AssetIOService::ReadRequest request;
    request.Metadata.Path = m_Placements->Store().File();
    request.Priority = AssetLoadPriority::High;
    request.RangeOffset = first;
    request.RangeBytes = request.MaximumBytes = bytes;
    request.CancelRequested = cancel;
    request.ProcessData = [result, cancel](ByteBuffer data) -> SharedPtr<Asset>
    {
        result->set_value(cancel->load(std::memory_order_acquire) ? ByteBuffer{} : std::move(data));
        return {};
    };
    request.OnFailure = [result](const String&) { result->set_value(ByteBuffer{}); };
    assets.SubmitRead(std::move(request));
}

void ForestPlacementStream::SubmitReads(AssetManager& assets, std::span<const ForestPageTicket> tickets)
{
    m_NextReads.clear();
    for (const auto ticket : tickets)
    {
        const auto range = m_Placements->Store().ReadRange(ticket.Page);
        if (range)
            m_NextReads.push_back({ticket, *range, m_NextReads.size()});
        else
            m_Placements->CompleteRead(ticket, {});
    }
    std::ranges::sort(m_NextReads, {}, [](const ReadPage& page) { return page.Range.Offset; });
    m_Runs.clear();
    const std::span pages(m_NextReads);
    for (size_t begin = 0; begin < pages.size();)
    {
        size_t end = begin + 1;
        size_t priority = pages[begin].Priority;
        while (end < pages.size() &&
               pages[end - 1].Range.Offset + pages[end - 1].Range.Bytes == pages[end].Range.Offset)
        {
            priority = std::min(priority, pages[end].Priority);
            ++end;
        }
        m_Runs.push_back({begin, end - begin, priority});
        begin = end;
    }
    // Submit each merged read at its earliest original rank.
    std::ranges::sort(m_Runs, {}, &ReadRun::Priority);
    for (const auto& run : m_Runs)
        Submit(assets, pages.subspan(run.First, run.Count));
}

bool ForestPlacementStream::Prepare(AssetManager& assets, const ForestPlacementView& view,
                                    uint64 frame, uint64 completedReaders, ForestPlacementFrame& plan,
                                    std::vector<ForestPlacementUpload>& uploads)
{
    uploads.clear();
    m_Completed.clear();
    if (!m_Placements)
    {
        plan = {};
        return false;
    }
    if (!m_Placements->Prepare(view, frame, completedReaders, plan))
        return false;

    if (!plan.CancelReads.empty())
    {
        for (auto& read : m_Reads)
        {
            for (auto& page : read.Pages)
                page.Cancelled |= std::ranges::find(plan.CancelReads, page.Ticket) != plan.CancelReads.end();
            if (std::ranges::all_of(read.Pages, &ReadPage::Cancelled))
                read.Cancel->store(true, std::memory_order_release);
        }
    }

    // A batch can still feed live tickets after a neighboring page has been retired.
    bool rejectedRead = false;
    std::erase_if(m_Reads, [&](Read& read)
    {
        if (read.Result.wait_for(std::chrono::seconds::zero()) != std::future_status::ready)
            return false;
        ByteBuffer bytes;
        try
        {
            bytes = read.Result.get();
        }
        catch (const std::future_error&)
        {
            // A queued job may be destroyed at shutdown without entering ProcessData.
        }
        const std::span<const uint8> data(bytes.Data(), bytes.Size());
        const uint64 first = read.Pages.front().Range.Offset;
        const size_t previous = uploads.size();
        for (const auto& page : read.Pages)
        {
            const uint64 offset = page.Range.Offset - first;
            std::span<const uint8> slice;
            if (!page.Cancelled && offset <= data.size() && page.Range.Bytes <= data.size() - offset)
                slice = data.subspan(static_cast<size_t>(offset), static_cast<size_t>(page.Range.Bytes));
            if (m_Placements->CompleteRead(page.Ticket, slice))
                uploads.push_back({page.Ticket, slice});
            else
                rejectedRead = true;
        }
        if (uploads.size() != previous)
            m_Completed.push_back(std::move(bytes));
        return true;
    });

    // An acknowledged cancellation can free a slot immediately. Keep the same
    // priority order and per-frame budget when reserving its replacement.
    if (rejectedRead)
        m_Placements->AppendReads(plan.Reads);
    SubmitReads(assets, plan.Reads);
    return true;
}

} // namespace GameEngine::Engine::Renderer
