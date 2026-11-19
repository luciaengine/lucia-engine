#include "Engine/Rendering/MaterialMipStreaming/MaterialMipStepPlanner.h"

#include <algorithm>

namespace GameEngine::Engine::Renderer
{

namespace
{
using Stream = MaterialMipStepPlanner::Stream;

// A donor gives up a level only for a stream with more than this multiple of its need.
constexpr float32 kDonorNeedMargin = 2.0f;

uint64 ResidentBytes(const Stream& stream)
{
    return stream.SuffixBytes[stream.FirstMip];
}

// The bytes that the next finer level adds. The stream is above level 0.
uint64 GainBytes(const Stream& stream)
{
    return stream.SuffixBytes[stream.FirstMip - 1] - stream.SuffixBytes[stream.FirstMip];
}

// The bytes that the first resident level frees. The stream is above its tail.
uint64 LossBytes(const Stream& stream)
{
    return stream.SuffixBytes[stream.FirstMip] - stream.SuffixBytes[stream.FirstMip + 1];
}

float32 Need(const Stream& stream)
{
    const uint32 residentExtent = std::max(stream.TopExtent >> stream.FirstMip, 1u);
    return stream.ScreenCoverage / static_cast<float32>(residentExtent);
}
} // namespace

bool MaterialMipStepPlanner::HasLessNeed(std::span<const Stream> streams, uint32 left, uint32 right) const
{
    if (m_Need[left] != m_Need[right])
        return m_Need[left] < m_Need[right];
    const uint64 leftBytes = LossBytes(streams[left]);
    const uint64 rightBytes = LossBytes(streams[right]);
    if (leftBytes != rightBytes)
        return leftBytes > rightBytes;
    return streams[left].Id < streams[right].Id;
}

bool MaterialMipStepPlanner::HasMoreNeed(std::span<const Stream> streams, uint32 left, uint32 right) const
{
    if (m_Need[left] != m_Need[right])
        return m_Need[left] > m_Need[right];
    const uint64 leftBytes = GainBytes(streams[left]);
    const uint64 rightBytes = GainBytes(streams[right]);
    if (leftBytes != rightBytes)
        return leftBytes < rightBytes;
    return streams[left].Id < streams[right].Id;
}

std::optional<MaterialMipStepPlanner::Step> MaterialMipStepPlanner::Plan(std::span<const Stream> streams,
                                                                         uint64 budgetBytes)
{
    m_Need.resize(streams.size());
    m_Takers.clear();
    m_Donors.clear();
    uint64 usedBytes = 0;
    for (uint32 index = 0; index < streams.size(); ++index)
    {
        const Stream& stream = streams[index];
        m_Need[index] = Need(stream);
        usedBytes += ResidentBytes(stream);
        if (stream.FirstMip > 0)
            m_Takers.push_back(index);
        if (stream.FirstMip < stream.Tail)
            m_Donors.push_back(index);
    }
    return usedBytes > budgetBytes ? PlanLoss(streams) : PlanGain(streams, budgetBytes - usedBytes);
}

std::optional<MaterialMipStepPlanner::Step> MaterialMipStepPlanner::PlanLoss(std::span<const Stream> streams) const
{
    const auto least = std::min_element(m_Donors.begin(), m_Donors.end(), [&](uint32 left, uint32 right)
    {
        return HasLessNeed(streams, left, right);
    });
    if (least == m_Donors.end())
        return std::nullopt;
    return Step{*least, streams[*least].FirstMip + 1};
}

std::optional<MaterialMipStepPlanner::Step> MaterialMipStepPlanner::PlanGain(std::span<const Stream> streams,
                                                                             uint64 freeBytes)
{
    std::sort(m_Takers.begin(), m_Takers.end(), [&](uint32 left, uint32 right)
    {
        return HasMoreNeed(streams, left, right);
    });
    std::sort(m_Donors.begin(), m_Donors.end(), [&](uint32 left, uint32 right)
    {
        return HasLessNeed(streams, left, right);
    });
    m_DonorSavings.resize(m_Donors.size());
    uint64 savings = 0;
    for (size_t donor = 0; donor < m_Donors.size(); ++donor)
    {
        savings += LossBytes(streams[m_Donors[donor]]);
        m_DonorSavings[donor] = savings;
    }

    for (const uint32 taker : m_Takers)
    {
        const uint64 gainBytes = GainBytes(streams[taker]);
        if (gainBytes <= freeBytes)
            return Step{taker, streams[taker].FirstMip - 1};

        // The donors of this taker are a prefix of m_Donors: need rises along it.
        const float32 donorNeedLimit = m_Need[taker] / kDonorNeedMargin;
        const auto firstTooNeedy = std::partition_point(m_Donors.begin(), m_Donors.end(), [&](uint32 donor)
        {
            return m_Need[donor] < donorNeedLimit;
        });
        const size_t donorCount = static_cast<size_t>(firstTooNeedy - m_Donors.begin());
        if (donorCount > 0 && m_DonorSavings[donorCount - 1] >= gainBytes - freeBytes)
            return Step{m_Donors.front(), streams[m_Donors.front()].FirstMip + 1};
    }
    return std::nullopt;
}

} // namespace GameEngine::Engine::Renderer
