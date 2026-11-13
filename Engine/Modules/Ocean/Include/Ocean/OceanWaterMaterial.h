#pragma once
#include "Components/Rendering/Ocean.h"
#include "Mathematics/Vector4.h"
#include "Types/Color.h"
#include "Types/Types.h"
#include <cstddef>
namespace GameEngine::Ocean
{
// A surface preset supplies material values; world/simulation fields are not overridden.
// std430 mirror in ocean_material_profiles.glsl. Eight-point footprints match water bodies.
struct alignas(16) OceanWaterMaterialGPU
{
    Mathematics::Vector4 Bounds;
    Mathematics::Vector4 Points[8];
    float PointCount = 0.0f;
    float _MetaPad[3]{}; // mirrors metaPad0..2 in ocean_material_profiles.glsl
    ColorLinear Deep{0.0f, 0.0f, 0.0f, 0.0f}, Diffuse{0.0f, 0.0f, 0.0f, 0.0f}, Grazing{0.0f, 0.0f, 0.0f, 0.0f}, Shadow{0.0f, 0.0f, 0.0f, 0.0f};
    ColorLinear Shallow{0.0f, 0.0f, 0.0f, 0.0f}, Subsurface{0.0f, 0.0f, 0.0f, 0.0f}, Foam{0.0f, 0.0f, 0.0f, 0.0f};
    Mathematics::Vector4 Fog;              // xyz extinction, w refraction strength
    Mathematics::Vector4 Surface;          // normal strength, normal scale, roughness, specular
    Mathematics::Vector4 FoamDetail;       // amount, scale, feather, relief
    Mathematics::Vector4 Bubbles;          // coverage, parallax, foam roughness, fresnel power
    Mathematics::Vector4 Optics;           // reflection strength, IOR air, IOR water, subsurface strength
    uint32 FoamTexture = ~0u;   // bindless foam texture index; ~0 = procedural foam
    uint32 NormalTexture = ~0u; // bindless detail normal index; ~0 = none
    uint32 EntityId = 0u;       // owning entity, the stable tiebreak between equal priorities
    int32 Priority = 0;         // higher applies later, over lower priorities
};
static_assert(sizeof(OceanWaterMaterialGPU) == 368);
// std430 offsets the GLSL mirror relies on: the point count lane pads to a vec4.
static_assert(offsetof(OceanWaterMaterialGPU, Deep) == 160);
static_assert(offsetof(OceanWaterMaterialGPU, FoamTexture) == 352);
OceanWaterMaterialGPU BuildOceanWaterMaterial(const Components::OceanSurface &surface);
} // namespace GameEngine::Ocean
