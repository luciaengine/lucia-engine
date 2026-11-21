#pragma once

#include "Mathematics/Matrix4x4.h"
#include "Mathematics/Vector4.h"
#include "Types/Types.h"

#include <cstddef>
#include <type_traits>

namespace GameEngine::Engine::Renderer
{

// The non-instanced PC block in adapter_vertex.glsl, with identity transforms.
struct alignas(16) IdentityMeshDrawConstants
{
    explicit IdentityMeshDrawConstants(uint32 materialIndex) : MaterialIndex(materialIndex) {}

    Mathematics::Matrix4x4 Model{};
    Mathematics::Vector4 NormalColumn0{1, 0, 0, 0};
    Mathematics::Vector4 NormalColumn1{0, 1, 0, 0};
    Mathematics::Vector4 NormalColumn2{0, 0, 1, 0};
    uint32 MaterialIndex;
    uint32 SkinPaletteOffset = 0;
    uint32 Reserved0 = 0;
    uint32 Reserved1 = 0;
};
static_assert(std::is_trivially_copyable_v<IdentityMeshDrawConstants>);
static_assert(sizeof(IdentityMeshDrawConstants) == 128);
static_assert(offsetof(IdentityMeshDrawConstants, NormalColumn0) == 64);
static_assert(offsetof(IdentityMeshDrawConstants, NormalColumn1) == 80);
static_assert(offsetof(IdentityMeshDrawConstants, NormalColumn2) == 96);
static_assert(offsetof(IdentityMeshDrawConstants, MaterialIndex) == 112);
static_assert(offsetof(IdentityMeshDrawConstants, SkinPaletteOffset) == 116);

} // namespace GameEngine::Engine::Renderer
