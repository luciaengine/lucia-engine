#include "Components/Rendering/PlanarReflection.h"
#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "ECSModules/Rendering/Systems/PlanarReflectionSystem.h"
#include "Engine/Rendering/PlanarReflectionFeature.h"
#include "Engine/Rendering/PlanarReflectionMath.h"
#include "Engine/Rendering/IEnvironmentSource.h"
#include "Engine/Rendering/ImageBasedLightingFeature.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/RenderOrigin.h"
#include "TestCameraEye.h"
#include "Engine/Rendering/ShadowMapRenderFeature.h"
#include "Engine/Rendering/Pipeline/Nodes/EngineNodeTypes.h"
#include "Engine/Rendering/Pipeline/Nodes/PlanarReflectionNode.h"
#include "Engine/Rendering/Pipeline/RenderPipelineNodes.h"
#include <gtest/gtest.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <cstring>
#include <limits>
#include <filesystem>
#include <fstream>
#include <iterator>

using namespace GameEngine;
using namespace GameEngine::Engine::Renderer;
namespace
{
Rendering::CameraData TestCamera(bool ortho = false)
{
    Rendering::CameraData camera{};
    const auto v = glm::lookAtLH(glm::vec3(0, 2, -6), glm::vec3(0, 0, 0), glm::vec3(0, 1, 0));
    const auto p = ortho ? glm::orthoLH_ZO(-4.0f, 4.0f, -3.0f, 3.0f, 100.0f, 0.1f)
                         : glm::perspectiveLH_ZO(glm::radians(60.0f), 1.5f, 100.0f, 0.1f);
    const auto vp = p * v;
    camera.view = Mathematics::Matrix4x4(v);
    camera.proj = Mathematics::Matrix4x4(p);
    camera.viewProj = Mathematics::Matrix4x4(vp);
    camera.cameraPos.w = ortho ? 1.0f : 0.0f;
    SetCameraEye(camera, Mathematics::WorldPosition{0.0, 2.0, -6.0});
    return camera;
}
PlanarReflectionPlane Plane()
{
    PlanarReflectionPlane p;
    const glm::mat4 identity(1);
    EXPECT_TRUE(ResolvePlanarReflectionPlane(glm::value_ptr(identity), 7, {}, p));
    return p;
}
ECS::EntityHandle AddPlane(ECS::World& world)
{
    const auto e = world.CreateEntity();
    world.AddComponentImmediate(e, Components::PlanarReflection{});
    world.AddComponentImmediate(e, Components::WorldTransform{});
    return e;
}
}

TEST(PlanarReflectionRegistration, SharedEngineRegistryCreatesPerViewNode)
{
    Pipeline::RenderPipelineNodeRegistry registry;
    Pipeline::Nodes::RegisterEngineNodeTypes(registry);
    const auto* type = registry.Find("PlanarReflection");
    ASSERT_NE(type, nullptr);
    EXPECT_TRUE(type->perView);
    ASSERT_TRUE(type->factory);
    EXPECT_NE(type->factory(), nullptr);
}
TEST(PlanarReflectionRegistration, CompositeSupportsInPlaceGraphicsAfterCompute)
{
    const auto desc = Pipeline::Nodes::PlanarReflectionNode::CompositeTargetDesc(640,480);
    EXPECT_EQ(desc.width,640u); EXPECT_EQ(desc.height,480u);
    for (auto usage : {Rendering::TextureUsage::RenderTarget, Rendering::TextureUsage::ShaderResource,
                       Rendering::TextureUsage::UnorderedAccess})
        EXPECT_NE(desc.usage & static_cast<uint32_t>(usage),0u);
}

