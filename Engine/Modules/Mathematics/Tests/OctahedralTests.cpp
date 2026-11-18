#include "Mathematics/Octahedral.h"

#include <gtest/gtest.h>

#include <cmath>

using namespace GameEngine::Mathematics;

namespace
{

constexpr float kTolerance = 1e-4f;

void ExpectDirection(const Vector3& expected, const Vector3& actual)
{
    EXPECT_NEAR(actual.x, expected.x, kTolerance);
    EXPECT_NEAR(actual.y, expected.y, kTolerance);
    EXPECT_NEAR(actual.z, expected.z, kTolerance);
}

// Directions over the whole sphere: a latitude-longitude grid, poles included.
template <typename Fn>
void ForEachSphereDirection(Fn&& fn)
{
    constexpr int kSteps = 24;
    constexpr float kPi = 3.14159265358979f;
    for (int latitude = 0; latitude <= kSteps; ++latitude)
    {
        for (int longitude = 0; longitude < kSteps; ++longitude)
        {
            const float theta = kPi * static_cast<float>(latitude) / kSteps;
            const float phi = 2.0f * kPi * static_cast<float>(longitude) / kSteps;
            fn(Vector3(std::sin(theta) * std::cos(phi), std::cos(theta), std::sin(theta) * std::sin(phi)));
        }
    }
}

} // namespace

TEST(OctahedralTests, SphereEncodeAndDecodeRoundTripOverTheWholeSphere)
{
    ForEachSphereDirection([](const Vector3& direction)
    {
        const Vector2 uv = OctahedralEncode(direction);
        EXPECT_GE(uv.x, 0.0f);
        EXPECT_LE(uv.x, 1.0f);
        EXPECT_GE(uv.y, 0.0f);
        EXPECT_LE(uv.y, 1.0f);
        ExpectDirection(direction, OctahedralDecode(uv));
    });
}

TEST(OctahedralTests, SphereMapPutsPlusZAtTheCenterAsTheShaderDoes)
{
    const Vector2 center = OctahedralEncode(Vector3(0.0f, 0.0f, 1.0f));
    EXPECT_NEAR(center.x, 0.5f, kTolerance);
    EXPECT_NEAR(center.y, 0.5f, kTolerance);
    // GE_OctEncode folds -Z to the corners.
    const Vector2 corner = OctahedralEncode(Vector3(0.0f, 0.0f, -1.0f));
    EXPECT_NEAR(std::abs(corner.x - 0.5f), 0.5f, kTolerance);
    EXPECT_NEAR(std::abs(corner.y - 0.5f), 0.5f, kTolerance);
}

TEST(OctahedralTests, HemisphereRoundTripsAboveTheHorizonWithPlusYAtTheCenter)
{
    ForEachSphereDirection([](const Vector3& direction)
    {
        if (direction.y < 0.0f)
            return;
        const Vector2 uv = HemiOctahedralEncode(direction);
        EXPECT_GE(uv.x, -kTolerance);
        EXPECT_LE(uv.x, 1.0f + kTolerance);
        EXPECT_GE(uv.y, -kTolerance);
        EXPECT_LE(uv.y, 1.0f + kTolerance);
        ExpectDirection(direction, HemiOctahedralDecode(uv));
    });
    const Vector2 up = HemiOctahedralEncode(Vector3(0.0f, 1.0f, 0.0f));
    EXPECT_NEAR(up.x, 0.5f, kTolerance);
    EXPECT_NEAR(up.y, 0.5f, kTolerance);
}

TEST(OctahedralTests, HemisphereMapsTheHorizonToTheSquareEdge)
{
    const Vector2 uv = HemiOctahedralEncode(Vector3(1.0f, 0.0f, 0.0f));
    const float edgeDistance = std::min({uv.x, uv.y, 1.0f - uv.x, 1.0f - uv.y});
    EXPECT_NEAR(edgeDistance, 0.0f, kTolerance);
    // A view from below the horizon clamps to the horizon point above it.
    ExpectDirection(Vector3(1.0f, 0.0f, 0.0f), HemiOctahedralDecode(HemiOctahedralEncode(Vector3(1.0f, -0.5f, 0.0f).Normalize())));
}

TEST(OctahedralTests, TheOuterHemisphereFramesLookFromTheHorizon)
{
    constexpr uint32_t kFramesPerSide = 8u;
    for (uint32_t index = 0; index < kFramesPerSide; ++index)
    {
        EXPECT_NEAR(OctahedralFrameDirection(OctahedralLayout::Hemisphere, index, 0u, kFramesPerSide).y, 0.0f, kTolerance);
        EXPECT_NEAR(OctahedralFrameDirection(OctahedralLayout::Hemisphere, 0u, index, kFramesPerSide).y, 0.0f, kTolerance);
    }
}

TEST(OctahedralTests, HemisphereEncodesStraightDownAsAHorizonPointWithoutDividingByZero)
{
    const Vector2 uv = HemiOctahedralEncode(Vector3(0.0f, -1.0f, 0.0f));
    EXPECT_TRUE(std::isfinite(uv.x) && std::isfinite(uv.y));
    ExpectDirection(Vector3(1.0f, 0.0f, 0.0f), HemiOctahedralDecode(uv));
}

TEST(OctahedralTests, FrameDirectionIsTheDecodedGridPoint)
{
    constexpr uint32_t kFramesPerSide = 8u;
    for (OctahedralLayout layout : {OctahedralLayout::Hemisphere, OctahedralLayout::Sphere})
    {
        for (uint32_t row = 0; row < kFramesPerSide; ++row)
        {
            for (uint32_t column = 0; column < kFramesPerSide; ++column)
            {
                const Vector3 direction = OctahedralFrameDirection(layout, column, row, kFramesPerSide);
                EXPECT_NEAR(direction.Length(), 1.0f, kTolerance);
                // The direction survives a round trip in both layouts. On the full sphere the corners
                // and the folded edges have two codes each, so only the hemisphere maps back to the
                // same grid point.
                ExpectDirection(direction, OctahedralDecode(layout, OctahedralEncode(layout, direction)));
                if (layout != OctahedralLayout::Hemisphere)
                    continue;
                const Vector2 uv = OctahedralEncode(layout, direction);
                EXPECT_EQ(static_cast<uint32_t>(std::lround(uv.x * (kFramesPerSide - 1))), column);
                EXPECT_EQ(static_cast<uint32_t>(std::lround(uv.y * (kFramesPerSide - 1))), row);
            }
        }
    }
}
