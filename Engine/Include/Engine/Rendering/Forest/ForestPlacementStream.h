#pragma once

#include "Engine/Rendering/Forest/ForestPlacementRuntime.h"
#include "Types/ByteBuffer.h"

#include <atomic>
#include <future>

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
    struct Read
    {
        std::vector<ReadPage> Pages;
        std::shared_ptr<std::atomic<bool>> Cancel;
        std::future<ByteBuffer> Result;
    };

    void SubmitReads(AssetManager& assets, std::span<const ForestPageTicket> tickets);
    void Submit(AssetManager& assets, std::span<const ReadPage> pages);

    std::shared_ptr<ForestPlacementRuntime> m_Placements;
    std::vector<Read> m_Reads;
    std::vector<ReadPage> m_NextReads;
    std::vector<ReadRun> m_Runs;
    std::vector<ByteBuffer> m_Completed;
};

} // namespace Engine::Renderer
} // namespace GameEngine
