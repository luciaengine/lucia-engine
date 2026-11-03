#pragma once

#include "Audio/AudioSystem.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstring>

namespace GameEngine::Audio::Detail
{

// Final device write: the recorder hears the full mix, then speaker mute is
// applied, and the master meter measures exactly what the device receives.
inline float FinalizeDeviceOutput(float* samples, std::uint32_t frameCount,
                                 std::uint32_t channels, std::uint32_t sampleRate,
                                 const std::atomic<bool>& muted,
                                 const AudioSystem::OutputCaptureCallback& capture)
{
    if (!samples || channels == 0)
        return 0.0f;
    if (capture)
        capture(samples, frameCount, channels, sampleRate);
    const std::size_t sampleCount = static_cast<std::size_t>(frameCount) * channels;
    if (muted.load(std::memory_order_relaxed))
        std::memset(samples, 0, sampleCount * sizeof(float));
    float peak = 0.0f;
    for (std::size_t i = 0; i < sampleCount; ++i)
        peak = std::max(peak, std::fabs(samples[i]));
    return peak;
}

} // namespace GameEngine::Audio::Detail
