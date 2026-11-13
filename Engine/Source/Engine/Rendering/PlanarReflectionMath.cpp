#include "Engine/Rendering/PlanarReflectionMath.h"
#include "Engine/Rendering/RenderOrigin.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <glm/gtc/matrix_access.hpp>
#include <glm/gtc/type_ptr.hpp>

namespace GameEngine::Engine::Renderer
{
namespace
{
bool Finite(const glm::mat4& m)
{
    const float* p = glm::value_ptr(m);
    return std::all_of(p, p + 16, [](float x) { return std::isfinite(x); });
}
float ClampFinite(float value, float lo, float hi, float fallback)
{
    return std::isfinite(value) ? std::clamp(value, lo, hi) : fallback;
}
}

float PlanarReflectionBlurSigma(float roughness, float resolutionScale)
{
    const float r = ClampFinite(roughness, 0.0f, 1.0f, 0.0f);
    return std::min(4.0f, r * r * 12.0f * ClampFinite(resolutionScale, 0.125f, 1.0f, 0.5f));
}

PlanarReflectionBlurKernel MakePlanarReflectionBlurKernel(float sigma)
{
    PlanarReflectionBlurKernel result{};
    result.Taps[0].y = 1.0f;
    if (!std::isfinite(sigma) || sigma <= 0.0f) return result;
    sigma = std::min(sigma, 4.0f);
    const int radius = std::min(12, static_cast<int>(std::ceil(3.0f * sigma)));
    float weights[14]{};
    weights[0] = 1.0f;
    float total = 1.0f;
    for (int i = 1; i <= radius; ++i)
    {
        weights[i] = std::exp(-0.5f * (i / sigma) * (i / sigma));
        total += 2.0f * weights[i];
    }
    result.Taps[0].y = 1.0f / total;
    for (int i = 1; i <= radius; i += 2)
    {
        const float sum = weights[i] + weights[i + 1];
        if (sum <= 1e-20f) continue;
        result.Taps[result.Count++] = glm::vec4(i + weights[i + 1] / sum, sum / total, 0, 0);
    }
    return result;
}

bool ResolvePlanarReflectionPlane(const float* worldMatrix, uint32_t entityId,
                                 const Components::PlanarReflection& settings,
                                 PlanarReflectionPlane& result)
{
    if (!worldMatrix || settings.CullMask == 0)
        return false;
    const auto world = glm::make_mat4(worldMatrix);
    if (!Finite(world) || std::abs(glm::determinant(world)) < 1e-8f)
        return false;
    result = {};
    result.EntityId = entityId;
    result.Settings = settings;
    auto& s = result.Settings;
    s.ResolutionScale = ClampFinite(s.ResolutionScale, 0.125f, 1.0f, 0.5f);
    s.MaxRoughness = ClampFinite(s.MaxRoughness, 0.0f, 1.0f, 0.35f);
    result.WorldToLocal = glm::inverse(world);
    auto p = glm::transpose(result.WorldToLocal) * glm::vec4(0, 1, 0, 0);
    const float n = glm::length(glm::vec3(p));
    if (!(n > 1e-6f) || !Finite(result.WorldToLocal))
        return false;
    result.Equation = p / n;
    return true;
}

glm::mat4 PlanarReflectionMatrix(const glm::vec4& unitPlane)
{
    const glm::vec3 n(unitPlane);
    glm::mat4 mirror(1.0f);
    for (int c = 0; c < 3; ++c)
        for (int r = 0; r < 3; ++r)
            mirror[c][r] -= 2.0f * n[c] * n[r];
    mirror[3] = glm::vec4(-2.0f * unitPlane.w * n, 1.0f);
    return mirror;
}

bool MakePlanarReflectionCamera(const Rendering::CameraData& main,
                                const PlanarReflectionPlane& plane,
                                Rendering::CameraData& reflected)
{
    return MakeMirrorCaptureCamera(main, plane.Equation, kPlanarClipBias, true, reflected);
}

bool MakeMirrorCaptureCamera(const Rendering::CameraData& main, const glm::vec4& unitPlane,
                             float clipBias, bool oneSided, Rendering::CameraData& reflected)
{
    const auto view = main.view.GetGLM();
    const auto proj = main.proj.GetGLM();
    if (!Finite(view) || !Finite(proj) || std::abs(glm::determinant(proj)) < 1e-12f)
        return false;
    const auto eye = CameraEye(main);
    const glm::dvec3 physicalEye(eye.X, eye.Y, eye.Z);
    glm::dvec3 normal(unitPlane);
    const double length = glm::length(normal);
    if (!std::isfinite(length) || length < 1e-6)
        return false;
    normal /= length;
    const double planeOffset = static_cast<double>(unitPlane.w) / length;
    const double distance = glm::dot(normal, physicalEye) + planeOffset;
    // Form distances before narrowing the eye into an absolute float position.
    if (oneSided && !(distance > static_cast<double>(clipBias) + 1e-4))
        return false;
    const glm::dvec3 mirroredEye = physicalEye - 2.0 * distance * normal;
    const glm::dmat3 reflection = glm::dmat3(1.0) - 2.0 * glm::outerProduct(normal, normal);
    const glm::dmat3 reflectedRotation = glm::dmat3(view) * reflection;
    const glm::dvec3 translation = -reflectedRotation * mirroredEye;
    glm::mat4 reflectedView(1.0f);
    for (int c = 0; c < 3; ++c)
        for (int r = 0; r < 3; ++r)
            reflectedView[c][r] = static_cast<float>(reflectedRotation[c][r]);
    reflectedView[3] = glm::vec4(glm::vec3(translation), 1.0f);
    // Transform the plane with the reflected rotation and its exact eye, avoiding
    // cancellation in the inverse of a rounded absolute view translation.
    const glm::dvec3 clipNormal = glm::transpose(glm::inverse(reflectedRotation)) * normal;
    const glm::vec4 clipPlane(glm::vec3(clipNormal), static_cast<float>(-distance - clipBias));
    const auto sign = [](float x) { return x >= 0.0f ? 1.0f : -1.0f; };
    const auto q = glm::inverse(proj) * glm::vec4(sign(clipPlane.x), sign(clipPlane.y), 0, 1);
    const float denominator = glm::dot(clipPlane, q);
    if (!std::isfinite(denominator) || std::abs(denominator) < 1e-6f)
        return false;
    // Reverse Z [0,1]: row3-row2 is near, row2 is far. Replace the near
    // plane while preserving the opposite far corner, for arbitrary mirror tilt.
    glm::mat4 reflectedProj = proj;
    const auto row3 = glm::row(proj, 3);
    const auto row2 = row3 - clipPlane * (glm::dot(row3, q) / denominator);
    for (int c = 0; c < 4; ++c) reflectedProj[c][2] = row2[c];
    const auto vp = reflectedProj * reflectedView;
    if (!Finite(vp)) return false;
    reflected = main;
    reflected.view = Mathematics::Matrix4x4(reflectedView);
    reflected.proj = Mathematics::Matrix4x4(reflectedProj);
    reflected.viewProj = Mathematics::Matrix4x4(vp);
    SetCameraEye(reflected, Mathematics::WorldPosition{mirroredEye.x, mirroredEye.y, mirroredEye.z});
    return true;
}
} // namespace GameEngine::Engine::Renderer
