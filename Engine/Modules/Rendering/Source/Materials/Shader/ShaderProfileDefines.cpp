#include "Rendering/Materials/Shader/ShaderProfileDefines.h"

#include "Rendering/Core/RendererProfile.h"
#include "Rendering/Materials/Shader/ShaderCompileService.h"

#include <atomic>

namespace GameEngine::Rendering
{
namespace
{
constexpr uint32_t kSpirvMask = 3u;
constexpr uint32_t kCompatibilityBit = 1u << 2u;
constexpr uint32_t kInterpolationBit = 1u << 3u;
constexpr uint32_t kClipDistanceBit = 1u << 4u;
std::atomic<uint32_t> s_CompileTarget{ShaderCompileTarget{}.Hash()};

void ReplaceTargetBits(uint32_t mask, uint32_t value)
{
    uint32_t before = s_CompileTarget.load(std::memory_order_relaxed);
    while (!s_CompileTarget.compare_exchange_weak(before, (before & ~mask) | (value & mask),
                                                 std::memory_order_relaxed))
    {
    }
}
} // namespace

ShaderCompileTarget CurrentShaderCompileTarget()
{
    const uint32_t packed = s_CompileTarget.load(std::memory_order_relaxed);
    return {static_cast<SpirvTarget>(packed & kSpirvMask),
            (packed & kCompatibilityBit) != 0u, (packed & kInterpolationBit) != 0u,
            (packed & kClipDistanceBit) != 0u};
}

bool IsCompatShaderProfile()
{
    return CurrentShaderCompileTarget().Compatibility;
}

void SetCompatShaderProfile(bool compat)
{
    ReplaceTargetBits(kCompatibilityBit, compat ? kCompatibilityBit : 0u);
}

bool AreInterpolationFunctionsAvailable()
{
    return CurrentShaderCompileTarget().InterpolationFunctions;
}

void SetInterpolationFunctionsAvailable(bool available)
{
    ReplaceTargetBits(kInterpolationBit, available ? kInterpolationBit : 0u);
}

void ShaderCompileService::SetMaxSupportedSpirv(SpirvTarget target)
{
    ReplaceTargetBits(kSpirvMask, static_cast<uint32_t>(target));
}

SpirvTarget ShaderCompileService::MaxSupportedSpirv()
{
    return CurrentShaderCompileTarget().Spirv;
}

bool AreClipDistancesAvailable()
{
    return CurrentShaderCompileTarget().ClipDistances;
}

void SetClipDistancesAvailable(bool available)
{
    ReplaceTargetBits(kClipDistanceBit, available ? kClipDistanceBit : 0u);
}

void ApplyShaderCompileProfile(const RendererProfile& profile)
{
    const uint32_t value = (profile.IsCompat() ? kCompatibilityBit : 0u) |
                           (profile.UseInterpolationFunctions ? kInterpolationBit : 0u) |
                           (profile.UseClipDistance ? kClipDistanceBit : 0u);
    ReplaceTargetBits(kCompatibilityBit | kInterpolationBit | kClipDistanceBit, value);
}

void ApplyShaderCompileProfile(const ShaderCompileTarget& target)
{
    s_CompileTarget.store(target.Hash(), std::memory_order_relaxed);
}

} // namespace GameEngine::Rendering
