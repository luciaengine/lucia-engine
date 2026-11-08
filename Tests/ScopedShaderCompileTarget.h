#pragma once

#include "Rendering/Materials/ShaderProfileDefines.h"

namespace GameEngine::TestSupport
{

class ScopedShaderCompileTarget
{
public:
    explicit ScopedShaderCompileTarget(const Rendering::ShaderCompileTarget& target)
        : m_Previous(Rendering::CurrentShaderCompileTarget())
    {
        Rendering::ApplyShaderCompileProfile(target);
    }

    ~ScopedShaderCompileTarget()
    {
        Rendering::ApplyShaderCompileProfile(m_Previous);
    }

    ScopedShaderCompileTarget(const ScopedShaderCompileTarget&) = delete;
    ScopedShaderCompileTarget& operator=(const ScopedShaderCompileTarget&) = delete;

private:
    Rendering::ShaderCompileTarget m_Previous;
};

} // namespace GameEngine::TestSupport