TEST(PlanarReflectionMath, ArbitraryPlaneReflectionIsAnInvolutionAndFixesPlanePoints)
{
    const glm::vec3 normal = glm::normalize(glm::vec3(1, 2, -3));
    const glm::vec4 plane(normal, -2.0f);
    const auto reflection = PlanarReflectionMatrix(plane);
    const glm::vec4 onPlane(normal * 2.0f, 1);
    EXPECT_LT(glm::length(reflection * onPlane - onPlane), 1e-5f);
    const glm::vec4 point(4, -2, 7, 1);
    EXPECT_NEAR(glm::dot(plane, reflection * point), -glm::dot(plane, point), 1e-5f);
    EXPECT_LT(glm::length(reflection * reflection * point - point), 1e-5f);
    EXPECT_NEAR(glm::determinant(reflection), -1.0f, 1e-5f);
}
TEST(PlanarReflectionMath, InverseTransposeNormalHandlesRotatedScaledParent)
{
    auto world = glm::translate(glm::mat4(1), glm::vec3(3, -2, 5));
    world = glm::rotate(world, 0.7f, glm::normalize(glm::vec3(1, 0, 1)));
    world = glm::scale(world, glm::vec3(4, 2, 7));
    world[2] += world[0] * 0.2f;
    PlanarReflectionPlane p;
    ASSERT_TRUE(ResolvePlanarReflectionPlane(glm::value_ptr(world), 1, {}, p));
    EXPECT_NEAR(glm::length(glm::vec3(p.Equation)), 1, 1e-6f);
    for (const auto local : {glm::vec4(0, 0, 0, 1), glm::vec4(.5f, 0, -.5f, 1)})
    {
        const auto position = world * local;
        EXPECT_NEAR(glm::dot(p.Equation, position), 0, 1e-5f);
        EXPECT_LT(glm::length(p.WorldToLocal * position - local), 1e-5f);
    }
}
TEST(PlanarReflectionMath, ReverseZClipsBelowPlaneForPerspectiveAndOrthographicCameras)
{
    for (bool ortho : {false, true})
    {
        SCOPED_TRACE(ortho);
        const auto main = TestCamera(ortho);
        Rendering::CameraData mirror{};
        ASSERT_TRUE(MakePlanarReflectionCamera(main, Plane(), mirror));
        EXPECT_NEAR(mirror.cameraPos[1], -2, 1e-6f);
        const auto vp = mirror.viewProj.GetGLM();
        const auto above = vp * glm::vec4(0, 1, 0, 1);
        const auto below = vp * glm::vec4(0, -.5f, 0, 1);
        EXPECT_GT(above.w, 0);
        EXPECT_GE(above.z, 0);
        EXPECT_LE(above.z, above.w);
        EXPECT_GT(below.z, below.w);
        // Mirror plane positions map to the same screen XY as the main view.
        const auto mainPoint = main.viewProj.GetGLM() * glm::vec4(1, 0, 0, 1);
        const auto mirrorPoint = vp * glm::vec4(1, 0, 0, 1);
        EXPECT_NEAR(mainPoint.x / mainPoint.w, mirrorPoint.x / mirrorPoint.w, 1e-6f);
        EXPECT_NEAR(mainPoint.y / mainPoint.w, mirrorPoint.y / mirrorPoint.w, 1e-6f);
    }
}
TEST(PlanarReflectionMath, RejectsBacksideCamerasSingularTransformsAndNonfiniteSettings)
{
    auto main = TestCamera();
    main.cameraPos[1] = -1;
    Rendering::CameraData mirror{};
    EXPECT_FALSE(MakePlanarReflectionCamera(main, Plane(), mirror));
    glm::mat4 singular(0);
    PlanarReflectionPlane p;
    EXPECT_FALSE(ResolvePlanarReflectionPlane(glm::value_ptr(singular), 1, {}, p));
    glm::mat4 bad(1); bad[0][0] = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FALSE(ResolvePlanarReflectionPlane(glm::value_ptr(bad), 1, {}, p));
    Components::PlanarReflection settings;
    settings.ResolutionScale = std::numeric_limits<float>::infinity();
    const glm::mat4 identity(1);
    EXPECT_TRUE(ResolvePlanarReflectionPlane(glm::value_ptr(identity), 1, settings, p));
    EXPECT_FLOAT_EQ(p.Settings.ResolutionScale, 0.5f);
}
TEST(PlanarReflectionMath, BlurFollowsRoughnessAndCaptureResolution)
{
    EXPECT_FLOAT_EQ(PlanarReflectionBlurSigma(0, 0.5f), 0);
    EXPECT_NEAR(PlanarReflectionBlurSigma(0.35f, 0.5f), 0.735f, 1e-6f);
    EXPECT_NEAR(PlanarReflectionBlurSigma(0.35f, 1.0f), 1.47f, 1e-6f);
    EXPECT_FLOAT_EQ(PlanarReflectionBlurSigma(1, 1), 4);
}
TEST(PlanarReflectionMath, SharedOceanAndMirrorCameraHaveIdenticalConventions)
{
    const auto main = TestCamera();
    Rendering::CameraData ocean{}, mirror{};
    ASSERT_TRUE(MakeMirrorCaptureCamera(main, Plane().Equation, kPlanarClipBias, false, ocean));
    ASSERT_TRUE(MakePlanarReflectionCamera(main, Plane(), mirror));
    for (int i = 0; i < 16; ++i) EXPECT_FLOAT_EQ(ocean.viewProj.Data()[i], mirror.viewProj.Data()[i]);
    for (int i = 0; i < 4; ++i) EXPECT_FLOAT_EQ(ocean.cameraPos[i], mirror.cameraPos[i]);
}
TEST(PlanarReflectionMath, MirroredEyeAndRelativeProjectionKeepSubcentimeterPositionFarFromOrigin)
{
    auto main = TestCamera();
    const Mathematics::WorldPosition eye{125000.003, 2.003, -60000.007};
    for (int row = 0; row < 3; ++row)
        main.view[3][row] = static_cast<float>(
            -(main.view[0][row] * eye.X + main.view[1][row] * eye.Y + main.view[2][row] * eye.Z));
    main.viewProj = main.proj * main.view;
    SetCameraEye(main, eye);
    const auto inputEye = CameraEye(main);
    Rendering::CameraData reflected{};
    ASSERT_TRUE(MakeMirrorCaptureCamera(main, glm::vec4(0, 1, 0, 0), kPlanarClipBias, true, reflected));
    const auto observed = CameraEye(reflected);
    EXPECT_NEAR(observed.X, inputEye.X, 0.0002);
    EXPECT_NEAR(observed.Y, -inputEye.Y, 0.0002);
    EXPECT_NEAR(observed.Z, inputEye.Z, 0.0002);
    const auto relative = ViewRelativePoint(reflected, observed);
    const auto atEye = reflected.viewRel.GetGLM() * glm::vec4(relative.x, relative.y, relative.z, 1.0f);
    EXPECT_LT(glm::length(glm::vec3(atEye)), 0.0002f);
    const auto expected = reflected.proj * reflected.viewRel;
    for (int i = 0; i < 16; ++i)
        EXPECT_NEAR(reflected.viewProjRel.Data()[i], expected.Data()[i], 0.0002f);
}

