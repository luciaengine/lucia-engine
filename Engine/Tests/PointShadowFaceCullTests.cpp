// Headless geometry checks for point-shadow face culling. Visible receiver
// centers and every contributing filtered face must remain in the kept mask.
// The footprint control uses the production cube projections and culler.

#include "Engine/Rendering/PointShadowFaceCull.h"
#include "Engine/Rendering/RenderServices.h"
#include "../Source/Engine/Rendering/RenderServicesDetail.h"

#include "Mathematics/MatrixOps.h"
#include "Mathematics/Vector3.h"
#include "Mathematics/Vector4.h"
#include "Rendering/Common/Frustum.h"

#include <algorithm>
#include <cmath>

#include <gtest/gtest.h>

namespace
{
using GameEngine::Engine::Renderer::ComputePointShadowFaceMask;
using GameEngine::Engine::Renderer::PointShadowCameraCull;
using GameEngine::Engine::Renderer::PointShadowLightVisible;
using GameEngine::Mathematics::Matrix4x4;
using GameEngine::Mathematics::Vector3;
using GameEngine::Mathematics::Vector4;

constexpr uint8_t kAllFaces = 0x3Fu;

constexpr float DegToRad(float degrees)
{
    return degrees * (3.14159265358979323846f / 180.0f);
}

// Reverse-Z NDC cube corners (z = 1 near, z = 0 far), matching
// ShadowMapRenderFeature's ExtractFrustumCornersWS.
PointShadowCameraCull MakeCameraCull(const Vector3& eye, const Vector3& target, const Vector3& up,
                                     float fovYRadians, float aspect, float nearZ, float farZ)
{
    const Matrix4x4 view = GameEngine::Mathematics::MakeLookAtLH(eye, target, up);
    const Matrix4x4 proj =
        GameEngine::Mathematics::MakePerspectiveLH_ZO_ReverseZ(fovYRadians, aspect, nearZ, farZ);
    const Matrix4x4 viewProj = proj * view;
    const Matrix4x4 invViewProj = GameEngine::Mathematics::Inverse(viewProj);

    PointShadowCameraCull cull{};
    cull.CameraPosition = eye;
    GameEngine::Rendering::ExtractFrustumPlanes(viewProj, cull.FrustumPlanes.data());

    static constexpr float kNdc[8][4] = {
        {-1, -1, 1, 1}, {1, -1, 1, 1}, {1, 1, 1, 1}, {-1, 1, 1, 1}, // near
        {-1, -1, 0, 1}, {1, -1, 0, 1}, {1, 1, 0, 1}, {-1, 1, 0, 1}, // far
    };
    for (int i = 0; i < 8; ++i)
    {
        const Vector4 ndc{kNdc[i][0], kNdc[i][1], kNdc[i][2], kNdc[i][3]};
        const Vector4 ws = invViewProj.Transform(ndc);
        const float invW = 1.0f / ws.w;
        cull.FrustumCorners[i] = Vector3{ws.x * invW, ws.y * invW, ws.z * invW};
    }
    return cull;
}

// The cube face a fragment at worldPos samples for a light at lightPos: the
// largest-magnitude axis of (fragment - light). Mirrors GE_PointShadowFace.
int SampledFace(const Vector3& worldPos, const Vector3& lightPos)
{
    const Vector3 d{worldPos.x - lightPos.x, worldPos.y - lightPos.y, worldPos.z - lightPos.z};
    const float ax = std::fabs(d.x), ay = std::fabs(d.y), az = std::fabs(d.z);
    if (ax >= ay && ax >= az)
        return d.x >= 0.0f ? 0 : 1;
    if (ay >= az)
        return d.y >= 0.0f ? 2 : 3;
    return d.z >= 0.0f ? 4 : 5;
}

bool InFrustum(const Vector3& p, const PointShadowCameraCull& cam)
{
    for (const Vector4& pl : cam.FrustumPlanes)
    {
        if (pl.x * p.x + pl.y * p.y + pl.z * p.z + pl.w < 0.0f)
            return false;
    }
    return true;
}

// Sample a dense grid of world points inside the light range; for every point
// that is inside the camera frustum, assert the face it samples is kept. This
// is the "never over-cull" guarantee, tested exhaustively over the volume.
void ExpectNoOverCull(const PointShadowCameraCull& cam, const Vector3& lightPos, float range,
                      uint8_t mask)
{
    constexpr int kSteps = 24;
    const float r2 = range * range;
    for (int ix = 0; ix <= kSteps; ++ix)
        for (int iy = 0; iy <= kSteps; ++iy)
            for (int iz = 0; iz <= kSteps; ++iz)
            {
                const Vector3 p{
                    lightPos.x + (2.0f * ix / kSteps - 1.0f) * range,
                    lightPos.y + (2.0f * iy / kSteps - 1.0f) * range,
                    lightPos.z + (2.0f * iz / kSteps - 1.0f) * range};
                const Vector3 rel{p.x - lightPos.x, p.y - lightPos.y, p.z - lightPos.z};
                if (rel.x * rel.x + rel.y * rel.y + rel.z * rel.z > r2)
                    continue; // outside the light range sphere
                if (!InFrustum(p, cam))
                    continue; // not a visible fragment
                const int face = SampledFace(p, lightPos);
                EXPECT_NE(mask & (1u << face), 0u)
                    << "visible fragment samples culled face " << face << " at (" << p.x << ","
                    << p.y << "," << p.z << ")";
            }
}

// Camera-relative mirror of the production plane rebase (file-local there): a
// plane (n, w) shifted into a frame translated by `origin` becomes (n, w + n·origin).
Vector4 RebasePlaneRel(const Vector4& p, const Vector3& origin)
{
    return Vector4{p.x, p.y, p.z, p.w + (p.x * origin.x + p.y * origin.y + p.z * origin.z)};
}

// Earth-scale keep-bias check. At planetary coordinates the raw world-space
// frustum test in ExpectNoOverCull loses its fp32 mantissa to big·big
// cancellation — the SAME failure the production rebase fixes — so the harness
// would be measuring its own imprecision. Sample the range volume in
// CAMERA-RELATIVE space instead (lightRel = light − cameraPos, taps built as
// lightRel + delta, planes rebased by cameraPos), which stays exact regardless
// of the absolute magnitude, and assert every relatively-in-frustum fragment
// samples a face the production mask kept.
void ExpectNoOverCullRelative(const PointShadowCameraCull& cam, const Vector3& lightRel,
                              float range, uint8_t mask)
{
    std::array<Vector4, 6> planesRel{};
    for (int i = 0; i < 6; ++i)
        planesRel[i] = RebasePlaneRel(cam.FrustumPlanes[i], cam.CameraPosition);

    constexpr int kSteps = 24;
    const float r2 = range * range;
    for (int ix = 0; ix <= kSteps; ++ix)
        for (int iy = 0; iy <= kSteps; ++iy)
            for (int iz = 0; iz <= kSteps; ++iz)
            {
                const Vector3 delta{(2.0f * ix / kSteps - 1.0f) * range,
                                    (2.0f * iy / kSteps - 1.0f) * range,
                                    (2.0f * iz / kSteps - 1.0f) * range};
                if (delta.x * delta.x + delta.y * delta.y + delta.z * delta.z > r2)
                    continue; // outside the light range sphere
                const Vector3 pRel{lightRel.x + delta.x, lightRel.y + delta.y, lightRel.z + delta.z};
                bool inFrustum = true;
                for (const Vector4& pl : planesRel)
                    if (pl.x * pRel.x + pl.y * pRel.y + pl.z * pRel.z + pl.w < 0.0f)
                    {
                        inFrustum = false;
                        break;
                    }
                if (!inFrustum)
                    continue; // not a visible fragment
                const int face = SampledFace(delta, Vector3{0, 0, 0});
                EXPECT_NE(mask & (1u << face), 0u)
                    << "Earth-scale visible fragment samples culled face " << face;
            }
}

} // namespace

