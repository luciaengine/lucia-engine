#include "Engine/Rendering/MaterialMipStreaming/MaterialMipStepPlanner.h"

#include <algorithm>
#include <limits>

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
    // Source opacity for an actively baking atlas has priority within the shared budget.
    if (stream.ImpostorOpacity)
        return std::numeric_limits<float32>::max();
    const uint32 residentExtent = std::max(stream.TopExtent >> stream.FirstMip, 1u);
    return stream.ScreenCoverage / static_cast<float32>(residentExtent);
}
} // namespace

bool MaterialMipStepPlanner::GainsBefore(const Candidate& left, const Candidate& right)
{
    if (left.Need != right.Need)
        return left.Need > right.Need;
    if (left.Bytes != right.Bytes)
        return left.Bytes < right.Bytes;
    return left.Id < right.Id;
}

bool MaterialMipStepPlanner::LosesBefore(const Candidate& left, const Candidate& right)
{
    if (left.Need != right.Need)
        return left.Need < right.Need;
    if (left.Bytes != right.Bytes)
        return left.Bytes > right.Bytes;
    return left.Id < right.Id;
}

bool MaterialMipStepPlanner::NeedsLessThan(const Candidate& donor, float32 need)
{
    return donor.Need < need;
}

std::optional<MaterialMipStepPlanner::Step> MaterialMipStepPlanner::Plan(std::span<const Stream> streams,
                                                                         uint64 budgetBytes)
{
    m_Takers.clear();
    m_Donors.clear();
    uint64 usedBytes = 0;
    for (uint32 index = 0; index < streams.size(); ++index)
    {
        const Stream& stream = streams[index];
        const float32 need = Need(stream);
        usedBytes += ResidentBytes(stream);
        if (stream.FirstMip > 0)
            m_Takers.push_back({need, GainBytes(stream), index, stream.Id});
        if (stream.FirstMip < stream.Tail)
            m_Donors.push_back({need, LossBytes(stream), index, stream.Id});
    }
    return usedBytes > budgetBytes ? PlanLoss(streams) : PlanGain(streams, budgetBytes - usedBytes);
}

std::optional<MaterialMipStepPlanner::Step> MaterialMipStepPlanner::PlanLoss(std::span<const Stream> streams) const
{
    const auto least = std::min_element(m_Donors.begin(), m_Donors.end(), LosesBefore);
    if (least == m_Donors.end())
        return std::nullopt;
    return Step{least->StreamIndex, streams[least->StreamIndex].FirstMip + 1};
}

std::optional<MaterialMipStepPlanner::Step> MaterialMipStepPlanner::PlanGain(std::span<const Stream> streams,
                                                                             uint64 freeBytes)
{
    std::sort(m_Takers.begin(), m_Takers.end(), GainsBefore);
    std::sort(m_Donors.begin(), m_Donors.end(), LosesBefore);
    m_DonorSavings.resize(m_Donors.size());
    uint64 savings = 0;
    for (size_t donor = 0; donor < m_Donors.size(); ++donor)
    {
        savings += m_Donors[donor].Bytes;
        m_DonorSavings[donor] = savings;
    }

    for (const Candidate& taker : m_Takers)
    {
        if (taker.Bytes <= freeBytes)
            return Step{taker.StreamIndex, streams[taker.StreamIndex].FirstMip - 1,
                        streams[taker.StreamIndex].ImpostorOpacity};

        // The donors of this taker are a prefix of m_Donors: need rises along it.
        const auto firstTooNeedy =
            std::lower_bound(m_Donors.begin(), m_Donors.end(), taker.Need / kDonorNeedMargin, NeedsLessThan);
        const size_t donorCount = static_cast<size_t>(firstTooNeedy - m_Donors.begin());
        if (donorCount > 0 && m_DonorSavings[donorCount - 1] >= taker.Bytes - freeBytes)
        {
            const uint32 donor = m_Donors.front().StreamIndex;
            return Step{donor, streams[donor].FirstMip + 1,
                        streams[taker.StreamIndex].ImpostorOpacity};
        }
    }
    return std::nullopt;
}

} // namespace GameEngine::Engine::Renderer
