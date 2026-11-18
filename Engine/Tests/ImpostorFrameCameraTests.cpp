#include "Engine/Rendering/Impostors/ImpostorFrameCamera.h"

#include "Mathematics/Geometry.h"
#include "Mathematics/Vector4.h"

#include <gtest/gtest.h>

#include <cmath>

using namespace GameEngine;
using namespace GameEngine::Engine::Renderer;

namespace
{

constexpr float kTolerance = 1e-4f;
constexpr uint32_t kFramesPerSide = 8u;

Mathematics::BoundingBox TreeBounds()
{
    Mathematics::BoundingBox bounds;
    bounds.center = Mathematics::Vector3(-0.8f, 12.5f, -2.4f);
    bounds.halfExtents = Mathematics::Vector3(12.8f, 12.7f, 12.5f);
    return bounds;
}

Mathematics::Vector3 Project(const Rendering::CameraData& camera, const Mathematics::Vector3& point)
{
    const Mathematics::Vector4 clip = camera.viewProj.Transform(Mathematics::Vector4(point.x, point.y, point.z, 1.0f));
    return Mathematics::Vector3(clip.x / clip.w, clip.y / clip.w, clip.z / clip.w);
}

} // namespace

TEST(ImpostorFrameCamera, EveryFrameSeesTheBoundsCenterAtTheMiddleOfTheImage)
{
    const Mathematics::BoundingBox bounds = TreeBounds();
    for (Mathematics::OctahedralLayout layout : {Mathematics::OctahedralLayout::Hemisphere, Mathematics::OctahedralLayout::Sphere})
    {
        for (uint32_t row = 0; row < kFramesPerSide; ++row)
        {
            for (uint32_t column = 0; column < kFramesPerSide; ++column)
            {
                const Rendering::CameraData camera = MakeImpostorFrameCamera(layout, column, row, kFramesPerSide, bounds.center, bounds.Radius());
                const Mathematics::Vector3 center = Project(camera, bounds.center);
                EXPECT_NEAR(center.x, 0.0f, kTolerance);
                EXPECT_NEAR(center.y, 0.0f, kTolerance);
                EXPECT_NEAR(center.z, 0.5f, kTolerance) << "the center sits halfway through the depth range";
            }
        }
    }
}

TEST(ImpostorFrameCamera, TheBoundingSphereFillsTheReverseZDepthRange)
{
    const Mathematics::BoundingBox bounds = TreeBounds();
    const float radius = bounds.Radius();
    const Mathematics::Vector3 direction =
        Mathematics::OctahedralFrameDirection(Mathematics::OctahedralLayout::Hemisphere, 2u, 5u, kFramesPerSide);
    const Rendering::CameraData camera =
        MakeImpostorFrameCamera(Mathematics::OctahedralLayout::Hemisphere, 2u, 5u, kFramesPerSide, bounds.center, radius);
    EXPECT_NEAR(Project(camera, bounds.center + direction * radius).z, 1.0f, kTolerance) << "near side is 1 (reverse-Z)";
    EXPECT_NEAR(Project(camera, bounds.center - direction * radius).z, 0.0f, kTolerance) << "far side is 0";
}

TEST(ImpostorFrameCamera, FrameAxesAreOrthonormalAtThePoleToo)
{
    for (const Mathematics::Vector3 direction : {Mathematics::Vector3(0.0f, 1.0f, 0.0f), Mathematics::Vector3(0.0f, -1.0f, 0.0f),
                                                 Mathematics::Vector3(1.0f, 0.0f, 0.0f), Mathematics::Vector3(0.6f, 0.0f, 0.8f)})
    {
        const ImpostorFrameAxes axes = MakeImpostorFrameAxes(direction);
        EXPECT_NEAR(axes.Right.Length(), 1.0f, kTolerance);
        EXPECT_NEAR(axes.Up.Length(), 1.0f, kTolerance);
        EXPECT_NEAR(Mathematics::Vector3::Dot(axes.Right, axes.Up), 0.0f, kTolerance);
        EXPECT_NEAR(Mathematics::Vector3::Dot(axes.Right, direction), 0.0f, kTolerance);
        EXPECT_NEAR(Mathematics::Vector3::Dot(axes.Up, direction), 0.0f, kTolerance);
    }
}
