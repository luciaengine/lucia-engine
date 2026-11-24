#include "ForestPlacementStream.h"

#include "Assets/AssetIOService.h"
#include "Assets/AssetManager.h"

#include <algorithm>

namespace GameEngine::Engine::Renderer
{

ForestPlacementStream::ForestPlacementStream(std::shared_ptr<ForestPlacementRuntime> placements)
    : m_Placements(std::move(placements))
    , m_ArchivePath(m_Placements ? std::make_shared<const std::filesystem::path>(m_Placements->Store().File()) : nullptr)
{
}

ForestPlacementStream::~ForestPlacementStream()
{
    for (const auto& read : m_Reads)
    {
        read.Cancel->store(true, std::memory_order_release);
        // The runtime outlives the stream when its owner keeps drawing the overview.
        for (const auto& page : read.Pages)
            m_Placements->CompleteRead(page.Ticket, {});
    }
}

void ForestPlacementStream::Publish(ReadResult& result, ByteBuffer bytes)
{
    result.Bytes = std::move(bytes);
    result.Done.store(true, std::memory_order_release);
}

void ForestPlacementStream::CancelRetiredReads(AssetManager& assets, std::span<const ForestPageTicket> cancelled)
{
    for (auto read = m_Reads.begin(); read != m_Reads.end();)
    {
        for (auto& page : read->Pages)
            page.Cancelled |= std::ranges::find(cancelled, page.Ticket) != cancelled.end();
        if (!std::ranges::all_of(read->Pages, &ReadPage::Cancelled))
        {
            ++read;
            continue;
        }
        assets.CancelRead(read->Cancel);
        read = m_Reads.erase(read);
    }
}

void ForestPlacementStream::Submit(AssetManager& assets, std::span<const ReadPage> pages)
{
    const uint64 first = pages.front().Range.Offset;
    const auto& last = pages.back().Range;
    const uint64 bytes = last.Offset + last.Bytes - first;
    auto result = std::make_shared<ReadResult>();
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    m_Reads.push_back({std::vector<ReadPage>(pages.begin(), pages.end()), cancel, result});
    // CancelRetiredReads removes a cancelled read from m_Reads, so nothing consumes its result.

    AssetIOService::ReadRequest request;
    request.SharedRangePath = m_ArchivePath;
    request.Priority = AssetLoadPriority::High;
    request.RangeOffset = first;
    request.RangeBytes = request.MaximumBytes = bytes;
    request.CancelRequested = cancel;
    request.ProcessData = [result](ByteBuffer data) -> SharedPtr<Asset>
    {
        Publish(*result, std::move(data));
        return {};
    };
    request.OnFailure = [result](const String&) { Publish(*result, {}); };
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

bool ForestPlacementStream::ConsumeRead(Read& read, std::vector<ForestPlacementUpload>& uploads, bool& rejectedRead)
{
    if (!read.Result->Done.load(std::memory_order_acquire))
        return false;
    ByteBuffer bytes = std::move(read.Result->Bytes);
    const std::span<const uint8> data(bytes.Data(), bytes.Size());
    const uint64 first = read.Pages.front().Range.Offset;
    const size_t previous = uploads.size();
    for (const auto& page : read.Pages)
    {
        if (page.Cancelled)
            continue;
        const uint64 offset = page.Range.Offset - first;
        std::span<const uint8> slice;
        if (offset <= data.size() && page.Range.Bytes <= data.size() - offset)
            slice = data.subspan(static_cast<size_t>(offset), static_cast<size_t>(page.Range.Bytes));
        if (m_Placements->CompleteRead(page.Ticket, slice))
            uploads.push_back({page.Ticket, slice});
        else
            rejectedRead = true;
    }
    if (uploads.size() != previous)
        m_Completed.push_back(std::move(bytes));
    return true;
}

bool ForestPlacementStream::Prepare(AssetManager& assets, const ForestPlacementView& view,
                                    uint64 frame, uint64 completedReaders,
                                    std::span<const ForestShadowPage> shadowPages, ForestPlacementFrame& plan,
                                    std::vector<ForestPlacementUpload>& uploads)
{
    uploads.clear();
    m_Completed.clear();
    if (!m_Placements)
    {
        plan = {};
        return false;
    }
    if (!m_Placements->Prepare(view, frame, completedReaders, shadowPages, plan))
        return false;

    if (!plan.CancelReads.empty())
        CancelRetiredReads(assets, plan.CancelReads);

    // A batch can still feed live tickets after a neighboring page has been retired.
    bool rejectedRead = false;
    std::erase_if(m_Reads, [this, &uploads, &rejectedRead](Read& read)
                  { return ConsumeRead(read, uploads, rejectedRead); });

    // A failed read may free a slot. Reserve its replacement within the frame budget.
    if (rejectedRead)
        m_Placements->AppendReads(plan.Reads);
    SubmitReads(assets, plan.Reads);
    return true;
}

} // namespace GameEngine::Engine::Renderer