TEST(PlanarReflectionMath, SharedShadowsPreserveSourceProjectionAndDepthAcrossOrigins)
{
    auto source = TestCamera();
    auto receiver = source;
    const auto moveEye = [](Rendering::CameraData& camera, const Mathematics::WorldPosition& eye)
    {
        for (int row = 0; row < 3; ++row)
            camera.view[3][row] = static_cast<float>(
                -(camera.view[0][row] * eye.X + camera.view[1][row] * eye.Y + camera.view[2][row] * eye.Z));
        camera.viewProj = camera.proj * camera.view;
        SetCameraEye(camera, eye);
    };
    moveEye(source, Mathematics::WorldPosition{100000.003, 2.001, -60000.007});
    moveEye(receiver, Mathematics::WorldPosition{-100000.004, 2.009, -60000.001});
    const auto origin = [](const Rendering::CameraData& c)
    {
        const auto p = SectorToWorld(CameraRenderOriginSector(c));
        return glm::vec3(p.x, p.y, p.z);
    };
    ShadowDataGPU data{};
    glm::mat4 light = glm::scale(glm::mat4(1), glm::vec3(.125f,.25f,.5f));
    for (auto& vp : data.shadowVP) std::memcpy(vp,glm::value_ptr(light),sizeof(vp));
    data.terrainShadowGrid0[0] = 20.0f;
    data.terrainShadowGrid0[1] = 30.0f;
    data.terrainShadowGrid0[2] = 40.0f;
    data.terrainShadowGrid0[3] = 50.0f;
    data.terrainShadowGrid3[2] = 60.0f;
    RebaseDirectionalShadowData(data,source,receiver);
    const auto originDelta = origin(receiver) - origin(source);
    EXPECT_FLOAT_EQ(data.terrainShadowGrid0[0], 20.0f - originDelta.x);
    EXPECT_FLOAT_EQ(data.terrainShadowGrid0[1], 30.0f - originDelta.z);
    EXPECT_FLOAT_EQ(data.terrainShadowGrid0[2], 40.0f - originDelta.x);
    EXPECT_FLOAT_EQ(data.terrainShadowGrid0[3], 50.0f - originDelta.z);
    EXPECT_FLOAT_EQ(data.terrainShadowGrid3[2], 60.0f - originDelta.y);
    const glm::vec3 world(100020,3,-60000);
    const auto expected = light * glm::vec4(world-origin(source),1);
    const auto actual = glm::make_mat4(data.shadowVP[0]) * glm::vec4(world-origin(receiver),1);
    EXPECT_LT(glm::length(actual-expected),1e-5f);
    const auto depth = Tests::ExactViewDepth(source, Mathematics::WorldPosition{world.x, world.y, world.z});
    EXPECT_NEAR(glm::dot(glm::make_vec4(data.ShadowReferenceDepth),glm::vec4(world-origin(receiver),1)),depth,1e-4f);
}
TEST(PlanarReflectionSystem, SharedCaptureTracksWorldMaskAndReleasesItsRegistryEntries)
{
    RenderServices services;
    MirrorCaptureView capture;
    Rendering::ViewDesc main{};
    main.worldId = 12; main.renderLayerMask = Rendering::RenderLayerMask{0xFu};
    main.cameraId = services.Views().AllocateCamera("MainMirrorTest");
    services.Views().SetCameraData(main.cameraId, TestCamera(), {0.25f, 543.0f});
    UpdateMirrorCaptureView(services,main,TestCamera(),0x5u,capture);
    const auto id = capture.View;
    auto* view = services.Views().FindViewDesc(id);
    ASSERT_NE(view,nullptr);
    EXPECT_FALSE(view->activeRenderPipeline);
    EXPECT_EQ(view->purpose,Rendering::ViewPurpose::UtilityCapture);
    EXPECT_EQ(view->worldId,12u);
    EXPECT_EQ(view->renderLayerMask.Bits,0x5u);
    const auto ranges = services.Views().FindCameraRanges(capture.Camera);
    EXPECT_FLOAT_EQ(ranges.Near, 0.25f);
    EXPECT_FLOAT_EQ(ranges.CullingRange, 543.0f);
    main.worldId = 13;
    UpdateMirrorCaptureView(services,main,TestCamera(),0x2u,capture);
    EXPECT_EQ(capture.View,id);
    EXPECT_EQ(services.Views().FindViewDesc(id)->worldId,13u);
    EXPECT_EQ(services.Views().FindViewDesc(id)->renderLayerMask.Bits,0x2u);
    ReleaseMirrorCaptureView(services,capture);
    EXPECT_EQ(capture.View,0u);
    EXPECT_EQ(services.Views().FindViewDesc(id),nullptr);
}
TEST(PlanarReflectionSystem, DisabledAndRemovedComponentsStopContributing)
{
    ECS::World world;
    const auto e = AddPlane(world);
    EXPECT_EQ(PlanarReflectionSystem::ResolvePlanes(world).size(), 1u);
    world.AddComponentImmediate(e, ECS::ComponentDisabled<Components::PlanarReflection>{});
    EXPECT_TRUE(PlanarReflectionSystem::ResolvePlanes(world).empty());
    world.RemoveComponentImmediate<ECS::ComponentDisabled<Components::PlanarReflection>>(e);
    world.AddComponentImmediate(e, ECS::Disabled{});
    EXPECT_TRUE(PlanarReflectionSystem::ResolvePlanes(world).empty());
    world.RemoveComponentImmediate<ECS::Disabled>(e);
    world.RemoveComponentImmediate<Components::PlanarReflection>(e);
    EXPECT_TRUE(PlanarReflectionSystem::ResolvePlanes(world).empty());
}
TEST(PlanarReflectionSystem, CapturesAreWorldScopedAndDoNotCreateProbeEnvironmentState)
{
    RenderServices services;
    PlanarReflectionSystem system(&services);
    ECS::World a, b;
    const auto e = AddPlane(a);
    AddPlane(b);
    system.Update(a, 0); system.Update(b, 0);
    auto* feature = services.GetFeature<PlanarReflectionFeature>();
    ASSERT_NE(feature, nullptr);
    Rendering::ViewDesc view{};
    view.worldId = a.GetWorldId();
    view.cameraId = services.Views().AllocateCamera("PlanarTestCamera");
    services.Views().SetCameraData(view.cameraId, TestCamera(), {0.1f, 100.0f});
    EXPECT_TRUE(feature->HasPlanes(view));
    a.AddComponentImmediate(e, ECS::ComponentDisabled<Components::PlanarReflection>{});
    system.Update(a, 0);
    EXPECT_FALSE(feature->HasPlanes(view));
    view.worldId = b.GetWorldId();
    EXPECT_TRUE(feature->HasPlanes(view));
    view.purpose = Rendering::ViewPurpose::UtilityCapture;
    EXPECT_FALSE(feature->HasPlanes(view));
    EXPECT_EQ(services.GetFeature<ImageBasedLightingFeature>(), nullptr);
}
TEST(PlanarReflectionSystem, PriorityBudgetIsDeterministic)
{
    RenderServices services;
    PlanarReflectionFeature feature;
    std::vector<PlanarReflectionPlane> planes;
    for (uint32_t i = 0; i < 7; ++i)
    {
        auto p = Plane(); p.EntityId = i; p.Settings.Priority = i / 2;
        planes.push_back(p);
    }
    feature.SyncWorld(services, 123, planes);
    Rendering::ViewDesc view{}; view.worldId = 123;
    view.cameraId = services.Views().AllocateCamera("PlanarTestCamera");
    services.Views().SetCameraData(view.cameraId, TestCamera(), {0.1f, 100.0f});
    const auto selected = feature.GetPlanes(view);
    ASSERT_EQ(selected.size(), 4u);
    EXPECT_EQ(selected[0].EntityId, 6u);
    EXPECT_EQ(selected[1].EntityId, 4u);
    EXPECT_EQ(selected[2].EntityId, 5u);
    EXPECT_EQ(selected[3].EntityId, 2u);
}