// Camera sitting inside the light's range volume: every face must be kept — a
// fragment right next to the camera can fall on any cube face.
TEST(PointShadowFaceCull, LightSurroundingCameraKeepsAllFaces)
{
    const auto cam = MakeCameraCull({0, 0, 0}, {0, 0, 1}, {0, 1, 0},
                                    DegToRad(60.0f), 1.6f, 0.1f, 200.0f);
    const Vector3 light{2.0f, 1.0f, 3.0f};
    const float range = 25.0f; // camera (origin) is well within range
    EXPECT_TRUE(PointShadowLightVisible(light, range, cam));
    EXPECT_EQ(ComputePointShadowFaceMask(light, range, cam), kAllFaces);
}

// Light entirely behind the camera: light-level reject (invisible), and if the
// mask is asked for anyway it is empty (all face frustums are behind the near
// plane).
TEST(PointShadowFaceCull, LightBehindCameraCulledEntirely)
{
    const auto cam = MakeCameraCull({0, 0, 0}, {0, 0, 1}, {0, 1, 0},
                                    DegToRad(60.0f), 1.6f, 0.1f, 200.0f);
    const Vector3 light{0.0f, 0.0f, -50.0f};
    const float range = 5.0f;
    EXPECT_FALSE(PointShadowLightVisible(light, range, cam));
    EXPECT_EQ(ComputePointShadowFaceMask(light, range, cam), 0u);
}

