/**
 * @file MaterialVertexAnimation.h
 * @brief The single predicate for "this material moves its own vertices".
 *
 * Either vertex-modifier form displaces the vertex in the vertex stage, from
 * inputs the CPU never sees — a modifier reads the shared animation clock
 * (`inst.deformationTimeSeconds`), so its rasterized depth can differ from
 * last frame's while every instance record is byte-identical.
 *
 * Two consumers read it for two different reasons: the depth class (a
 * deformed surface needs its own depth variant rather than the shared
 * position-only one) and the per-frame content signal (the depth-derived
 * caches must not retain a layer rasterized at an older deformation).
 * Unknown modifiers remain conservative. A built-in impostor card still
 * needs its depth variant when the authored wind amplitude is zero.
 */
#pragma once

#include "Engine/Rendering/Material.h"

#include "Rendering/Materials/Shader/ShaderVariantKey.h"
#include "Types/Fnv1a.h"

#include <limits>

namespace GameEngine::Engine::Renderer
{

inline bool MaterialDeformsVertices(const Material& mat)
{
    namespace R = ::GameEngine::Rendering;
    const R::MaterialKeyword keywords = mat.GetVariantKey().materialKeywords;
    return R::HasKeyword(keywords, R::MaterialKeyword::HasVertexMod)
        || R::HasKeyword(keywords, R::MaterialKeyword::HasVertexOutputMod);
}

inline bool MaterialHasAnimatedVertices(const Material& mat)
{
    if (!MaterialDeformsVertices(mat))
        return false;
    const auto& spec = mat.GetCompileSpec();
    if (spec.surfaceShaderPath != "Surfaces/impostor.glsl" ||
        spec.vertexModifierPath != "VertexModifiers/impostor_card.glsl")
        return true;
    // The card faces its pass's viewer. Only the sway reads the animation clock.
    const float amplitude = mat.GetFloat(HashStringId("foliageWindSway"),
                                         std::numeric_limits<float>::quiet_NaN());
    return !(amplitude <= 0.0f);
}

} // namespace GameEngine::Engine::Renderer