TEST(PlanarReflectionMath, VerticalMirrorClipsItsBackSide)
{
    const auto transform = glm::rotate(glm::mat4(1), glm::radians(-90.0f), glm::vec3(1, 0, 0));
    PlanarReflectionPlane plane;
    ASSERT_TRUE(ResolvePlanarReflectionPlane(glm::value_ptr(transform), 1, {}, plane));
    Rendering::CameraData reflected{};
    ASSERT_TRUE(MakePlanarReflectionCamera(TestCamera(), plane, reflected));
    EXPECT_NEAR(reflected.cameraPos[2], 6, 1e-5f);
    const auto vp = reflected.viewProj.GetGLM();
    const auto front = vp * glm::vec4(0, 1, -1, 1);
    const auto behind = vp * glm::vec4(0, 1, 1, 1);
    EXPECT_GT(front.w, 0);
    EXPECT_GE(front.z, 0);
    EXPECT_LE(front.z, front.w);
    EXPECT_GT(behind.z, behind.w);
}

namespace
{
using glm::vec2;
using glm::vec4;
using glm::abs;
using glm::clamp;
using glm::dot;
using glm::exp;
using glm::max;
using glm::min;
using glm::mix;
using glm::normalize;
using glm::smoothstep;
using glm::sqrt;
#include "../Modules/Rendering/Shaders/Includes/planar_reconstruction.glsl"
}

