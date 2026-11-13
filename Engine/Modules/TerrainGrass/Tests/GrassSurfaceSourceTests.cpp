// The ground decisions grass placement and shading share, and the surface level the placement reads. The
// tests run the shipped GLSL blocks of grass_surface_pages.glsl and cbt_surface_pages.glsl, extracted at
// build time and compiled through GlslShim.h.

#include <gtest/gtest.h>

#include "GlslShim.h"
#include "Mathematics/Angle.h"
#include "Terrain/TerrainTypes.h"

#include <cmath>

namespace
{
using GameEngine::GlslShim::uint;
using GameEngine::GlslShim::vec2;
using GameEngine::GlslShim::vec3;
using GameEngine::GlslShim::vec4;

namespace Shader
{
// Using-declarations, not a using-directive: they hide <cmath>'s global overloads inside this namespace.
using GameEngine::GlslShim::abs;
using GameEngine::GlslShim::log2;
using GameEngine::GlslShim::max;
using GameEngine::GlslShim::min;
using GameEngine::GlslShim::sqrt;
using GameEngine::GlslShim::uint;
using GameEngine::GlslShim::vec3;
using GameEngine::GlslShim::length;
using GameEngine::GlslShim::vec2;
using GameEngine::GlslShim::vec4;
#include "CbtSurfacePageLevelExtracted.h"
#include "GrassGroundSourceExtracted.h"
} // namespace Shader

constexpr uint kAtlasFlags = Shader::kTerrainFlagAtlasBacked;
constexpr uint kPlanarFlags = 0u;
constexpr uint kUnbound = 0u;
constexpr uint kBoundSplat = 19u;

// A camera at `Eye` pitched down by `PitchDegrees`, yawed by `YawDegrees` about +Y (left-handed, +Z
// forward), over a viewport `Width` x `Height` pixels: a pinhole with a vertical field of `FovYDegrees`,
// or an orthographic view `OrthoHeight` meters tall. The ground is the plane y = Slope.x x + Slope.y z.
struct Camera
{
    vec3 Eye{0.0f, 30.0f, 0.0f};
    float PitchDegrees = 20.0f;
    float YawDegrees = 30.0f;
    float FovYDegrees = 60.0f;
    float Width = 1920.0f;
    float Height = 1080.0f;
    float OrthoHeight = 0.0f; // > 0: orthographic
    vec2 Slope{0.0f, 0.0f};

    bool Orthographic() const { return OrthoHeight > 0.0f; }
    float ProjY() const
    {
        return Orthographic() ? 2.0f / OrthoHeight
                              : 1.0f / std::tan(GameEngine::Mathematics::ToRadians(FovYDegrees * 0.5f));
    }
    vec3 Forward() const
    {
        const float pitch = GameEngine::Mathematics::ToRadians(PitchDegrees);
        const float yaw = GameEngine::Mathematics::ToRadians(YawDegrees);
        return {std::cos(pitch) * std::sin(yaw), -std::sin(pitch), std::cos(pitch) * std::cos(yaw)};
    }
    vec3 Right() const
    {
        const float yaw = GameEngine::Mathematics::ToRadians(YawDegrees);
        return {std::cos(yaw), 0.0f, -std::sin(yaw)};
    }
    vec3 Up() const
    {
        const vec3 f = Forward();
        const vec3 r = Right();
        return {f.y * r.z - f.z * r.y, f.z * r.x - f.x * r.z, f.x * r.y - f.y * r.x};
    }
    // World vector `v` in view space: (right, up, forward) components.
    vec3 ToView(vec3 v) const { return {dot(Right(), v), dot(Up(), v), dot(Forward(), v)}; }
    // The point on the ground the centre of pixel (px, py) sees.
    vec3 GroundAt(float px, float py) const
    {
        const float ndcX = (px + 0.5f) / Width * 2.0f - 1.0f;
        const float ndcY = 1.0f - (py + 0.5f) / Height * 2.0f;
        const float projX = ProjY() * Height / Width;
        const vec3 r = Right(), u = Up(), f = Forward();
        const float dx = ndcX / projX, dy = ndcY / ProjY();
        const vec3 offset{r.x * dx + u.x * dy, r.y * dx + u.y * dy, r.z * dx + u.z * dy};
        const vec3 origin = Orthographic() ? Eye + offset : Eye;
        const vec3 dir = Orthographic() ? f : f + offset;
        const float t = (Slope.x * origin.x + Slope.y * origin.z - origin.y) /
                        (dir.y - Slope.x * dir.x - Slope.y * dir.z);
        return origin + dir * t;
    }
};

// length(fwidth(position)) of the ground fragment at pixel (px, py): the one-pixel differences along
// screen x and screen y, summed in size per component.
float RasterFootprint(const Camera& camera, float px, float py)
{
    const vec3 p = camera.GroundAt(px, py);
    const vec3 px1 = camera.GroundAt(px + 1.0f, py);
    const vec3 py1 = camera.GroundAt(px, py + 1.0f);
    const float wx = std::fabs(px1.x - p.x) + std::fabs(py1.x - p.x);
    const float wy = std::fabs(px1.y - p.y) + std::fabs(py1.y - p.y);
    const float wz = std::fabs(px1.z - p.z) + std::fabs(py1.z - p.z);
    return std::sqrt(wx * wx + wy * wy + wz * wz);
}

float GrassFootprint(const Camera& camera, vec3 groundWS)
{
    const vec3 eyeToGround{groundWS.x - camera.Eye.x, groundWS.y - camera.Eye.y, groundWS.z - camera.Eye.z};
    return Shader::GrassSurface_GroundPixelFootprint(
        camera.ToView(eyeToGround), camera.ToView({1.0f, 0.0f, 0.0f}), camera.ToView({0.0f, 1.0f, 0.0f}),
        camera.ToView({0.0f, 0.0f, 1.0f}), camera.Slope, camera.ProjY(),
        camera.Orthographic() ? -camera.Height : camera.Height);
}

// The placement's footprint from the slopes of the four one-sided ground planes at `groundWS` (-X, +X, -Z, +Z).
float GrassFootprintAcrossFaces(const Camera& camera, vec3 groundWS, vec4 faceSlopes)
{
    const vec3 eyeToGround{groundWS.x - camera.Eye.x, groundWS.y - camera.Eye.y, groundWS.z - camera.Eye.z};
    return Shader::GrassSurface_GroundPixelFootprintAcrossFaces(
        camera.ToView(eyeToGround), camera.ToView({1.0f, 0.0f, 0.0f}), camera.ToView({0.0f, 1.0f, 0.0f}),
        camera.ToView({0.0f, 0.0f, 1.0f}), faceSlopes, camera.ProjY(),
        camera.Orthographic() ? -camera.Height : camera.Height);
}

} // namespace

