#pragma once

#include "Engine/Rendering/Forest/ForestPlacementRuntime.h"
#include "Types/ByteBuffer.h"

#include <atomic>
#include <filesystem>
#include <memory>

namespace GameEngine
{
class AssetManager;

namespace Engine::Renderer
{

struct ForestPlacementUpload
{
    ForestPageTicket Ticket;
    std::span<const uint8> Bytes;
};

/// The asset IO side of a forest placement runtime. Prepare runs on the asset
/// owner thread and returns only verified bytes for GPU upload. The caller
/// reports upload completion (or a failed/aborted upload) to the runtime.
/// Upload bytes borrow the stream's completed reads until the next Prepare.
class ForestPlacementStream
{
public:
    explicit ForestPlacementStream(std::shared_ptr<ForestPlacementRuntime> placements);
    ~ForestPlacementStream();
    ForestPlacementStream(const ForestPlacementStream&) = delete;
    ForestPlacementStream& operator=(const ForestPlacementStream&) = delete;

    bool Prepare(AssetManager& assets, const ForestPlacementView& view, uint64 frame, uint64 completedReaders,
                 ForestPlacementFrame& plan, std::vector<ForestPlacementUpload>& uploads);

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
    // Written once on an asset reader thread, then published by Done.
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
