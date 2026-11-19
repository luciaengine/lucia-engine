#pragma once

#include "AssetCore/GUID.h"
#include "Types/Types.h"

#include <optional>
#include <span>
#include <vector>

namespace GameEngine::Engine::Renderer
{

/// Chooses the next step of material mip streaming: the texture that gains or loses one mip level.
///
/// The need of a stream is its screen coverage per resident texel:
/// ScreenCoverage / (TopExtent >> FirstMip). Over the budget, the stream with the least need loses
/// a level. Under the budget, the stream with the most need gains a level. When that level does
/// not fit, the streams with less than half of that need each give up one level, if together they
/// free enough bytes. Half is the smallest margin for which the two streams do not trade back.
///
/// Equal needs order by bytes, then by Id: streams with no demand data step in a fixed order.
class MaterialMipStepPlanner
{
public:
    struct Stream
    {
        GUID Id;
        std::span<const uint64> SuffixBytes; ///< [i] is the resident bytes when level i is the first resident level.
        uint32 FirstMip = 0;                 ///< The first resident level.
        uint32 Tail = 0;                     ///< The coarsest first resident level. A stream keeps Tail and below.
        uint32 TopExtent = 1;                ///< The longest edge of level 0, in texels.
        float32 ScreenCoverage = 0.0f;       ///< MaterialScreenDemand coverage. 0 without demand data.
    };

    struct Step
    {
        size_t Stream = 0;    ///< An index into the planned span.
        uint32 TargetMip = 0; ///< The new first resident level of that stream.
    };

    /// The next step for `streams` under `budgetBytes`. Nullopt when no step improves the set.
    std::optional<Step> Plan(std::span<const Stream> streams, uint64 budgetBytes);

private:
    std::optional<Step> PlanLoss(std::span<const Stream> streams) const;
    std::optional<Step> PlanGain(std::span<const Stream> streams, uint64 freeBytes);
    bool HasLessNeed(std::span<const Stream> streams, uint32 left, uint32 right) const;
    bool HasMoreNeed(std::span<const Stream> streams, uint32 left, uint32 right) const;

    // Scratch that Plan fills, kept between calls for its capacity.
    std::vector<float32> m_Need;          // by stream index
    std::vector<uint32> m_Takers;         // streams that can gain a level, most need first
    std::vector<uint32> m_Donors;         // streams that can lose a level, least need first
    std::vector<uint64> m_DonorSavings;   // [i] is the bytes that m_Donors[0..i] free with one level each
};

} // namespace GameEngine::Engine::Renderer