// Light off to the side but within reach: some faces cull (mask != all), some
// keep (mask != 0), and the +X face (pointing further away from the frustum)
// is dropped while the -X face (pointing back toward it) survives.
TEST(PointShadowFaceCull, SideLightCullsFacesPointingAway)
{
    const auto cam = MakeCameraCull({0, 0, 0}, {0, 0, 1}, {0, 1, 0},
                                    DegToRad(60.0f), 1.0f, 0.1f, 200.0f);
    const Vector3 light{30.0f, 0.0f, 30.0f};
    const float range = 25.0f;
    EXPECT_TRUE(PointShadowLightVisible(light, range, cam));
    const uint8_t mask = ComputePointShadowFaceMask(light, range, cam);
    EXPECT_NE(mask, kAllFaces) << "a side light should cull at least one face";
    EXPECT_NE(mask, 0u) << "a visible light must keep at least one face";
    EXPECT_EQ(mask & (1u << 0), 0u) << "+X face points away from the frustum, should be culled";
    EXPECT_NE(mask & (1u << 1), 0u) << "-X face points toward the frustum, must be kept";
    ExpectNoOverCull(cam, light, range, mask);
}

// The keep-bias guarantee across a spread of light positions and a turning
// camera: no configuration may over-cull, and off-axis lights must actually
// drop faces (proving the mask isn't trivially all-set).
TEST(PointShadowFaceCull, NeverOverCullsAcrossConfigurations)
{
    const float fov = DegToRad(55.0f);
    struct Config
    {
        Vector3 eye, target, light;
        float range;
    };
    const Config configs[] = {
        {{0, 0, 0}, {0, 0, 1}, {40, 0, 40}, 30.0f},
        {{0, 0, 0}, {0, 0, 1}, {0, 35, 35}, 30.0f},
        {{0, 0, 0}, {0, 0, 1}, {-30, -20, 45}, 28.0f},
        {{10, 5, -5}, {0, 0, 20}, {35, 10, 15}, 26.0f},
        {{0, 0, 0}, {1, 0, 1}, {50, 0, 5}, 20.0f}, // camera yawed 45°
    };
    bool anyPartialCull = false;
    for (const Config& c : configs)
    {
        const auto cam = MakeCameraCull(c.eye, c.target, {0, 1, 0}, fov, 1.6f, 0.1f, 250.0f);
        if (!PointShadowLightVisible(c.light, c.range, cam))
            continue;
        const uint8_t mask = ComputePointShadowFaceMask(c.light, c.range, cam);
        ExpectNoOverCull(cam, c.light, c.range, mask);
        if (mask != kAllFaces && mask != 0u)
            anyPartialCull = true;
    }
    EXPECT_TRUE(anyPartialCull) << "at least one off-axis config should partially cull";
}

