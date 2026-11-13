#include "Ocean/OceanWaterMaterial.h"

#include "Mathematics/Interpolation.h"
#include <algorithm>
namespace GameEngine::Ocean
{
OceanWaterMaterialGPU BuildOceanWaterMaterial(const Components::OceanSurface &s)
{
    OceanWaterMaterialGPU m{};
    m.Deep = s.DeepColor;
    m.Diffuse = s.Diffuse;
    m.Grazing = s.DiffuseGrazing;
    m.Shadow = s.DiffuseShadow;
    m.Shallow = s.SubSurfaceShallowCol;
    m.Subsurface = s.SubSurfaceColour;
    m.Foam = s.FoamColor;
    m.Fog = Mathematics::Vector4(s.DepthFogDensity.r, s.DepthFogDensity.g, s.DepthFogDensity.b, s.RefractionStrength);
    m.Surface[0] = std::max(s.NormalsStrength, 0.0f);
    m.Surface[1] = std::max(s.NormalsScale, 0.001f);
    m.Surface[2] = Math::Clamp01(s.Roughness);
    m.Surface[3] = std::max(s.Specular, 0.0f);
    m.FoamDetail[0] = std::max(s.FoamAmount, 0.0f);
    m.FoamDetail[1] = std::max(s.FoamScale, 0.01f);
    m.FoamDetail[2] = std::max(s.FoamFeather, 0.001f);
    m.FoamDetail[3] = std::clamp(s.FoamNormalStrength, 0.0f, 2.0f);
    m.Bubbles[0] = Math::Clamp01(s.FoamBubbleCoverage);
    m.Bubbles[1] = std::clamp(s.FoamBubbleParallax, 0.0f, 0.5f);
    m.Bubbles[2] = std::clamp(s.FoamRoughness, 0.04f, 1.0f);
    m.Bubbles[3] = s.FresnelPower;
    m.Optics[0] = s.ReflectionStrength;
    m.Optics[1] = s.IorAir;
    m.Optics[2] = s.IorWater;
    m.Optics[3] = s.SubsurfaceStrength;
    return m;
}
} // namespace GameEngine::Ocean
