#pragma once

#include <cstdint>

namespace GameEngine::Rendering
{

enum class SpirvTarget : uint8_t
{
    Spirv_1_3,
    Spirv_1_5,
    Spirv_1_6
};

/// Code-generation inputs captured with one shader request.
struct ShaderCompileTarget
{
    SpirvTarget Spirv = SpirvTarget::Spirv_1_5;
    bool Compatibility = false;
    bool InterpolationFunctions = false;
    bool ClipDistances = false;

    bool operator==(const ShaderCompileTarget&) const = default;

    constexpr uint32_t Hash() const
    {
        return static_cast<uint32_t>(Spirv) |
               (static_cast<uint32_t>(Compatibility) << 2u) |
               (static_cast<uint32_t>(InterpolationFunctions) << 3u) |
               (static_cast<uint32_t>(ClipDistances) << 4u);
    }
};

/// Capture the device's published compile inputs in one atomic read.
ShaderCompileTarget CurrentShaderCompileTarget();

} // namespace GameEngine::Rendering