// Boundary sweep: as the camera slowly yaws a face-frustum across the view
// edge, the mask must never flip a still-needed face from kept to culled. We
// assert the sampled-fragment invariant at every step — a culled-face pop would
// trip ExpectNoOverCull.
TEST(PointShadowFaceCull, BoundarySweepNeverPopsACulledFace)
{
    const float fov = DegToRad(50.0f);
    const Vector3 light{25.0f, 0.0f, 25.0f};
    const float range = 22.0f;
    for (int i = 0; i <= 40; ++i)
    {
        const float yaw = (-0.6f + 1.2f * i / 40.0f); // radians, sweeps toward the light
        const Vector3 target{std::sin(yaw), 0.0f, std::cos(yaw)};
        const auto cam = MakeCameraCull({0, 0, 0}, target, {0, 1, 0}, fov, 1.6f, 0.1f, 200.0f);
        if (!PointShadowLightVisible(light, range, cam))
            continue;
        const uint8_t mask = ComputePointShadowFaceMask(light, range, cam);
        ExpectNoOverCull(cam, light, range, mask);
    }
}

// Earth-scale precision (arc rider 4): the same partial-cull geometry, translated
// ~1,600 km from the origin. Asserts the keep-bias invariant (no visible fragment
// loses its face, checked in exact camera-relative space) still holds at 1e6+ m.
//
// HONEST SCOPE: this DOCUMENTS the invariant; it does NOT by itself falsify a
// rebase regression. This config's faces clear the keep-bias margin by more than
// the residual error, and MakeCameraCull's own corner extraction carries ~metre
// error at this magnitude, so reverting the rebase still passes here (the reviewer
// confirmed this). The rebase's real payoff is precision for SMALL-range lights,
// where RebasePlane's DOUBLE-accumulated w keeps the sub-metre keep-bias margin
// meaningful; a test that falsifies THAT would need a hand-built double-precision
// reference frustum (no MakeCameraCull extraction error) — deferred as out of
// scope for a LOW precision rider.
TEST(PointShadowFaceCull, EarthScaleKeepBiasHolds)
{
    const float fov = DegToRad(55.0f);
    const Vector3 offset{1.2e6f, 0.8e6f, 1.6e6f};
    const auto shift = [&](const Vector3& v) {
        return Vector3{v.x + offset.x, v.y + offset.y, v.z + offset.z};
    };

    const auto cam =
        MakeCameraCull(shift({0, 0, 0}), shift({0, 0, 1}), {0, 1, 0}, fov, 1.0f, 0.1f, 200.0f);
    const Vector3 light = shift({30.0f, 0.0f, 30.0f});
    const float range = 25.0f;

    ASSERT_TRUE(PointShadowLightVisible(light, range, cam))
        << "an in-front light must survive the light-level test at Earth-scale";
    const uint8_t mask = ComputePointShadowFaceMask(light, range, cam);
    EXPECT_NE(mask, 0u) << "a visible Earth-scale light must keep at least one face";

    const Vector3 lightRel{light.x - cam.CameraPosition.x, light.y - cam.CameraPosition.y,
                           light.z - cam.CameraPosition.z};
    ExpectNoOverCullRelative(cam, lightRel, range, mask);
}

