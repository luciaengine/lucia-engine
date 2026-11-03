/**
 * @file MaterialColorClassSignature.h
 * @brief Collision-free key identifying a color-PSO class for the P2 merge.
 *
 * Exact-value equality (never a lossy hash of the discriminant): two materials
 * with an equal signature compile to a byte-identical color GraphicsPipelineDesc,
 * so they may share one merged indirect batch with a single representative
 * binding the pipeline. Populated by ComputeColorClassSignature.
 */
#pragma once

#include "Rendering/Core/HashUtils.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace GameEngine::Engine::Renderer
{

struct ColorClassSignature
{
    std::string SurfaceShaderPath;
    std::string VertexModifierPath;
    std::string LightingModel;
    uint64_t    MaterialKeywords  = 0u;
    uint8_t     AlphaMode         = 0u;
    bool        DoubleSided       = false;
    bool        IgnoreVertexColor = false;

    bool operator==(const ColorClassSignature&) const = default;
};

} // namespace GameEngine::Engine::Renderer

namespace std
{
template <>
struct hash<GameEngine::Engine::Renderer::ColorClassSignature>
{
    size_t operator()(
        const GameEngine::Engine::Renderer::ColorClassSignature& s) const noexcept
    {
        using GameEngine::Rendering::HashUtils::HashCombine;
        uint64_t h = std::hash<std::string>{}(s.SurfaceShaderPath);
        h = HashCombine(h, std::hash<std::string>{}(s.VertexModifierPath));
        h = HashCombine(h, std::hash<std::string>{}(s.LightingModel));
        h = HashCombine(h, s.MaterialKeywords);
        h = HashCombine(h, (static_cast<uint32_t>(s.AlphaMode) << 2)
                               | (s.DoubleSided ? 2u : 0u)
                               | (s.IgnoreVertexColor ? 1u : 0u));
        return static_cast<size_t>(h);
    }
};
} // namespace std
