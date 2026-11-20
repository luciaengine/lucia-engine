#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace GameEngine::Rendering
{
class SpecializationConstants;
struct ViewDesc;
}

namespace GameEngine::Engine::Renderer
{
class RenderServices;

// Matches GE_FORWARD_HEX_TILING in forward_shader_specialization.glsl.
inline constexpr uint32_t kHexTilingShaderConstant = 302;

/// Derives forward pipeline constants from live scene and material state.
class ForwardShaderSpecialization
{
  public:
    /// Resolve immutable constants during pass declaration.
    std::shared_ptr<const Rendering::SpecializationConstants> Resolve(
        RenderServices& services, const Rendering::ViewDesc& view);

  private:
    bool HasHexMaterials(RenderServices& services);

    uint64_t m_MaterialEpoch = 0;
    size_t m_MaterialCount = 0;
    bool m_HexMaterials = true;
    std::array<int32_t, 6> m_State{};
    std::shared_ptr<const Rendering::SpecializationConstants> m_Constants;
};
}
