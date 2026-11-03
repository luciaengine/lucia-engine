#include "Engine/Rendering/Camera.h"
#include "Engine/Rendering/CameraAspectRatio.h"
#include "Engine/Rendering/RenderServices.h"
#include "Rendering/CameraPixelGrid.h"
#include <gtest/gtest.h>
#include <array>
#include <cmath>
#include <limits>

using namespace GameEngine;
using namespace GameEngine::Rendering;
using namespace GameEngine::Engine::Renderer;

TEST(PixelPerfectCamera, OrthoDisplayGridTracksCompensatedRasterAcrossPanning)
{
    CameraData camera{};
    for (int i = 0; i < 16; i += 5) camera.view[i] = camera.proj[i] = 1;
    camera.proj[0] = camera.proj[5] = 0.02f;
    constexpr uint32_t width = 161, height = 91;
    for (float pan : {-1.25f, -0.35f, 0.0f, 0.25f, 0.4f, 1.75f})
    {
        camera.view[12] = pan;
        camera.view[13] = -pan;
        std::array<float, 4> uv{};
        const auto snapped = StabilizeCameraToPixelGrid(camera, width, height, 10, uv);
        const std::array<float, 4> grid{1.0f / width, 1.0f / height, -uv[2] / uv[0], -uv[3] / uv[1]};
        for (int axis = 0; axis < 2; ++axis)
        {
            const float size = float(axis == 0 ? width : height);
            // The texel is a sum of order-one float terms scaled by (size + 2), so its rounding error is a
            // few float epsilons of that scale (at most 1.6 measured at 161 px, constant across widths).
            const float tolerance = 4.0f * std::numeric_limits<float>::epsilon() * (size + 2);
            for (float screen : {0.0f, 0.001f, 0.37f, 0.999f, 1.0f})
            {
                const float cell = std::floor((screen - grid[axis + 2]) / grid[axis]);
                const float center = (cell + 0.5f) * grid[axis] + grid[axis + 2];
                const float texel = (center * uv[axis] + 0.5f * (1 - uv[axis]) + uv[axis + 2]) * (size + 2);
                EXPECT_NEAR(texel, cell + 1.5f, tolerance);
            }
        }
        const float rasterU = snapped.viewProj[12] * 0.5f + 0.5f;
        const float displayU = (rasterU - 0.5f * (1 - uv[0]) - uv[2]) / uv[0];
        const float cell = std::floor((displayU - grid[2]) / grid[0]);
        const float displayCenter = (cell + 0.5f) * grid[0] + grid[2];
        const float worldSample = ((displayCenter - 0.5f) * 2.0f) / camera.proj[0] - camera.view[12];
        EXPECT_NEAR(worldSample, 0.0f, 2e-5f);
    }
}

TEST(PixelPerfectCamera, ActiveCameraSnapsInScreenAxesAndPreservesTransform)
{
    RenderServices services;
    const auto cameraId = services.Views().AllocateCamera("PixelPerfect.Test");
    const auto viewId = services.Views().AllocateView("PixelPerfect.Test", cameraId);
    Engine::Renderer::Camera camera;
    camera.params.Perspective = false;
    camera.params.PixelPerfect = true;
    camera.params.PixelPerfectPixelSnap = true;
    camera.params.PixelPerfectPixelsPerUnit = 10;
    camera.params.PixelPerfectReferenceWidth = 161;
    camera.params.PixelPerfectReferenceHeight = 91;
    for (float angle : {0.0f, 0.7f, 1.5707963f})
    {
        auto* matrix = camera.worldTransform.Data();
        matrix[0] = matrix[5] = std::cos(angle);
        matrix[1] = std::sin(angle);
        matrix[4] = -std::sin(angle);
        for (float pan : {-0.17f, -0.04f, 0.0f, 0.04f, 0.17f})
        {
            matrix[12] = pan;
            matrix[13] = -pan;
            ApplyActiveCameraAspect(services, viewId, cameraId, camera, 483, 273);
            const auto* actual = services.Views().FindCameraData(cameraId);
            ASSERT_NE(actual, nullptr);
            const auto state = services.Views().GetViewPixelPerfect(viewId);
            ASSERT_TRUE(state.Active);
            const auto view = camera.ComputeViewMatrix();
            for (int axis = 0; axis < 2; ++axis)
            {
                const float original = view.Data()[12 + axis];
                const float snapped = std::round(original * 10.0f) / 10.0f;
                EXPECT_NEAR(actual->viewProj[12 + axis], snapped * actual->proj[axis * 5], 1e-5f);
                const float fraction = axis == 0 ? state.FracX : state.FracY;
                EXPECT_NEAR(fraction, (snapped - original) * 10.0f, 1e-5f);
            }
            EXPECT_FLOAT_EQ(matrix[12], pan);
            EXPECT_FLOAT_EQ(matrix[13], -pan);
        }
    }
}

