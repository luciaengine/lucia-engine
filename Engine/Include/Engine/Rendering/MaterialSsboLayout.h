#pragma once

#include "Engine/Rendering/Material.h"
#include "Rendering/Materials/MaterialParamsLayout.h"

#include <cstdint>

namespace GameEngine::Engine::Renderer
{

// One MaterialParams SSBO row (set 0, binding 13), the GLSL `MaterialData`: the param block, 8 texture
// indices, 8 x 2 UV rows, the packed sampler uint and the normal-variance index. The param size comes
// from the shared field list (material_params.glsl): a hardcoded size misaligns every texture index.
inline constexpr uint32_t kMaterialParamBytes = GameEngine::kMaterialParamBlockBytes;
inline constexpr uint32_t kTextureIndexBytes = kTextureSlotArraySize * sizeof(uint32_t);
inline constexpr uint32_t kTextureSTBytes = kTextureSlotArraySize * 8 * sizeof(float); // two affine rows per slot
inline constexpr uint32_t kSamplerIndicesOffset = kMaterialParamBytes + kTextureIndexBytes + kTextureSTBytes;
// Not a material slot: the texture service writes it when the normal slot binds, black otherwise.
inline constexpr uint32_t kNormalVarianceIndexOffset = kSamplerIndicesOffset + sizeof(uint32_t);
// std430 rounds the MaterialData array stride up to vec4 alignment, so the two tail
// uints own a full 16-byte block (8 used + 8 pad).
inline constexpr uint32_t kMaterialEntryStride = kSamplerIndicesOffset + 16;
static_assert(kMaterialEntryStride % 16 == 0, "MaterialData std430 array stride must be 16-byte aligned");
static_assert(kNormalVarianceIndexOffset + sizeof(uint32_t) <= kMaterialEntryStride,
              "the normal-variance index must fit in the tail block");

} // namespace GameEngine::Engine::Renderer