TEST(PlanarReflectionReconstruction, PreservesConstantHdrAndEmptyCapture)
{
    for (auto value : {vec4(0), vec4(1000, 80, 3, 1), vec4(2, 1, .5f, .25f)})
        for (int y = 0; y <= 8; ++y)
            for (int x = 0; x <= 8; ++x)
            {
                vec4 samples[16];
                std::fill(std::begin(samples), std::end(samples), value);
                const auto output = GE_PlanarReconstruct(samples, vec2(x, y) / 8.0f);
                for (int c = 0; c < 4; ++c)
                    EXPECT_NEAR(output[c], value[c], 2e-4f);
            }
}

TEST(PlanarReflectionReconstruction, SilhouetteCoverageRemainsPremultipliedWithoutHdrRinging)
{
    const vec4 foreground(500, 100, 20, 1);
    vec4 samples[16];
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x)
            samples[y * 4 + x] = x + y >= 3 ? foreground : vec4(0);
    for (int y = 0; y <= 16; ++y)
        for (int x = 0; x <= 16; ++x)
        {
            const auto output = GE_PlanarReconstruct(samples, vec2(x, y) / 16.0f);
            EXPECT_GE(output.w, 0);
            EXPECT_LE(output.w, 1);
            for (int c = 0; c < 3; ++c)
            {
                EXPECT_GE(output[c], 0);
                EXPECT_LE(output[c], foreground[c]);
                EXPECT_NEAR(output[c], foreground[c] * output.w, foreground[c] * 1e-6f);
            }
        }
}

