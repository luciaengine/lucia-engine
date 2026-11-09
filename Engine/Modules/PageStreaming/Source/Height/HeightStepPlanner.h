#pragma once

#include "PageStreaming/PageStore/PageStoreFormat.h"

#include <array>
#include <span>
#include <vector>

namespace GameEngine::PageStreaming
{

/// The finest step of an absolute store's level: 2^-20 of the source's units (under a micrometer in
/// meters), finer than any elevation source measures.
inline constexpr int32 kHeightStepExponentMin = -20;
/// Allows 1 percent of a level's pages, at least one, to use 32-bit integers.
/// A single-page level must fit its only page in 16 bits.
inline constexpr float64 kHeightWidePageShare = 0.01;

/// The exponent of the finest power-of-two step at which values spanning `range` fit a page's 16-bit
/// words, the rounding of the base and the top included; at least kHeightStepExponentMin.
int32 SixteenBitStepExponent(float32 range);

/// Chooses deterministic power-of-two steps from row-major level-0 samples and footprint ranges.
/// The step limits wide pages and integer magnitude, and never falls below kHeightStepExponentMin.
/// Faces follow one another in row-major order; memory uses two floats per level-0 page.
class HeightStepPlanner
{
public:
    HeightStepPlanner(uint32 samplesX, uint32 samplesZ, uint32 faces);

    void Push(std::span<const float32> samples);

    /// Each level's step exponent once every sample is pushed; levels past the store's are 0.
    std::array<int8, kPageStoreMaxLevels> StepExponents() const;

private:
    void PushRowSpan(std::span<const float32> samples, uint32 firstX);

    std::vector<PageStoreLevel> m_Levels;
    std::vector<float32> m_PageMin; // per level-0 page: the lowest sample under its footprint
    std::vector<float32> m_PageMax;
    float32 m_Magnitude = 0.0f; // the largest |sample|
    uint32 m_Faces = 0;
    std::size_t m_FacePages = 0; // level-0 pages of one face
    uint32 m_X = 0;              // where the next pushed sample lands
    uint32 m_Z = 0;
    uint32 m_Face = 0;
};

} // namespace GameEngine::PageStreaming