TEST(PixelPerfectCamera, DisabledPixelSnapPreservesProjectionAndZeroCompensation)
{
    RenderServices services;
    const auto cameraId = services.Views().AllocateCamera("PixelPerfect.Disabled");
    const auto viewId = services.Views().AllocateView("PixelPerfect.Disabled", cameraId);
    Engine::Renderer::Camera camera;
    camera.params.Perspective = false;
    camera.params.PixelPerfect = true;
    camera.params.PixelPerfectPixelSnap = false;
    camera.params.PixelPerfectPixelsPerUnit = 10;
    camera.worldTransform.Data()[12] = 0.037f;
    ApplyActiveCameraAspect(services, viewId, cameraId, camera, 640, 360);
    const auto state = services.Views().GetViewPixelPerfect(viewId);
    const auto expected = camera.ToCameraData(float(state.PaddedWidth) / state.PaddedHeight,
                                               float(state.PaddedHeight) / 10.0f);
    const auto* actual = services.Views().FindCameraData(cameraId);
    ASSERT_NE(actual, nullptr);
    EXPECT_FLOAT_EQ(state.FracX, 0.0f);
    EXPECT_FLOAT_EQ(state.FracY, 0.0f);
    for (int element = 0; element < 16; ++element)
        EXPECT_FLOAT_EQ(actual->viewProj[element], expected.viewProj[element]);
}

namespace
{
Components::Camera PixelPerfectCamera(uint32 refWidth, uint32 refHeight)
{
    Components::Camera camera{};
    camera.Perspective = false;
    camera.PixelPerfect = true;
    camera.PixelPerfectReferenceWidth = refWidth;
    camera.PixelPerfectReferenceHeight = refHeight;
    return camera;
}
}

TEST(PixelPerfectCameraViewState, InactiveWithoutTheOptIn)
{
    // Zero regression for existing content: a camera that never opted in must not
    // acquire an offscreen RT or an upscale pass.
    Components::Camera plain{};
    plain.Perspective = false;
    EXPECT_FALSE(ComputePixelPerfectViewState(plain, 1920, 1080).Active);

    Components::Camera perspective = PixelPerfectCamera(320, 180);
    perspective.Perspective = true;
    EXPECT_FALSE(ComputePixelPerfectViewState(perspective, 1920, 1080).Active);
}

TEST(PixelPerfectCameraViewState, PaddedExtentIsTheReferencePlusOneTexelPerSide)
{
    // The border is load-bearing, not decoration: it is what keeps a shifted
    // sample window off clamp-to-edge.
    const PixelPerfectViewState state = ComputePixelPerfectViewState(PixelPerfectCamera(320, 180), 1920, 1080);
    ASSERT_TRUE(state.Active);
    EXPECT_EQ(state.ReferenceWidth, 320u);
    EXPECT_EQ(state.ReferenceHeight, 180u);
    EXPECT_EQ(state.PaddedWidth, state.ReferenceWidth + 2u);
    EXPECT_EQ(state.PaddedHeight, state.ReferenceHeight + 2u);
}

