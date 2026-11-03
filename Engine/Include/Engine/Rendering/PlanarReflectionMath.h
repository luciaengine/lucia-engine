#pragma once

#include "Components/Rendering/PlanarReflection.h"
#include "Rendering/CameraTypes.h"
#include <glm/glm.hpp>
#include <cstdint>

namespace GameEngine::Engine::Renderer
{
inline constexpr float kPlanarClipBias = 0.01f;
inline constexpr float kPlanarSurfaceTolerance = 0.02f;
inline constexpr float kPlanarEdgeFade = 0.02f;
float PlanarReflectionBlurSigma(float roughness, float resolutionScale);
// Shared arbitrary-plane, reverse-Z camera construction for mirrors and oceans.
// Ocean retains its above-water clipping convention for underwater views.
bool MakeMirrorCaptureCamera(const Rendering::CameraData& main, const glm::vec4& unitPlane,
                             float clipBias, bool oneSided, Rendering::CameraData& reflected);
struct PlanarReflectionPlane
{
    uint32_t EntityId = 0;
    Components::PlanarReflection Settings{};
    glm::mat4 WorldToLocal{1.0f};
    glm::vec4 Equation{0.0f, 1.0f, 0.0f, 0.0f};
};

// Center followed by paired positive-side taps; xy = offset, normalized weight.
// Sample each non-center tap at both +/- offset using linear filtering.
struct PlanarReflectionBlurKernel
{
    glm::vec4 Taps[7]{};
    uint32_t Count = 1;
};
PlanarReflectionBlurKernel MakePlanarReflectionBlurKernel(float sigma);

// Reject singular/non-finite transforms rather than sending invalid inverses
// to camera culling. The normal is inverse-transpose, including parent shear.
bool ResolvePlanarReflectionPlane(const float* worldMatrix, uint32_t entityId,
                                 const Components::PlanarReflection& settings,
                                 PlanarReflectionPlane& result);
glm::mat4 PlanarReflectionMatrix(const glm::vec4& unitPlane);
bool MakePlanarReflectionCamera(const Rendering::CameraData& main,
                                const PlanarReflectionPlane& plane,
                                Rendering::CameraData& reflected);
} // namespace GameEngine::Engine::Renderer