TEST(GrassGroundSource, APageThatAnswersWinsOverTheUnifiedSplatmap)
{
    EXPECT_EQ(Shader::GrassSurface_SelectSource(true, kBoundSplat), Shader::kGrassGroundPages);
    EXPECT_EQ(Shader::GrassSurface_SelectSource(true, kUnbound), Shader::kGrassGroundPages);
}

TEST(GrassGroundSource, WithNoPageTheUnifiedSplatmapAnswersAsOnTheGround)
{
    EXPECT_EQ(Shader::GrassSurface_SelectSource(false, kBoundSplat), Shader::kGrassGroundUnified);
    EXPECT_EQ(Shader::GrassSurface_SelectSource(false, kUnbound), Shader::kGrassGroundNone);
}

TEST(GrassGroundSource, AnAtlasTerrainOffItsPagesIsNotReadable)
{
    EXPECT_FALSE(Shader::GrassSurface_GroundReadable(kAtlasFlags, false));
    EXPECT_TRUE(Shader::GrassSurface_GroundReadable(kAtlasFlags, true));
    EXPECT_TRUE(Shader::GrassSurface_GroundReadable(kPlanarFlags, false));
    // The bit is tested as a mask: another flag beside it does not change the answer.
    constexpr uint kReceiveShadowsBit = 1u;
    EXPECT_FALSE(Shader::GrassSurface_GroundReadable(kAtlasFlags | kReceiveShadowsBit, false));
    EXPECT_TRUE(Shader::GrassSurface_GroundReadable(kReceiveShadowsBit, false));
}

TEST(GrassGroundSource, TheAtlasFlagMirrorsTheEngineConstant)
{
    EXPECT_EQ(Shader::kTerrainFlagAtlasBacked, GameEngine::Terrain::kTerrainFlagAtlasBacked);
}

TEST(GrassGroundFootprint, MatchesTheFragmentsDerivativeFootprintOnTheGround)
{
    Camera camera;
    for (const float py : {640.0f, 760.0f, 900.0f, 1050.0f})
        for (const float px : {100.0f, 960.0f, 1800.0f})
        {
            const float raster = RasterFootprint(camera, px, py);
            const float grass = GrassFootprint(camera, camera.GroundAt(px, py));
            EXPECT_NEAR(grass, raster, raster * 0.03f) << "pixel (" << px << ", " << py << ")";
        }
}

// The fragment's derivatives see the slope: on ground of 10 to 30 degrees facing toward and away from the
// camera, the footprint keeps its Y term.
TEST(GrassGroundFootprint, MatchesTheFragmentsDerivativeFootprintOnSlopes)
{
    for (const float degrees : {10.0f, 20.0f, 30.0f})
        for (const float facing : {-1.0f, 1.0f})
        {
            Camera camera;
            camera.Eye = {0.0f, 60.0f, 0.0f};
            camera.PitchDegrees = 30.0f;
            camera.YawDegrees = 0.0f;
            camera.Slope = {0.0f, facing * std::tan(GameEngine::Mathematics::ToRadians(degrees))};
            for (const float py : {640.0f, 900.0f})
                for (const float px : {100.0f, 960.0f, 1800.0f})
                {
                    const float raster = RasterFootprint(camera, px, py);
                    const float grass = GrassFootprint(camera, camera.GroundAt(px, py));
                    EXPECT_NEAR(grass, raster, raster * 0.03f)
                        << degrees << " degrees, facing " << facing << ", pixel (" << px << ", " << py << ")";
                }
        }
}

