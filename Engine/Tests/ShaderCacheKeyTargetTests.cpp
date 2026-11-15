#include "Engine/Rendering/ShaderCompilationCache.h"

#include "ScopedShaderCompileTarget.h"

#include <gtest/gtest.h>

#include <functional>

using namespace GameEngine;
using namespace GameEngine::Engine::Renderer;

namespace
{

ShaderCacheKey MakeKey()
{
    ShaderCacheKey key{};
    key.VariantKey.vertexFlags = Rendering::VertexAttributeFlags::StandardMesh;
    key.Source.SurfaceShaderPath = "surface.glsl";
    return key;
}

} // namespace

// A key captures the compile target when it is made. A later device change does not move it.
TEST(ShaderCacheKeyTarget, KeyCapturesTheTargetWhenItIsMade)
{
    Rendering::ShaderCompileTarget compat{};
    compat.Compatibility = true;
    ShaderCacheKey captured{};
    {
        TestSupport::ScopedShaderCompileTarget scope(compat);
        captured = MakeKey();
    }
    EXPECT_EQ(captured.Target, compat);
    EXPECT_NE(captured.Target, Rendering::CurrentShaderCompileTarget());
}

// Two programs compiled for different targets never share a cache entry.
TEST(ShaderCacheKeyTarget, TargetSeparatesTheIdentity)
{
    ShaderCacheKey desktop = MakeKey();
    desktop.Target = Rendering::ShaderCompileTarget{};
    ShaderCacheKey compat = desktop;
    compat.Target.Compatibility = true;
    EXPECT_FALSE(desktop == compat);
    EXPECT_NE(std::hash<ShaderCacheKey>{}(desktop), std::hash<ShaderCacheKey>{}(compat));

    ShaderCacheKey same = desktop;
    EXPECT_TRUE(desktop == same);
    EXPECT_EQ(std::hash<ShaderCacheKey>{}(desktop), std::hash<ShaderCacheKey>{}(same));
}
