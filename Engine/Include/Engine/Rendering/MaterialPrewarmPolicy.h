#pragma once

#include "Rendering/Materials/Shader/ShaderVariantKey.h"
#include <cstddef>

namespace GameEngine::Engine::Renderer
{
// Scheduling only: variants omitted from warmup remain available on demand.
// Configure before loading a project; each queued job snapshots this policy.
enum class MaterialPrewarmPolicy { Full, StaticColor };
void SetMaterialPrewarmPolicy(MaterialPrewarmPolicy policy);
MaterialPrewarmPolicy GetMaterialPrewarmPolicy();

template<class Compile>
size_t ForEachMaterialPrewarmVariant(MaterialPrewarmPolicy policy,
    Rendering::MaterialKeyword base, Rendering::MaterialKeyword color, Compile&& compile)
{
    using VF = Rendering::VertexAttributeFlags;
    using MK = Rendering::MaterialKeyword;
    const VF standard = VF::HasPosition | VF::HasNormal | VF::HasUV0;
    const VF tangent = standard | VF::HasTangent;
    size_t count = 0;
    auto pass = [&](MK keywords) {
        compile(standard, keywords); ++count;
        compile(tangent, keywords); ++count;
        if (policy == MaterialPrewarmPolicy::Full) {
            compile(standard | VF::Skinned, keywords); ++count;
            compile(tangent | VF::Skinned, keywords); ++count;
        }
    };
    pass(color);
    if (Rendering::HasKeyword(base, MK::Transmission)) pass(color | MK::SceneColorGrab);
    if (policy == MaterialPrewarmPolicy::Full) pass(MK::Instanced);
    return count;
}
}