TEST(PixelPerfectCameraViewState, DisplayExtentIsTheViewportItWasComputedAgainst)
{
    // The pipeline reads the crossing's OUTPUT extent from here — the padded RT
    // is the caller's target size, so the panel can only reach the pipeline
    // through this state.
    const PixelPerfectViewState state =
        ComputePixelPerfectViewState(PixelPerfectCamera(320, 180), 1288, 728);
    ASSERT_TRUE(state.Active);
    EXPECT_EQ(state.DisplayWidth, 1288u);
    EXPECT_EQ(state.DisplayHeight, 728u);

    // Inactive state carries no extent, so a non-pixel-perfect view keeps its
    // caller extent as the output basis.
    Components::Camera plain{};
    plain.Perspective = false;
    const PixelPerfectViewState inactive = ComputePixelPerfectViewState(plain, 1288, 728);
    EXPECT_FALSE(inactive.Active);
    EXPECT_EQ(inactive.DisplayWidth, 0u);
    EXPECT_EQ(inactive.DisplayHeight, 0u);
}

TEST(PixelPerfectCameraViewState, ZoomIsTheLargestIntegerThatFits)
{
    struct Case { uint32 screenW, screenH, expectedZoom; };
    // 320x180 reference: 1920x1080 is exactly 6x; 1900x1070 must floor to 5x
    // rather than filter, and a screen smaller than the reference clamps to 1x.
    for (const Case& c : {Case{1920, 1080, 6u}, Case{1900, 1070, 5u}, Case{640, 360, 2u},
                          Case{320, 180, 1u}, Case{200, 100, 1u}})
    {
        const PixelPerfectViewState state =
            ComputePixelPerfectViewState(PixelPerfectCamera(320, 180), c.screenW, c.screenH);
        ASSERT_TRUE(state.Active);
        EXPECT_EQ(state.Zoom, c.expectedZoom) << c.screenW << "x" << c.screenH;
    }
}

TEST(PixelPerfectCameraViewState, LetterboxKeepsExactlyTheReferenceWindow)
{
    // Default policy: show the reference resolution and black-bar the surplus.
    Components::Camera camera = PixelPerfectCamera(320, 180);
    camera.PixelPerfectExpand = false;
    const PixelPerfectViewState state = ComputePixelPerfectViewState(camera, 1900, 1070);
    ASSERT_TRUE(state.Active);
    EXPECT_EQ(state.ReferenceWidth, 320u);
    EXPECT_EQ(state.ReferenceHeight, 180u);
}

TEST(PixelPerfectCameraViewState, ExpandGrowsTheWindowToCoverTheScreen)
{
    // Expand reveals more world instead of bars, but the zoom is unchanged, so a
    // sprite stays the same size on screen in either crop mode.
    Components::Camera letterboxed = PixelPerfectCamera(320, 180);
    Components::Camera expanded = letterboxed;
    expanded.PixelPerfectExpand = true;

    const PixelPerfectViewState boxed = ComputePixelPerfectViewState(letterboxed, 1900, 1070);
    const PixelPerfectViewState grown = ComputePixelPerfectViewState(expanded, 1900, 1070);
    ASSERT_TRUE(grown.Active);
    EXPECT_EQ(grown.Zoom, boxed.Zoom);
    EXPECT_GE(grown.ReferenceWidth, boxed.ReferenceWidth);
    EXPECT_GE(grown.ReferenceHeight, boxed.ReferenceHeight);
    // ceil(screen / zoom) covers the screen, so nothing is left unpainted.
    EXPECT_GE(grown.ReferenceWidth * grown.Zoom, 1900u);
    EXPECT_GE(grown.ReferenceHeight * grown.Zoom, 1070u);
    EXPECT_EQ(grown.PaddedWidth, grown.ReferenceWidth + 2u);
}

TEST(PixelPerfectCameraViewState, ExpandNeverShrinksBelowTheReference)
{
    Components::Camera camera = PixelPerfectCamera(320, 180);
    camera.PixelPerfectExpand = true;
    const PixelPerfectViewState state = ComputePixelPerfectViewState(camera, 200, 100);
    ASSERT_TRUE(state.Active);
    EXPECT_GE(state.ReferenceWidth, 320u);
    EXPECT_GE(state.ReferenceHeight, 180u);
}
