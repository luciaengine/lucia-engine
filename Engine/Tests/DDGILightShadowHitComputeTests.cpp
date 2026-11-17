// Run the production shadow hit rules of a DDGI light. The shadow ray of an emitter
// proxy goes to the proxy center, inside the emitter mesh: a hit on that mesh must
// not hide the proxy, and a hit on each other instance must hide it.
#include <gtest/gtest.h>

#include "DDGICaseKernel.h"
#include "TestDeviceHelper.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace
{
// Mirrors ShadowHitCase in Tests/Shaders/ddgi_light_shadow_hit_test.comp:
// the nine lanes of Includes/light_packed_fields.glsl, then the hit lane.
struct ShadowHitCase
{
    std::array<uint32_t, 4> Meta;  // x = type, w = casts light
    std::array<float, 4> PosRange;
    std::array<float, 4> DirIntensity;
    std::array<float, 4> ColorAreaWidth;
    std::array<float, 4> AreaParams;
    std::array<float, 4> SpotParams;
    std::array<float, 4> AreaRight;
    std::array<float, 4> AreaUp;
    std::array<int32_t, 4> ShadowSlots;  // y = the emitter instance of a proxy
    std::array<int32_t, 4> Hit;          // x = the instance that the shadow ray hit
    std::array<float, 4> Ray;            // x = distance to the light center, y = tMin, z = tMax
};
static_assert(sizeof(ShadowHitCase) == 176, "must match the std430 stride of ShadowHitCase");

constexpr uint32_t kPointLight = 1;
constexpr uint32_t kEmitterProxy = 3;  // GE_DDGI_LIGHT_TYPE_EMITTER_PROXY
constexpr int32_t kNoEmitter = -1;     // the lane value of a light that is not a proxy
constexpr int32_t kEmitterInstance = 7;
constexpr int32_t kOtherInstance = 3;

ShadowHitCase Case(uint32_t type, int32_t emitterInstance, int32_t hitInstance)
{
    ShadowHitCase c{};
    c.Meta = {type, 1u, 0u, 1u};
    c.ShadowSlots = {-1, emitterInstance, -1, -1};
    c.Hit = {hitInstance, 0, 0, 0};
    return c;
}

TEST(DDGILightShadowHitComputeTest, OnlyTheEmitterMeshDoesNotHideItsProxy)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    DDGICaseKernel kernel;
    std::string error;
    ASSERT_TRUE(kernel.Load(*device, "ddgi_light_shadow_hit_test", error)) << error;

    const auto results = kernel.Run(std::vector<ShadowHitCase>{
        // The ray to a proxy hit the emitter's own mesh: the proxy stays visible.
        Case(kEmitterProxy, kEmitterInstance, kEmitterInstance),
        // The ray to a proxy hit a different instance first: that instance hides the proxy.
        Case(kEmitterProxy, kEmitterInstance, kOtherInstance),
        // Each hit hides a point light.
        Case(kPointLight, kNoEmitter, kOtherInstance),
        // The light type decides, not the lane: a point light has no mesh of its own.
        Case(kPointLight, kEmitterInstance, kEmitterInstance)});
    ASSERT_EQ(results.size(), 4u);
    EXPECT_EQ(results[0][0], 0.0f);
    EXPECT_EQ(results[1][0], 1.0f);
    EXPECT_EQ(results[2][0], 1.0f);
    EXPECT_EQ(results[3][0], 1.0f);
    // Only the ray to a proxy needs its nearest hit.
    EXPECT_EQ(results[0][1], 1.0f);
    EXPECT_EQ(results[2][1], 0.0f);
}

ShadowHitCase ProxyRay(float boundingRadius, float distanceToCenter, float tMin, float tMax)
{
    ShadowHitCase c = Case(kEmitterProxy, kEmitterInstance, kOtherInstance);
    c.AreaParams = {0.0f, boundingRadius, 2.0f, 0.0f};  // y = the bounding radius of the emitter
    c.Ray = {distanceToCenter, tMin, tMax, 0.0f};
    return c;
}

// The first segment of a proxy shadow ray ends at the bounding sphere of the emitter. The emitter mesh is
// inside that sphere, so the first hit that the traversal finds in this segment is on a different instance.
TEST(DDGILightShadowHitComputeTest, ProxyShadowRaySplitsAtTheBoundingSphereOfTheEmitter)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    DDGICaseKernel kernel;
    std::string error;
    ASSERT_TRUE(kernel.Load(*device, "ddgi_light_shadow_hit_test", error)) << error;

    constexpr float kTMin = 0.001f;
    const auto results = kernel.Run(std::vector<ShadowHitCase>{
        // The ray starts outside the sphere: the split is at the sphere surface.
        ProxyRay(2.0f, 10.0f, kTMin, 9.9f),
        // The ray starts inside the sphere: there is no first segment.
        ProxyRay(2.0f, 1.5f, kTMin, 1.4f),
        // A sphere smaller than the end bias of the ray: there is no second segment.
        ProxyRay(0.05f, 10.0f, kTMin, 9.9f)});
    ASSERT_EQ(results.size(), 3u);
    EXPECT_FLOAT_EQ(results[0][2], 8.0f);
    EXPECT_FLOAT_EQ(results[1][2], kTMin);
    EXPECT_FLOAT_EQ(results[2][2], 9.9f);
}
}  // namespace
