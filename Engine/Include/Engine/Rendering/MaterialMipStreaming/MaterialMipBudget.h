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
    /// The share of the memory that `caps` reports. One half of the device-local memory of a
    /// discrete GPU, and one quarter of unified memory, which the CPU and all other GPU data also
    /// use. kUnknownDeviceBytes when the backend reports no memory size.
    static uint64 FromDevice(const Rendering::RenderingDeviceCapabilities& caps);

    /// The budget of the platform that this build targets. A Steam Deck build uses
    /// kSteamDeckBytes. A build that is not Release reads GE_MATERIAL_MIP_BUDGET_MB, a test
    /// override in MiB; 0 there turns streaming off.
    static uint64 ForPlatform(const Rendering::RenderingDeviceCapabilities& caps);

    static constexpr uint64 kBytesPerMiB = 1024ull * 1024ull;
    /// A backend that reports no memory size: the share of a GPU with 2 GiB.
    static constexpr uint64 kUnknownDeviceBytes = 1024ull * kBytesPerMiB;
    /// Steam Deck: 16 GiB of unified memory, of which a game gets about one half.
    static constexpr uint64 kSteamDeckBytes = 2048ull * kBytesPerMiB;
};

} // namespace GameEngine::Engine::Renderer