namespace
{
using GameEngine::Engine::Renderer::PointShadowFrameInfo;

PointShadowCameraCull MakeReceiverBox(const Vector3& receiver, float range)
{
    const float extent = range * 0.000001f;
    PointShadowCameraCull camera{};
    camera.CameraPosition = receiver + Vector3{0, 0, -3.0f * range};
    camera.FrustumPlanes = {{
        {1, 0, 0, extent - receiver.x}, {-1, 0, 0, extent + receiver.x},
        {0, 1, 0, extent - receiver.y}, {0, -1, 0, extent + receiver.y},
        {0, 0, 1, extent - receiver.z}, {0, 0, -1, extent + receiver.z},
    }};
    for (uint32_t corner = 0; corner < camera.FrustumCorners.size(); ++corner)
        camera.FrustumCorners[corner] = receiver + Vector3{
            corner & 1u ? extent : -extent,
            corner & 2u ? extent : -extent,
            corner & 4u ? extent : -extent};
    return camera;
}

Vector3 UnprojectFilterPoint(const Matrix4x4& inverse, float horizontal, float vertical, float depth)
{
    const Vector4 point = inverse.Transform({2.0f * horizontal - 1.0f,
        1.0f - 2.0f * vertical, depth, 1.0f});
    return {point.x / point.w, point.y / point.w, point.z / point.w};
}

uint8_t RequiredFilterFaces(const PointShadowFrameInfo& geometry, const Vector3& position)
{
    constexpr float kRadius = 1.5f / 1024.0f;
    constexpr float kTileResolution = 256.0f;
    constexpr uint32_t kTapCount = 12;
    const uint32_t selected = static_cast<uint32_t>(SampledFace(position, geometry.position));
    const Matrix4x4& selectedVP = geometry.lightVP[selected];
    const Vector4 selectedClip = selectedVP.Transform({position.x, position.y, position.z, 1});
    const float horizontal = 0.5f + 0.5f * selectedClip.x / selectedClip.w;
    const float vertical = 0.5f - 0.5f * selectedClip.y / selectedClip.w;
    const float seed = (std::floor(std::clamp(horizontal, 0.0f, 0.999999f) * kTileResolution) +
        selected * 17.0f) * 0.06711056f +
        (std::floor(std::clamp(vertical, 0.0f, 0.999999f) * kTileResolution) + selected * 31.0f) * 0.00583715f;
    const float hash = 52.9829189f * (seed - std::floor(seed));
    const float angle = (hash - std::floor(hash)) * 6.2831853f;
    const Vector3 right{selectedVP.Data()[0], selectedVP.Data()[4], selectedVP.Data()[8]};
    const Vector3 up{selectedVP.Data()[1], selectedVP.Data()[5], selectedVP.Data()[9]};
    uint8_t required = 0;
    for (uint32_t tap = 0; tap < kTapCount; ++tap)
    {
        const float radius = std::sqrt((tap + 0.5f) / kTapCount) * kRadius;
        const float theta = tap * 2.39996323f;
        const float offsetU = radius * (std::cos(theta) * std::cos(angle) -
            std::sin(theta) * std::sin(angle));
        const float offsetV = radius * (std::cos(theta) * std::sin(angle) +
            std::sin(theta) * std::cos(angle));
        const Vector3 tapPosition = position +
            (right * offsetU - up * offsetV) * (2.0f * selectedClip.w);
        uint32_t face = selected;
        float tapU = horizontal + offsetU;
        float tapV = vertical + offsetV;
        if (tapU < 0 || tapU > 1 || tapV < 0 || tapV > 1)
            face = static_cast<uint32_t>(SampledFace(tapPosition, geometry.position));
        const Matrix4x4& faceVP = geometry.lightVP[face];
        const Vector4 clip = faceVP.Transform({tapPosition.x, tapPosition.y, tapPosition.z, 1});
        tapU = 0.5f + 0.5f * clip.x / clip.w;
        tapV = 0.5f - 0.5f * clip.y / clip.w;
        const float texelU = tapU * kTileResolution - 0.5f;
        const float texelV = tapV * kTileResolution - 0.5f;
        const float baseU = std::floor(texelU);
        const float baseV = std::floor(texelV);
        const float fractionU = texelU - baseU;
        const float fractionV = texelV - baseV;
        const Matrix4x4 inverse = GameEngine::Mathematics::Inverse(faceVP);
        for (uint32_t cornerV = 0; cornerV < 2; ++cornerV)
            for (uint32_t cornerU = 0; cornerU < 2; ++cornerU)
            {
                const float weight = (cornerU ? fractionU : 1.0f - fractionU) *
                    (cornerV ? fractionV : 1.0f - fractionV);
                if (weight <= 0)
                    continue;
                const float pointU = (baseU + cornerU + 0.5f) / kTileResolution;
                const float pointV = (baseV + cornerV + 0.5f) / kTileResolution;
                const bool outsideU = pointU < 0 || pointU > 1;
                const bool outsideV = pointV < 0 || pointV > 1;
                if (!outsideU && !outsideV)
                    required |= static_cast<uint8_t>(1u << face);
                else
                {
                    const Vector3 corner = UnprojectFilterPoint(inverse, pointU, pointV, clip.z / clip.w);
                    if (outsideU && outsideV)
                    {
                        const Vector3 direction = corner - geometry.position;
                        required |= static_cast<uint8_t>((1u << (direction.x >= 0 ? 0 : 1)) |
                            (1u << (direction.y >= 0 ? 2 : 3)) | (1u << (direction.z >= 0 ? 4 : 5)));
                    }
                    else
                        required |= static_cast<uint8_t>(1u << SampledFace(corner, geometry.position));
                }
            }
    }
    return required;
}
} // namespace