// A top-down orthographic view (a strategy camera) reads the projected pixel size, not level 0.
TEST(GrassGroundFootprint, AnOrthographicViewReadsItsPixelSize)
{
    Camera camera;
    camera.Eye = {0.0f, 500.0f, 0.0f};
    camera.PitchDegrees = 60.0f;
    camera.OrthoHeight = 400.0f;
    for (const float py : {100.0f, 540.0f, 1000.0f})
    {
        const float raster = RasterFootprint(camera, 960.0f, py);
        const float grass = GrassFootprint(camera, camera.GroundAt(960.0f, py));
        ASSERT_GT(raster, 0.0f);
        EXPECT_NEAR(grass, raster, raster * 0.03f) << "row " << py;
    }
}

TEST(GrassGroundFootprint, AnUnknownViewportAsksForTheFinestLevel)
{
    Camera camera;
    camera.Height = 0.0f;
    EXPECT_EQ(GrassFootprint(camera, {0.0f, 0.0f, 50.0f}), 0.0f);
}

// The placement reads the surface pages at the level the terrain fragment under the blade reads them:
// red when the placement reads level 0 while the ground reads a blended coarser level.
TEST(GrassGroundFootprint, ThePlacementReadsTheSurfaceLevelTheGroundReads)
{
    Camera camera;
    // A 1,024 m terrain of 4,097 samples a side: 0.25 m spacing, so the far ground reads past level 1.
    constexpr float kWorldSize = 1024.0f;
    constexpr uint kSamples = 4097u;
    bool sawBlendedLevel = false;
    for (float py = 230.0f; py < 1080.0f; py += 6.0f)
    {
        const vec3 ground = camera.GroundAt(960.0f, py);
        const float fragment = Shader::CBT_SurfPageLevel(RasterFootprint(camera, 960.0f, py), kWorldSize,
                                                         kWorldSize, kSamples, kSamples);
        const float grass = Shader::CBT_SurfPageLevel(GrassFootprint(camera, ground), kWorldSize, kWorldSize,
                                                      kSamples, kSamples);
        EXPECT_NEAR(grass, fragment, 0.05f) << "row " << py;
        sawBlendedLevel = sawBlendedLevel || (fragment > 1.0f && std::fmod(fragment, 1.0f) > 0.75f);
    }
    EXPECT_TRUE(sawBlendedLevel) << "no sample reached a level's blend quarter: the test does not reach "
                                    "the case it guards";
}

// A ridge along X, its crest half a height sample behind the root: the far face falls 10 degrees away from
// the camera, and the fragment's triangle across the crest is flat. A tap along the far face reads a coarser
// level. Red when the placement reads a coarser level than the ground.
TEST(GrassGroundFootprint, AtACrestThePlacementReadsNoCoarserALevelThanTheGround)
{
    constexpr float kWorldSize = 1024.0f;
    constexpr uint kSamples = 8193u;
    constexpr float kPitchDegrees = 20.0f;
    const float fall = std::tan(GameEngine::Mathematics::ToRadians(10.0f));
    Camera crest; // the triangle across the crest: the plane y = 0 through the root at the origin
    crest.PitchDegrees = kPitchDegrees;
    crest.YawDegrees = 0.0f;
    crest.Eye = {0.0f, 30.0f, -30.0f / std::tan(GameEngine::Mathematics::ToRadians(kPitchDegrees))};
    Camera farFace = crest;
    farFace.Slope = {0.0f, -fall};
    const vec3 root = farFace.GroundAt(959.5f, 539.5f);

    const float fragment = Shader::CBT_SurfPageLevel(RasterFootprint(crest, 959.5f, 539.5f), kWorldSize, kWorldSize,
                                                     kSamples, kSamples);
    const float alongFarFace = Shader::CBT_SurfPageLevel(GrassFootprint(farFace, root), kWorldSize, kWorldSize,
                                                         kSamples, kSamples);
    ASSERT_GT(alongFarFace - fragment, 0.5f) << "the far face does not separate the levels: the test does not reach "
                                                "the case it guards";
    // -X and +X flat, -Z across the crest flat, +Z along the far face.
    const vec4 faceSlopes{0.0f, 0.0f, 0.0f, -fall};
    const float grass = Shader::CBT_SurfPageLevel(GrassFootprintAcrossFaces(farFace, root, faceSlopes), kWorldSize,
                                                  kWorldSize, kSamples, kSamples);
    EXPECT_LE(grass, fragment + 0.05f);

    // Ground with no bend reads the plane's own footprint.
    const vec4 smooth{0.0f, 0.0f, -fall, -fall};
    EXPECT_NEAR(GrassFootprintAcrossFaces(farFace, root, smooth), GrassFootprint(farFace, root),
                GrassFootprint(farFace, root) * 1e-4f);
}
