#pragma once

#include "Types/Types.h"

namespace GameEngine::Rendering
{
struct RenderingDeviceCapabilities;
} // namespace GameEngine::Rendering

namespace GameEngine::Engine::Renderer
{

/// The bytes that material mip streams can hold on a device: a share of its memory.
class MaterialMipBudget
{
  public:
    /// The share of the memory that `caps` reports: one half of the device-local memory of a
    /// discrete GPU, or of its dedicated video memory when the backend gives no memory topology.
    /// Unified memory, which the CPU and all other GPU data also use, gives one quarter and not
    /// less than kMinimumBytes. kMinimumBytes when the backend reports no memory size.
    static uint64 FromDevice(const Rendering::RenderingDeviceCapabilities& caps);

    /// The budget of the platform that this build targets: FromDevice, or a fixed value in a
    /// Steam Deck build. A build that does not define NDEBUG (Debug, DebugFast) reads
    /// GE_MATERIAL_MIP_BUDGET_MB, a test override in MiB; 0 there turns streaming off.
    static uint64 ForPlatform(const Rendering::RenderingDeviceCapabilities& caps);

    /// The budget of a device that reports no memory size, and the least budget of unified
    /// memory: the share of a discrete GPU with 2 GiB.
    static constexpr uint64 kMinimumBytes = 1024ull * 1024ull * 1024ull;
};

} // namespace GameEngine::Engine::Renderer