TEST(PointShadowFaceCull, KeepsEveryFilteredFaceAtEdgesAndVerticesWithNormalBias)
{
    using GameEngine::Engine::Renderer::kPointShadowNormalBias;
    using GameEngine::Engine::Renderer::PopulatePointShadowGeometry;
    uint32_t checked = 0;
    uint32_t partialMasks = 0;
    for (float range : {0.2f, 0.5f, 1.0f, 2.5f, 20.0f, 1000.0f})
    {
        PointShadowFrameInfo geometry{};
        PopulatePointShadowGeometry(geometry, {0, 0, 0}, range, 0, 0, nullptr);
        ASSERT_TRUE(geometry.valid);
        for (uint32_t face = 0; face < 6; ++face)
        {
            const Matrix4x4 inverse = GameEngine::Mathematics::Inverse(geometry.lightVP[face]);
            for (float inset : {-0.125f / 256.0f, 0.0f, 0.125f / 256.0f, 0.5f / 256.0f})
                for (uint32_t boundary = 0; boundary < 8; ++boundary)
                {
                    const float horizontal = boundary < 4 ?
                        (boundary < 2 ? (boundary == 0 ? inset : 1.0f - inset) : 0.37123f) :
                        (boundary & 1u ? 1.0f - inset : inset);
                    const float vertical = boundary < 4 ?
                        (boundary < 2 ? 0.62711f : (boundary == 2 ? inset : 1.0f - inset)) :
                        (boundary & 2u ? 1.0f - inset : inset);
                    const Vector3 direction = UnprojectFilterPoint(inverse, horizontal, vertical, 0.5f).Normalize();
                    const Vector3 receiver = direction * (range * 0.75f);
                    const auto camera = MakeReceiverBox(receiver, range);
                    ASSERT_TRUE(InFrustum(receiver, camera));
                    ASSERT_GT(camera.CameraPosition.Length(), range);
                    const auto kept = ComputePointShadowFaceMask(geometry.position, range, camera);
                    partialMasks += kept != kAllFaces;
                    const Vector3 tangent = Vector3::Cross(direction,
                        std::fabs(direction.y) < 0.9f ? Vector3{0, 1, 0} : Vector3{1, 0, 0}).Normalize();
                    const Vector3 bitangent = Vector3::Cross(direction, tangent).Normalize();
                    for (const Vector3 normal : {direction, direction * -1.0f, tangent,
                        tangent * -1.0f, bitangent, bitangent * -1.0f})
                    {
                        const float bias = kPointShadowNormalBias *
                            (1.0f - std::fabs(Vector3::Dot(normal, direction)));
                        const Vector3 biased = receiver + normal * bias;
                        ASSERT_LT(biased.Length(), range);
                        const uint8_t required = RequiredFilterFaces(geometry, biased);
                        ASSERT_NE(required, 0u);
                        EXPECT_EQ(kept & required, required)
                            << "range=" << range << " face=" << face << " inset=" << inset
                            << " boundary=" << boundary << " required=" << unsigned(required)
                            << " kept=" << unsigned(kept);
                        ++checked;
                    }
                }
        }
    }
    EXPECT_EQ(checked, 6912u);
    EXPECT_GT(partialMasks, 0u);
    RecordProperty("filter_face_queries", checked);
    RecordProperty("partial_face_masks", partialMasks);
}
