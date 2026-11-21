#include "Engine/Rendering/MaterialMipStreaming/MaterialMipBudget.h"

#include "Rendering/Core/Device.h"
#include "Types/ParseNumber.h"

#include <cstdlib>
#include <limits>
#include <optional>

namespace GameEngine::Engine::Renderer
{

namespace
{
constexpr uint64 kDiscreteShareDivisor = 2;
constexpr uint64 kUnifiedShareDivisor = 4;
} // namespace

uint64 MaterialMipBudget::FromDevice(const Rendering::RenderingDeviceCapabilities& caps)
{
    if (caps.memoryTopology && caps.memoryTopology->deviceLocalHeapBytesTotal != 0)
    {
        const uint64 divisor = caps.memoryTopology->isUnifiedMemory ? kUnifiedShareDivisor : kDiscreteShareDivisor;
        return caps.memoryTopology->deviceLocalHeapBytesTotal / divisor;
    }
    if (caps.dedicatedVideoMemory != 0)
        return static_cast<uint64>(caps.dedicatedVideoMemory) / kDiscreteShareDivisor;
    return kUnknownDeviceBytes;
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
