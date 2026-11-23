#pragma once

#include "ForestPlacementRuntime.h"
#include "Types/ByteBuffer.h"

#include <atomic>
#include <filesystem>
#include <memory>

namespace GameEngine
{
class AssetManager;

namespace Engine::Renderer
{

/// Verified bytes of one page for its slot.
struct ForestPlacementUpload
{
    ForestPageTicket Ticket;
    std::span<const uint8> Bytes;
};

/// The asset IO side of a forest placement runtime. It merges reads of byte-adjacent pages, submits
/// them through the AssetManager and returns only verified bytes for upload. The caller reports
/// each upload to the runtime. Upload bytes borrow the stream's completed reads until the next
/// Prepare. Runs on the render thread, which is also the AssetManager's owner thread. The
/// destructor fails the runtime's open reads; reads already queued run to completion and are dropped.
class ForestPlacementStream
{
public:
    explicit ForestPlacementStream(std::shared_ptr<ForestPlacementRuntime> placements);
    ~ForestPlacementStream();
    ForestPlacementStream(const ForestPlacementStream&) = delete;
    ForestPlacementStream& operator=(const ForestPlacementStream&) = delete;

    /// Plans the frame (ForestPlacementRuntime::Prepare), consumes finished reads and submits new ones.
    bool Prepare(AssetManager& assets, const ForestPlacementView& view, uint64 frame, uint64 completedReaders,
                 std::span<const ForestShadowPage> shadowPages, ForestPlacementFrame& plan,
                 std::vector<ForestPlacementUpload>& uploads);

private:
    struct ReadPage
    {
        ForestPageTicket Ticket;
        PageStreaming::PlacementByteRange Range;
        size_t Priority;
        bool Cancelled = false;
    };
    struct ReadRun
    {
        size_t First;
        size_t Count;
        size_t Priority;
    };
    // Written once by the read's ProcessData or OnFailure, then published by Done.
    struct ReadResult
    {
        ByteBuffer Bytes;
        std::atomic<bool> Done{false};
    };
    struct Read
    {
        std::vector<ReadPage> Pages;
        std::shared_ptr<std::atomic<bool>> Cancel;
        std::shared_ptr<ReadResult> Result;
    };
    static void Publish(ReadResult& result, ByteBuffer bytes);
    // Withdraws each read whose pages the runtime all cancelled.
    void CancelRetiredReads(AssetManager& assets, std::span<const ForestPageTicket> cancelled);
    // Hands the pages of a finished read to the runtime. Returns false while the read is in flight.
    bool ConsumeRead(Read& read, std::vector<ForestPlacementUpload>& uploads, bool& rejectedRead);

    void SubmitReads(AssetManager& assets, std::span<const ForestPageTicket> tickets);
    void Submit(AssetManager& assets, std::span<const ReadPage> pages);

    std::shared_ptr<ForestPlacementRuntime> m_Placements;
    const std::shared_ptr<const std::filesystem::path> m_ArchivePath;
    std::vector<Read> m_Reads;
    std::vector<ReadPage> m_NextReads;
    std::vector<ReadRun> m_Runs;
    std::vector<ByteBuffer> m_Completed;
};

} // namespace Engine::Renderer
} // namespace GameEngine