TEST(PlanarReflectionReconstruction, ReducesDiagonalStepErrorAgainstBilinear)
{
    double reconstructedError = 0, bilinearError = 0;
    for (float slope : {0.5f, 1.0f, 2.0f})
        for (float offset : {-0.3f, 0.0f, 0.3f})
            for (int y = -20; y <= 20; ++y)
                for (int x = -20; x <= 20; ++x)
                {
                    const vec2 position = vec2(x, y) / 4.0f;
                    const auto base = glm::floor(position);
                    const auto fraction = position - base;
                    vec4 samples[16];
                    for (int j = 0; j < 4; ++j)
                        for (int i = 0; i < 4; ++i)
                        {
                            const auto p = base + vec2(i - 1, j - 1);
                            samples[j * 4 + i] = vec4(p.y > slope * p.x + offset ? 1.0f : 0.0f);
                        }
                    // Integrate the analytic edge across a 4x-upscaled pixel.
                    float reference = 0;
                    for (int sy = 0; sy < 8; ++sy)
                        for (int sx = 0; sx < 8; ++sx)
                        {
                            auto p = position + ((vec2(sx, sy) + .5f) / 8.0f - .5f) * .25f;
                            reference += p.y > slope * p.x + offset ? 1.0f / 64.0f : 0.0f;
                        }
                    const float reconstructed = GE_PlanarReconstruct(samples, fraction).x;
                    const float bilinear = mix(mix(samples[5], samples[6], fraction.x),
                                               mix(samples[9], samples[10], fraction.x), fraction.y).x;
                    reconstructedError += (reconstructed - reference) * (reconstructed - reference);
                    bilinearError += (bilinear - reference) * (bilinear - reference);
                }
    EXPECT_LT(reconstructedError, bilinearError) << "reconstruction=" << reconstructedError
                                                << " bilinear=" << bilinearError;
}

TEST(PlanarReflectionBlur, PairedLinearSamplesReproduceNormalizedGaussian)
{
    for (float sigma : {0.25f, 0.75f, 1.0f, 2.0f, 4.0f})
    {
        const auto kernel = MakePlanarReflectionBlurKernel(sigma);
        ASSERT_LE(kernel.Count, 7u);
        float discrete[14]{};
        discrete[0] = kernel.Taps[0].y;
        for (uint32_t i = 1; i < kernel.Count; ++i)
        {
            const auto tap = kernel.Taps[i];
            const int left = static_cast<int>(std::floor(tap.x));
            const float fraction = tap.x - left;
            discrete[left] += tap.y * (1.0f - fraction);
            discrete[left + 1] += tap.y * fraction;
        }
        const int radius = static_cast<int>(std::ceil(3.0f * sigma));
        float total = 1;
        for (int i = 1; i <= radius; ++i) total += 2 * std::exp(-.5f * (i / sigma) * (i / sigma));
        float reconstructedTotal = discrete[0];
        for (int i = 0; i < 14; ++i)
        {
            const float expected = i <= radius ? std::exp(-.5f * (i / sigma) * (i / sigma)) / total : 0;
            EXPECT_NEAR(discrete[i], expected, 1e-6f);
            EXPECT_GE(discrete[i], 0);
            if (i > 0)
            {
                EXPECT_LE(discrete[i], discrete[i - 1] + 1e-6f);
                reconstructedTotal += 2 * discrete[i];
            }
        }
        EXPECT_NEAR(reconstructedTotal, 1, 1e-6f);
    }
    EXPECT_EQ(MakePlanarReflectionBlurKernel(0.75f).Count, 3u); // 5 fetches per axis
}
TEST(PlanarReflectionBlur, InvalidAndDisabledRadiiProduceIdentity)
{
    for (float sigma : {0.0f, -1.0f, std::numeric_limits<float>::infinity(),
                        std::numeric_limits<float>::quiet_NaN()})
    {
        const auto kernel = MakePlanarReflectionBlurKernel(sigma);
        EXPECT_EQ(kernel.Count, 1u);
        EXPECT_EQ(kernel.Taps[0].x, 0);
        EXPECT_EQ(kernel.Taps[0].y, 1);
    }
}

