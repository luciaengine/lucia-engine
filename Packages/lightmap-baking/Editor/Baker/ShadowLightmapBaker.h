#pragma once

#include <glm/glm.hpp>
#include <cstdint>
#include <functional>
#include <limits>
#include <span>
#include <string>
#include <vector>

namespace GameEngine { struct Mesh; }
namespace GameEngine::Engine::Renderer
{
// UV1 is the imported second UV set, already in the engine's texture convention.
// Each receiver requires a unique non-overlapping unwrap in [0,1]. Transforms
// include the model's source-node transform, exactly as for drawing.
struct LightmapSurface
{
    glm::vec3 Albedo{0.8f}; // diffuse reflectance, scene-linear, metallic removed
    glm::vec3 Emission{0.0f}; // outgoing scene-linear radiance
};
struct ShadowLightmapMesh
{
    const Mesh* Geometry = nullptr;
    glm::mat4 LocalToWorld{1.0f};
    LightmapSurface Surface;
    bool UseVertexColor = true;
    // Optional UV0 material evaluation (albedo/emission textures).
    std::function<LightmapSurface(glm::vec2)> SampleSurface;
};
struct LightmapBounceLight
{
    enum class Type { Directional, Point, Spot };
    Type Kind = Type::Point;
    glm::vec3 Position{0};
    glm::vec3 Direction{0,0,1}; // direction the light shines
    glm::vec3 Radiance{1};
    float Range = 10;
    float InnerAngle = 0.5f, OuterAngle = 0.8f;
};
inline constexpr uint32_t kMaxLightmapResolution = 8192;

// Packed layouts shared with the GPU intersection shader. Triangle indices refer
// to the input's consecutive groups of nine world-space position floats.
struct LightmapRay
{
    glm::vec3 Origin{0};
    float MaxDistance = std::numeric_limits<float>::max();
    glm::vec3 Direction{0,1,0};
    float MinDistance = 0.0000001f;
};
struct LightmapRayHit
{
    float Distance = 0, U = 0, V = 0;
    uint32_t Triangle = UINT32_MAX;
};
using LightmapRayBatch = std::function<bool(std::span<const LightmapRay>, std::span<LightmapRayHit>)>;

// Creates a private headless device and descriptor-bound BLAS/TLAS. Empty means
// unavailable; a false batch result asks the caller to retry that batch on CPU.
LightmapRayBatch CreateGpuLightmapRayTracer(std::span<const float> trianglePositions);

struct ShadowLightmapBakeOptions
{
    uint32_t Width = 1024;
    uint32_t Height = 1024;
    glm::vec3 DirectionToLight{0.0f, 1.0f, 0.0f};
    float AngularRadiusRadians = 0.0f;
    uint32_t RaysPerSample = 32;
    float Bias = 0.001f;
    uint32_t Padding = 4;
    // Zero disables GI, leaving a shadow-only bake. Cosine-weighted diffuse
    // path tracing stores irradiance / PI for multiplication by diffuse albedo.
    uint32_t IndirectSamples = 256;
    uint32_t IndirectBounces = 2;
    // Variance-guided diffuse filtering respects UV charts and surface geometry.
    // Directional visibility is never blurred. Disable for reference/raw bakes.
    bool Denoise = true;
    glm::vec3 LightRadiance{1.0f};
    glm::vec3 EnvironmentRadiance{0.0f};
    // Optional scene-linear environment radiance by world-space direction.
    std::function<glm::vec3(glm::vec3)> SampleEnvironment;
    // Additional scene lights contribute indirect illumination; direct lighting
    // stays live. The primary directional is described by DirectionToLight.
    std::vector<LightmapBounceLight> BounceLights;
    // Called between triangles, rows, ray batches and filter/padding work; false cancels.
    std::function<bool()> Continue;
    // Monotonic work progress in [0,1], called on the baking thread. Reaches 1
    // only after a successful bake; callers must marshal UI updates themselves.
    std::function<void(float)> Progress;
    // Optional acceleration provider. The editor selects GPU ray queries
    // automatically; embedders and CPU-only tests can leave this empty.
    std::function<LightmapRayBatch(std::span<const float>)> CreateRayTracer;
    std::function<void(bool)> AccelerationChanged;
};
struct ShadowLightmapBakeResult
{
    uint32_t Width = 0, Height = 0;
    // Linear visibility: 0 = occluded, 255 = lit. Rows follow UV1.y.
    std::vector<uint8_t> Visibility;
    // Linear HDR RGB irradiance / PI, row-major. Empty for shadow-only bakes.
    // Contains sky, emissive surfaces and bounced directional light. The
    // receiver's direct directional lighting remains realtime.
    std::vector<float> IndirectRGB;
    uint32_t CoveredTexels = 0;
    uint64_t TracedRays = 0;
    uint64_t GpuTracedRays = 0;
};
// Opaque, double-sided static triangle occluders. Include the receiver in
// occluders when it casts shadows. Output remains unchanged on failure/cancel.
// UV-space directional shadowmask with batched diffuse path tracing. GPU
// intersections are optional; material evaluation and filtering are shared.
bool BakeShadowLightmap(const ShadowLightmapMesh& receiver,
                        std::span<const ShadowLightmapMesh> occluders,
                        const ShadowLightmapBakeOptions& options,
                        ShadowLightmapBakeResult& output,
                        std::string& error);

// Godot shadowmask modes: Replace fades live visibility into the bake; Overlay
// multiplies the bake by live visibility fading to 1. fade is [0,1].
enum class ShadowLightmapMode : uint32_t { None, Replace, Overlay, Only };
float BlendShadowLightmap(float realtime, float baked, float fade, ShadowLightmapMode mode);
}
