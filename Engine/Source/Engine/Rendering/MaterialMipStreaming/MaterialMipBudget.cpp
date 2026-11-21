#include "Engine/Rendering/MaterialMipStreaming/MaterialMipBudget.h"

#include "Rendering/Core/Device.h"
#include "Types/ParseNumber.h"

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <optional>

namespace GameEngine::Engine::Renderer
{

namespace
{
constexpr uint64 kBytesPerMiB = 1024ull * 1024ull;
constexpr uint64 kDiscreteShareDivisor = 2;
constexpr uint64 kUnifiedShareDivisor = 4;
// Steam Deck: 16 GiB of unified memory. One quarter is 4 GiB; the system and the CPU side of a
// game take a larger part there than on a desktop, so the budget is one half of that quarter.
[[maybe_unused]] constexpr uint64 kSteamDeckBytes = 2048ull * kBytesPerMiB;
} // namespace

uint64 MaterialMipBudget::FromDevice(const Rendering::RenderingDeviceCapabilities& caps)
{
    if (caps.memoryTopology && caps.memoryTopology->deviceLocalHeapBytesTotal != 0)
    {
        const uint64 total = caps.memoryTopology->deviceLocalHeapBytesTotal;
        // A unified device can report a small device-local heap while it uses the system RAM.
        return caps.memoryTopology->isUnifiedMemory ? std::max(total / kUnifiedShareDivisor, kMinimumBytes)
                                                    : total / kDiscreteShareDivisor;
    }
    if (caps.dedicatedVideoMemory != 0)
        return static_cast<uint64>(caps.dedicatedVideoMemory) / kDiscreteShareDivisor;
    return kMinimumBytes;
}

uint64 MaterialMipBudget::ForPlatform(const Rendering::RenderingDeviceCapabilities& caps)
{
#if defined(PLATFORM_STEAMDECK)
    uint64 bytes = kSteamDeckBytes;
    static_cast<void>(caps);
#else
    uint64 bytes = FromDevice(caps);
#endif
#ifndef NDEBUG
    if (const char* value = std::getenv("GE_MATERIAL_MIP_BUDGET_MB"))
    {
        const std::optional<uint64_t> budgetMiB = ParseUnsigned(value);
        if (budgetMiB && *budgetMiB <= std::numeric_limits<uint32>::max())
            bytes = *budgetMiB * kBytesPerMiB;
    }
#endif
    return bytes;
}

} // namespace GameEngine::Engine::Renderer