TEST(PlanarReflectionSystem, BudgetSelectsVisibleFrontFacingPlanesIndependentlyPerView)
{
    RenderServices services;
    PlanarReflectionFeature feature;
    std::vector<PlanarReflectionPlane> planes;
    const glm::mat4 transforms[] = {
        glm::translate(glm::mat4(1), glm::vec3(1000, 0, 0)),
        glm::translate(glm::mat4(1), glm::vec3(0, 0, -100)),
        glm::rotate(glm::mat4(1), glm::radians(180.0f), glm::vec3(1, 0, 0)),
        glm::translate(glm::mat4(1), glm::vec3(0, 4, 0)),
        glm::mat4(1)
    };
    for (uint32_t i = 0; i < std::size(transforms); ++i)
    {
        Components::PlanarReflection settings;
        settings.Priority = 10 - i;
        PlanarReflectionPlane plane;
        ASSERT_TRUE(ResolvePlanarReflectionPlane(glm::value_ptr(transforms[i]), i, settings, plane));
        planes.push_back(plane);
    }
    feature.SyncWorld(services, 123, planes);
    Rendering::ViewDesc first{};
    first.worldId = 123;
    first.cameraId = services.Views().AllocateCamera("FirstRoom");
    services.Views().SetCameraData(first.cameraId, TestCamera(), {0.1f, 100.0f});
    const auto selected = feature.GetPlanes(first);
    ASSERT_EQ(selected.size(), 1u);
    EXPECT_EQ(selected.front().EntityId, 4u);

    auto second = first;
    second.cameraId = services.Views().AllocateCamera("SecondRoom");
    auto camera = TestCamera();
    const auto view = glm::lookAtLH(glm::vec3(1000, 2, -6), glm::vec3(1000, 0, 0), glm::vec3(0, 1, 0));
    const auto viewProj = camera.proj.GetGLM() * view;
    camera.view = Mathematics::Matrix4x4(view);
    camera.viewProj = Mathematics::Matrix4x4(viewProj);
    SetCameraEye(camera, Mathematics::WorldPosition{1000.0, 2.0, -6.0});
    services.Views().SetCameraData(second.cameraId, camera, {0.1f, 100.0f});
    const auto otherRoom = feature.GetPlanes(second);
    ASSERT_EQ(otherRoom.size(), 1u);
    EXPECT_EQ(otherRoom.front().EntityId, 0u);
    EXPECT_EQ(feature.GetPlanes(first).front().EntityId, 4u);
    first.cameraId = 0;
    EXPECT_TRUE(feature.GetPlanes(first).empty());
}

TEST(PlanarReflectionOwnership, ShaderSetupUsesANamedOwnerFunction)
{
    const auto path = std::filesystem::path(__FILE__).parent_path().parent_path() /
        "Source/Engine/Rendering/Pipeline/Nodes/PlanarReflectionNode.cpp";
    std::ifstream file(path, std::ios::binary);
    const std::string source(std::istreambuf_iterator<char>(file), {});
    ASSERT_FALSE(source.empty());
    EXPECT_EQ(source.find("auto load ="), std::string::npos);
    EXPECT_NE(source.find("void LoadPlanarShader("), std::string::npos);
}
