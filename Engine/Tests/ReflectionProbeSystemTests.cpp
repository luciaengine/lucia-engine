#include "Components/Rendering/Camera.h"
#include "Components/Rendering/ReflectionProbe.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Components/Rendering/LocalBounds.h"
#include "Components/Rendering/Light.h"
#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "ECSModules/Rendering/Systems/ReflectionProbeSystem.h"
#include "Engine/Rendering/ImageBasedLightingFeature.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/SceneReflectionProbeEnvironmentSource.h"
#include "Scene/SceneSchemaRegistry.h"

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Engine::Renderer;

TEST(ReflectionProbeSchema, InfluenceShapeRoundTripsThroughTheReflectedSchema)
{
    Scene::EnsureBuiltInSchemasRegistered();
    const auto* schema = Scene::SceneSchemaRegistry::Find("ReflectionProbe");
    ASSERT_NE(schema, nullptr);
    ECS::World world;
    for (const char* shape : {"Box", "Ellipsoid"})
    {
        SCOPED_TRACE(shape);
        const auto entity = world.CreateEntity();
        std::string error;
        ASSERT_TRUE(schema->ApplyProperty(world, entity, {}, "InfluenceShape", shape, &error)) << error;
        const auto expected = std::string_view(shape) == "Box"
            ? Components::ReflectionProbeInfluenceShape::Box
            : Components::ReflectionProbeInfluenceShape::Ellipsoid;
        ASSERT_NE(world.GetComponent<Components::ReflectionProbe>(entity), nullptr);
        EXPECT_EQ(world.GetComponent<Components::ReflectionProbe>(entity)->InfluenceShape, expected);

        std::vector<std::string> lines;
        schema->Serialize(world, entity, {}, lines);
        const auto restored = world.CreateEntity();
        bool wroteShape = false;
        for (const auto& line : lines)
        {
            EXPECT_EQ(line.find("BoxProjection"), std::string::npos);
            constexpr std::string_view prefix = "ReflectionProbe.InfluenceShape = ";
            if (line.starts_with(prefix))
            {
                wroteShape = true;
                ASSERT_TRUE(schema->ApplyProperty(world, restored, {}, "InfluenceShape",
                                                  std::string_view(line).substr(prefix.size()), &error)) << error;
            }
        }
        ASSERT_TRUE(wroteShape);
        ASSERT_NE(world.GetComponent<Components::ReflectionProbe>(restored), nullptr);
        EXPECT_EQ(world.GetComponent<Components::ReflectionProbe>(restored)->InfluenceShape, expected);
    }
}

namespace {
Components::WorldTransform MakeTransform(float x, float scale = 4.0f)
{
    Components::WorldTransform xf{};
    xf.matrix[0] = xf.matrix[5] = xf.matrix[10] = scale;
    xf.matrix[12] = x;
    return xf;
}
ECS::EntityHandle AddProbe(ECS::World& world, float x, uint32 priority = 0)
{
    const auto e = world.CreateEntity();
    Components::ReflectionProbe probe{};
    probe.Priority = priority;
    world.AddComponentImmediate(e, probe);
    world.AddComponentImmediate(e, MakeTransform(x));
    return e;
}
} // namespace

TEST(ReflectionProbeSystem, TwoRoomsRetainBothCapturesWhenCameraLeavesBothVolumes)
{
    ECS::World world;
    AddProbe(world, 0.0f);
    AddProbe(world, 10.0f);
    const auto camera = world.CreateEntity();
    world.AddComponentImmediate(camera, Components::Camera{});
    world.AddComponentImmediate(camera, MakeTransform(0.0f, 1.0f));
    RenderServices rs;
    ReflectionProbeSystem system(&rs);
    system.Update(world, 0.016f);
    auto* feature = rs.GetFeature<ImageBasedLightingFeature>();
    ASSERT_NE(feature, nullptr);
    ASSERT_EQ(feature->GetLocalProbeCount(world.GetWorldId()), 2u);
    const auto* first = feature->GetLocalProbeCapture(world.GetWorldId(), 0);
    const auto* second = feature->GetLocalProbeCapture(world.GetWorldId(), 1);
    world.GetComponentForWrite<Components::WorldTransform>(camera)->matrix[12] = 100.0f;
    for (int i = 0; i < 10; ++i) system.Update(world, 0.016f);
    EXPECT_EQ(feature->GetLocalProbeCapture(world.GetWorldId(), 0), first);
    EXPECT_EQ(feature->GetLocalProbeCapture(world.GetWorldId(), 1), second);
    EXPECT_FLOAT_EQ(feature->GetLocalProbeDesc(world.GetWorldId(), 0)->Intensity, 1.0f);
    EXPECT_FLOAT_EQ(feature->GetLocalProbeDesc(world.GetWorldId(), 1)->Intensity, 1.0f);
    EXPECT_EQ(feature->GetEnvironmentSource(), nullptr) << "local probes must not replace the global sky source";
}

TEST(ReflectionProbeSystem, PriorityReorderPreservesCaptureIdentity)
{
    ECS::World world;
    AddProbe(world, 0.0f);
    auto right = AddProbe(world, 10.0f);
    RenderServices rs;
    ReflectionProbeSystem system(&rs);
    system.Update(world, 0.016f);
    auto* feature = rs.GetFeature<ImageBasedLightingFeature>();
    const auto* rightCapture = feature->GetLocalProbeCapture(world.GetWorldId(), 1);
    world.GetComponentForWrite<Components::ReflectionProbe>(right)->Priority = 100;
    system.Update(world, 0.016f);
    EXPECT_EQ(feature->GetLocalProbeCapture(world.GetWorldId(), 0), rightCapture);
    EXPECT_EQ(feature->GetLocalProbeDesc(world.GetWorldId(), 0)->Priority, 100u);
}

TEST(ReflectionProbeSystem, WorldsAreIsolatedAndDisablingRemovesOnlyTheOwningProbe)
{
    ECS::World leftWorld, rightWorld, previewWorld;
    const auto left = AddProbe(leftWorld, 0.0f);
    AddProbe(rightWorld, 10.0f);
    RenderServices rs;
    ReflectionProbeSystem system(&rs);
    system.Update(leftWorld, 0.016f);
    system.Update(rightWorld, 0.016f);
    auto* feature = rs.GetFeature<ImageBasedLightingFeature>();
    const auto* right = feature->GetLocalProbeCapture(rightWorld.GetWorldId(), 0);
    system.Update(previewWorld, 0.016f);
    leftWorld.AddComponentImmediate(left, ECS::ComponentDisabled<Components::ReflectionProbe>{});
    system.Update(leftWorld, 0.016f);
    EXPECT_EQ(feature->GetLocalProbeCount(leftWorld.GetWorldId()), 0u);
    EXPECT_EQ(feature->GetLocalProbeCount(previewWorld.GetWorldId()), 0u);
    EXPECT_EQ(feature->GetLocalProbeCapture(rightWorld.GetWorldId(), 0), right);
}

TEST(ReflectionProbeSystem, CapacityUsesPriorityAndStableEntityOrder)
{
    ECS::World world;
    AddProbe(world, 0.0f, 1);
    AddProbe(world, 10.0f, 2);
    AddProbe(world, 20.0f, 3);
    RenderServices rs;
    ReflectionProbeSystem system(&rs);
    system.Update(world, 0.016f);
    auto* feature = rs.GetFeature<ImageBasedLightingFeature>();
    ASSERT_EQ(feature->GetLocalProbeCount(world.GetWorldId()), 2u);
    EXPECT_EQ(feature->GetLocalProbeDesc(world.GetWorldId(), 0)->Priority, 3u);
    EXPECT_EQ(feature->GetLocalProbeDesc(world.GetWorldId(), 1)->Priority, 2u);
}

TEST(ReflectionProbeSystem, EllipsoidRetainsVolumeAndOffsetWithoutCamera)
{
    ECS::World world;
    auto entity = AddProbe(world, 7.0f);
    auto* probe = world.GetComponentForWrite<Components::ReflectionProbe>(entity);
    probe->InfluenceShape = Components::ReflectionProbeInfluenceShape::Ellipsoid;
    probe->OriginOffsetX = 0.5f;
    probe->BlendDistance = 0.75f;
    RenderServices rs;
    ReflectionProbeSystem system(&rs);
    system.Update(world, 0.016f);
    auto* feature = rs.GetFeature<ImageBasedLightingFeature>();
    auto* desc = feature->GetLocalProbeDesc(world.GetWorldId(), 0);
    ASSERT_NE(desc, nullptr);
    EXPECT_FALSE(desc->BoxProjection);
    EXPECT_FLOAT_EQ(desc->BoxHalfExtents[0], 2.0f);
    EXPECT_FLOAT_EQ(desc->BoxCenter[0], 7.0f);
    EXPECT_FLOAT_EQ(desc->Position[0], 7.5f);
    EXPECT_FLOAT_EQ(desc->BlendDistance, 0.75f);
}

TEST(ReflectionProbeSystem, ClosingAWorldReleasesItsCaptureViews)
{
    ECS::World world;
    AddProbe(world, 0.0f);
    RenderServices rs;
    ReflectionProbeSystem system(&rs);
    system.Update(world, 0.016f);
    auto* feature = rs.GetFeature<ImageBasedLightingFeature>();
    ASSERT_EQ(feature->GetLocalProbeCount(world.GetWorldId()), 1u);
    FeatureCullingContext culling{};
    feature->OnScheduleCulling(culling);
    // The next frame has no ECS registration for the closed world.
    feature->OnScheduleCulling(culling);
    EXPECT_EQ(feature->GetLocalProbeCount(world.GetWorldId()), 0u);
    EXPECT_TRUE(rs.Views().GetViews().empty());
}

TEST(ReflectionProbeSystem, MaterialEditsInvalidateOnceCaptureButBlendControlsDoNot)
{
    RenderServices rs;
    SceneReflectionProbeEnvironmentSource source(rs);
    SceneReflectionProbeEnvironmentDesc desc{};
    desc.WorldId = 1;
    desc.SourceKey = 1;
    desc.UpdateMode = SceneReflectionProbeUpdateMode::Once;
    source.SetProbe(desc);
    auto material = Material::TestFactory::Create(GUID::Null(), "Probe material", 16);
    Material::TestFactory::AddPropertyLayout(material, HashStringId("roughness"), 0, 4);
    const auto initial = source.InputDigest();
    desc.Intensity = 2;
    desc.BlendDistance = 3;
    desc.Priority = 7;
    source.SetProbe(desc);
    EXPECT_EQ(source.InputDigest(), initial);
    material.SetFloat(HashStringId("roughness"), 0.8f);
    EXPECT_NE(source.InputDigest(), initial);
    const auto edited = source.InputDigest();
    desc.CaptureEnvironment = true;
    source.SetProbe(desc);
    EXPECT_NE(source.InputDigest(), edited);
}

#include "Rendering/Core/CommandList.h"
#include "Rendering/Materials/ShaderCompileService.h"
#include "TestDeviceHelper.h"
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>

// Execute the production GLSL and the production EnvData upload together. This
// catches shader/CPU layout drift as well as the two-room blending behavior.
TEST(ReflectionProbeSystem, GpuWeightsBlendRoomsSkyPriorityAndCaptureReadiness)
{
#ifndef GE_RENDERER_REPO_ROOT
    GTEST_SKIP() << "Repository shader source unavailable";
#else
    using namespace GameEngine::Rendering;
    auto device = CreateVulkanDeviceFast();
    if (!device) GTEST_SKIP() << "No rendering device available";
    RenderServices rs;
    ImageBasedLightingFeature feature;
    ASSERT_TRUE(feature.Initialize(device.get()));
    SceneReflectionProbeEnvironmentDesc probes[2];
    for (uint32_t i = 0; i < 2; ++i)
    {
        auto& probe = probes[i];
        probe.WorldId = 1;
        probe.SourceKey = i + 1;
        probe.Position[0] = probe.BoxCenter[0] = i == 0 ? -2.0f : 2.0f;
        probe.Position[1] = 0.5f; // deliberately offset the capture from the volume center
        for (auto& extent : probe.BoxHalfExtents) extent = 4.0f;
        probe.BlendDistance = 2.0f;
        probe.CaptureResolution = 32;
    }
    feature.UpdateLocalProbes(rs, 1, probes);
    for (uint32_t i = 0; i < 2; ++i)
    {
        auto* capture = const_cast<ImageBasedLightingFeature*>(feature.GetLocalProbeCapture(1, i));
        ASSERT_TRUE(capture->Initialize(device.get()));
        capture->SetLastBakedDigest(1); // completed-capture publication seam
    }
    const auto shaderDir = std::filesystem::path(GE_RENDERER_REPO_ROOT) / "Engine/Modules/Rendering/Shaders";
    ShaderProgramCompileRequest request{};
    request.debugName = "LocalProbeBlendGpuTest";
    request.baseDirectory = shaderDir;
    request.includeDirs = {shaderDir};
    request.cacheRoot = std::filesystem::temp_directory_path() / "ge-local-probe-gpu-tests";
    ShaderStageCompileSpec stage{};
    stage.stage = "cs";
    stage.sourcePath = shaderDir / "local_probe_test.comp";
    stage.inlineSource = R"GLSL(#version 450
#include "Includes/environment_data.glsl"
#include "Includes/local_reflections.glsl"
layout(local_size_x=1) in;
layout(std430,set=0,binding=0) buffer Results { vec4 values[]; } result;
void main() {
    result.values[0] = vec4(GE_LocalProbeWeights(vec3(-4,0,0)), 0);
    result.values[1] = vec4(GE_LocalProbeWeights(vec3(0,0,0)), 0);
    result.values[2] = vec4(GE_LocalProbeWeights(vec3(4,0,0)), 0);
    result.values[3] = vec4(GE_LocalProbeWeights(vec3(5,0,0)), 0);
    result.values[4] = vec4(GE_LocalProbeWeights(vec3(7,0,0)), 0);
    result.values[5] = vec4(GE_ProbeReflectionDirection(Env.localProbes[0], vec3(-3,0,0), vec3(1,0,0)), 0);
    GE_LocalProbeData p = Env.localProbes[0];
    p.halfExtentsProjection = vec4(4,2,1,0);
    p.control.z = 2u;
    p.axisX = vec4(0,0,1,0); p.axisZ = vec4(-1,0,0,0);
    result.values[6] = vec4(GE_ProbeInfluence(p, p.centerBlend.xyz + vec3(0,0,3)),
                            GE_ProbeInfluence(p, p.centerBlend.xyz + vec3(0,0,3.5)),
                            GE_ProbeInfluence(p, p.centerBlend.xyz + vec3(1.1,0,0)), 0);
})GLSL";
    request.stages.push_back(stage);
    ShaderProgramCompileResult compiled{};
    std::string error;
    ASSERT_TRUE(ShaderCompileService::CompileProgramToCache(request, device->PreferredShaderSource(), compiled, &error)) << error;
    PipelineDesc pipelineDesc{};
    pipelineDesc.type = PipelineType::Compute;
    pipelineDesc.computeShader = compiled.stageBytes.at("cs");
    DescriptorSetLayoutDesc layout{};
    auto binding = [&](uint32_t index, DescriptorType type) {
        DescriptorBinding desc{};
        desc.binding = index; desc.type = type; desc.count = 1; desc.shaderStages = kShaderStageCompute;
        layout.bindings.push_back(desc);
    };
    binding(0, DescriptorType::StorageBuffer);
    binding(21, DescriptorType::UniformBuffer);
    for (uint32_t i = 44; i <= 47; ++i) binding(i, DescriptorType::CombinedImageSampler);
    pipelineDesc.descriptorSetLayouts.push_back(layout);
    const auto pipeline = device->CreatePipeline(pipelineDesc);
    ASSERT_TRUE(pipeline.IsValid());
    constexpr size_t bytes = 7 * 4 * sizeof(float);
    BufferDesc outputDesc{};
    outputDesc.size = bytes;
    outputDesc.usage = static_cast<uint32_t>(BufferUsage::Storage);
    outputDesc.memoryUsage = BufferMemoryUsage::Readback;
    outputDesc.flags = BufferCreateFlags::PersistentlyMapped;
    const auto output = device->CreateBuffer(outputDesc);
    ASSERT_TRUE(output.IsValid());
    DescriptorSetDesc setDesc{};
    setDesc.layout = layout;
    const auto set = device->CreateDescriptorSet(setDesc);
    ASSERT_TRUE(set.IsValid());
    DescriptorSetUpdate outputBinding{};
    outputBinding.binding = 0;
    outputBinding.type = DescriptorType::StorageBuffer;
    outputBinding.buffers = {output}; outputBinding.bufferOffsets = {0}; outputBinding.bufferRanges = {bytes};
    device->UpdateDescriptorSet(set, outputBinding);
    for (uint32_t i = 44; i <= 47; ++i)
        device->UpdateCombinedImageSamplerBinding(set, i, feature.GetSet().diffuseEnvView, feature.GetCubeSampler());

    auto run = [&]() {
        DescriptorSetUpdate envBinding{};
        envBinding.binding = 21; envBinding.type = DescriptorType::UniformBuffer;
        envBinding.buffers = {feature.UploadEnvData(device.get(), 1)};
        envBinding.bufferOffsets = {0}; envBinding.bufferRanges = {ImageBasedLightingFeature::kEnvDataBytes};
        device->UpdateDescriptorSet(set, envBinding);
        auto commands = device->CreateCommandList(IDevice::QueueType::Compute);
        commands->Begin(); commands->SetPipeline(pipeline); commands->BindDescriptorSet(0, set, pipeline);
        commands->Dispatch(1,1,1); commands->End();
        device->ExecuteCommandLists({commands.get()}); device->WaitForIdle();
        std::array<std::array<float,4>,7> values{};
        auto* mapped = device->MapBuffer(output);
        EXPECT_NE(mapped, nullptr);
        if (mapped) std::memcpy(values.data(), mapped, bytes);
        device->UnmapBuffer(output);
        return values;
    };
    auto equal = run();
    const float expected[5][3] = {{1,0,0}, {0.5f,0.5f,0}, {0,1,0}, {0,0.5f,0.5f}, {0,0,1}};
    for (int i = 0; i < 5; ++i)
        for (int j = 0; j < 3; ++j) EXPECT_NEAR(equal[i][j], expected[i][j], 1e-5f) << i << "," << j;
    EXPECT_NEAR(equal[5][0], 4.0f / std::sqrt(16.25f), 1e-5f);
    EXPECT_NEAR(equal[5][1], -0.5f / std::sqrt(16.25f), 1e-5f);
    EXPECT_NEAR(equal[6][0], 1.0f, 1e-5f);
    EXPECT_NEAR(equal[6][1], 0.5f, 1e-5f);
    EXPECT_NEAR(equal[6][2], 0.0f, 1e-5f);

    probes[0].Priority = 0xFFFFFFFFu; // preserve full integer priority in the UBO
    feature.UpdateLocalProbes(rs, 1, probes);
    auto priority = run();
    EXPECT_NEAR(priority[1][0], 1.0f, 1e-5f);
    EXPECT_NEAR(priority[1][1], 0.0f, 1e-5f);
    const_cast<ImageBasedLightingFeature*>(feature.GetLocalProbeCapture(1, 0))->SetLastBakedDigest(0);
    auto pending = run();
    EXPECT_NEAR(pending[0][2], 1.0f, 1e-5f) << "a pending capture leaves sky coverage";
    EXPECT_NEAR(pending[1][1], 1.0f, 1e-5f) << "a pending high-priority probe must not hide a completed lower-priority probe";
    device->DestroyDescriptorSet(set);
    device->DestroyPipeline(pipeline);
    device->DestroyBuffer(output);
#endif
}


TEST(ReflectionProbeSystem, ProjectionBoundsDoNotChangeInfluenceBounds)
{
    ECS::World world;
    auto entity = AddProbe(world, 5.0f);
    auto* probe = world.GetComponentForWrite<Components::ReflectionProbe>(entity);
    probe->InfluenceShape = Components::ReflectionProbeInfluenceShape::Box;
    probe->ProjectionScaleX = 2;
    probe->ProjectionOffsetZ = 3;
    probe->OriginOffsetY = 0.5f;
    RenderServices rs;
    ReflectionProbeSystem system(&rs);
    system.Update(world, 0.016f);
    auto* feature = rs.GetFeature<ImageBasedLightingFeature>();
    const auto* desc = feature->GetLocalProbeDesc(world.GetWorldId(), 0);
    ASSERT_NE(desc, nullptr);
    EXPECT_TRUE(desc->BoxProjection);
    EXPECT_EQ(desc->InfluenceShape, 1u);
    EXPECT_FLOAT_EQ(desc->BoxHalfExtents[0], 2);
    EXPECT_FLOAT_EQ(desc->ProjectionHalfExtents[0], 4);
    EXPECT_FLOAT_EQ(desc->ProjectionCenter[2], 3);
    EXPECT_FLOAT_EQ(desc->BoxCenter[2], 0);
    EXPECT_FLOAT_EQ(desc->Position[1], 0.5f);
    EXPECT_GT(desc->InfluenceRadius, 6.0f) << "Capture range must include the projection room's walls";
    probe = world.GetComponentForWrite<Components::ReflectionProbe>(entity);
    probe->InfluenceShape = Components::ReflectionProbeInfluenceShape::Ellipsoid;
    system.Update(world, 0.016f);
    desc = feature->GetLocalProbeDesc(world.GetWorldId(), 0);
    EXPECT_EQ(desc->InfluenceShape, 2u);
    EXPECT_FLOAT_EQ(desc->BoxHalfExtents[0], 2);
}

TEST(ReflectionProbeSystem, MovingGeometryInvalidatesOnlyIntersectingCaptureRanges)
{
    ECS::World world;
    AddProbe(world, 0);
    AddProbe(world, 20);
    const auto wall = world.CreateEntity();
    world.AddComponentImmediate(wall, Components::MeshRenderer{});
    world.AddComponentImmediate(wall, Components::LocalBounds{});
    world.AddComponentImmediate(wall, MakeTransform(0, 1));
    RenderServices rs;
    ReflectionProbeSystem system(&rs);
    system.Update(world, 0.016f);
    auto* ibl = rs.GetFeature<ImageBasedLightingFeature>();
    auto digest = [&](uint32_t index) { return ibl->GetLocalProbeDesc(world.GetWorldId(), index)->SceneEditDigest; };
    auto west = digest(0), east = digest(1);
    world.GetComponentForWrite<Components::WorldTransform>(wall)->matrix[12] = 1;
    system.Update(world, 0.016f);
    EXPECT_NE(digest(0), west);
    EXPECT_EQ(digest(1), east);
    west = digest(0);
    world.GetComponentForWrite<Components::WorldTransform>(wall)->matrix[12] = 20;
    system.Update(world, 0.016f);
    EXPECT_NE(digest(0), west) << "Removing a wall from the old room changes that room's capture";
    EXPECT_NE(digest(1), east) << "Entering the new room invalidates its capture too";
}

TEST(ReflectionProbeSystem, MaterialVersionsAndLightEditsRefreshStaticCaptures)
{
    ECS::World world;
    AddProbe(world, 0);
    AddProbe(world, 20);
    RenderServices rs;
    rs.Materials().Registry().SeedTextureSlotDefaults(0, 0, 0);
    const GUID materialId = GUID::Generate();
    MaterialDocument document{};
    document.materialName = "Probe wall";
    document.lightingModel = "StandardPBR";
    auto* material = rs.Materials().Registry().Register(materialId, document);
    ASSERT_NE(material, nullptr);
    Material::TestFactory::AddPropertyLayout(*material, HashStringId("probe_test_value"), 0, 4);
    auto wall = world.CreateEntity();
    Components::MeshRenderer mesh{};
    mesh.materialAssetGuid.Set(materialId);
    world.AddComponentImmediate(wall, mesh);
    world.AddComponentImmediate(wall, Components::LocalBounds{});
    world.AddComponentImmediate(wall, MakeTransform(0, 1));
    auto sun = world.CreateEntity();
    world.AddComponentImmediate(sun, Components::Light{});
    world.AddComponentImmediate(sun, MakeTransform(0, 1));
    ReflectionProbeSystem system(&rs);
    system.Update(world, 0.016f);
    auto* ibl = rs.GetFeature<ImageBasedLightingFeature>();
    auto digest = [&](uint32_t index) { return ibl->GetLocalProbeCapture(world.GetWorldId(), index)->GetEnvironmentSource()->InputDigest(); };
    auto west = digest(0), east = digest(1);
    material->SetFloat(HashStringId("probe_test_value"), 0.4f);
    system.Update(world, 0.016f);
    EXPECT_NE(digest(0), west);
    EXPECT_EQ(digest(1), east) << "A material outside a probe's capture range must not invalidate it";
    west = digest(0);
    material->SetFloat(HashStringId("probe_test_value"), 0.4f);
    system.Update(world, 0.016f);
    EXPECT_EQ(digest(0), west) << "Idempotent material writes must not keep static probes baking";
    world.GetComponentForWrite<Components::Light>(sun)->ColorTemperature = 3500;
    world.GetComponentForWrite<Components::Light>(sun)->UseColorTemperature = true;
    system.Update(world, 0.016f);
    EXPECT_NE(digest(0), west);
    EXPECT_NE(digest(1), east) << "Directional-light edits affect both capture ranges";
}

TEST(ReflectionProbeSystem, ExplicitRebakeRefreshesProbesWithoutChangingOtherWorlds)
{
    ECS::World first, second;
    auto e = AddProbe(first, 0);
    AddProbe(second, 0);
    RenderServices rs;
    ReflectionProbeSystem system(&rs);
    system.Update(first, 0.016f); system.Update(second, 0.016f);
    auto* ibl = rs.GetFeature<ImageBasedLightingFeature>();
    auto* firstSource = ibl->GetLocalProbeCapture(first.GetWorldId(), 0)->GetEnvironmentSource();
    auto* secondSource = ibl->GetLocalProbeCapture(second.GetWorldId(), 0)->GetEnvironmentSource();
    auto a = firstSource->InputDigest(), b = secondSource->InputDigest();
    const auto key = ibl->GetLocalProbeDesc(first.GetWorldId(), 0)->SourceKey;
    ibl->RequestLocalProbeRebake(first.GetWorldId(), key);
    EXPECT_NE(firstSource->InputDigest(), a);
    EXPECT_EQ(secondSource->InputDigest(), b);
}

TEST(ReflectionProbeSystem, AgingOutweighsPriority)
{
    RenderServices rs;
    SceneReflectionProbeEnvironmentSource high(rs), waiting(rs);
    SceneReflectionProbeEnvironmentDesc desc{};
    desc.SourceKey = 1; desc.WorldId = 1; desc.Priority = 0xffffffffu;
    high.SetProbe(desc);
    desc.SourceKey = 2; desc.Priority = 0;
    waiting.SetProbe(desc);
    high.TickPending(0.01f);
    waiting.TickPending(3.0f);
    EXPECT_GT(waiting.SchedulingScore(), high.SchedulingScore()) << "A low-priority probe cannot starve";

}

TEST(ReflectionProbeSystem, PublishingACompletedCapturePreservesFeatureAndResourceOwnership)
{
    using namespace Rendering;
    auto device = CreateVulkanDeviceFast();
    if (!device) GTEST_SKIP() << "No rendering device available";
    ImageBasedLightingFeature visible, staging;
    ASSERT_TRUE(visible.Initialize(device.get()));
    ASSERT_TRUE(staging.Initialize(device.get()));
    ASSERT_TRUE(staging.EnsureCaptureResolution(32));
    const auto oldDiffuse = visible.GetIrradianceCube();
    const auto oldSpecular = visible.GetPrefilterCube();
    const auto newSpecular = staging.GetPrefilterCube();
    const auto oldLut = visible.GetBrdfLut();
    const auto buffer = visible.UploadEnvData(device.get());
    EXPECT_EQ(visible.GetPrefilterCube(), oldSpecular) << "Staging allocation cannot replace the visible capture";
    visible.PublishCaptureFrom(staging);
    EXPECT_EQ(visible.GetPrefilterCube(), newSpecular);
    EXPECT_EQ(visible.GetEnvCaptureSize(), 32u);
    EXPECT_EQ(staging.GetIrradianceCube(), oldDiffuse);
    EXPECT_EQ(staging.GetPrefilterCube(), oldSpecular);
    EXPECT_EQ(visible.GetBrdfLut(), oldLut);
    EXPECT_EQ(visible.UploadEnvData(device.get()), buffer);
    device->WaitForIdle();
}


TEST(ReflectionProbeSystem, LayerAndBoundsEditsRefreshCachedCaptureMembership)
{
    ECS::World world;
    const auto probe = AddProbe(world, 0);
    world.GetComponentForWrite<Components::ReflectionProbe>(probe)->CullMask = 1;
    const auto object = world.CreateEntity();
    Components::MeshRenderer mesh{};
    const GUID material = GUID::Generate();
    mesh.materialAssetGuid.Set(material);
    mesh.renderLayerMask = 2;
    world.AddComponentImmediate(object, mesh);
    world.AddComponentImmediate(object, Components::LocalBounds{});
    world.AddComponentImmediate(object, MakeTransform(0, 1));
    RenderServices services;
    ReflectionProbeSystem system(&services);
    system.Update(world, 0);
    auto* feature = services.GetFeature<ImageBasedLightingFeature>();
    ASSERT_NE(feature, nullptr);
    const auto capture = [&]() { return feature->GetLocalProbeDesc(world.GetWorldId(), 0); };
    ASSERT_NE(capture(), nullptr);
    EXPECT_TRUE(capture()->MaterialKeys.empty());
    for (int frame = 0; frame < 3; ++frame) system.Update(world, 0);
    world.GetComponentForWrite<Components::MeshRenderer>(object)->renderLayerMask = 1;
    system.Update(world, 0);
    EXPECT_EQ(capture()->MaterialKeys, std::vector<GUID>{material});
    const auto included = capture()->SceneEditDigest;
    world.GetComponentForWrite<Components::LocalBounds>(object)->Box.center.x = 100;
    system.Update(world, 0);
    EXPECT_TRUE(capture()->MaterialKeys.empty());
    EXPECT_NE(capture()->SceneEditDigest, included);
    world.GetComponentForWrite<Components::LocalBounds>(object)->Box.center.x = 0;
    system.Update(world, 0);
    EXPECT_EQ(capture()->MaterialKeys, std::vector<GUID>{material});
    world.AddComponentImmediate(object, ECS::ComponentDisabled<Components::MeshRenderer>{});
    system.Update(world, 0);
    EXPECT_TRUE(capture()->MaterialKeys.empty());
}

TEST(ReflectionProbeSystem, RemovingAllProbesAndAddingOneRebuildsTheDescriptor)
{
    ECS::World world;
    const auto first = AddProbe(world, 0);
    RenderServices services;
    ReflectionProbeSystem system(&services);
    system.Update(world, 0);
    auto* feature = services.GetFeature<ImageBasedLightingFeature>();
    ASSERT_NE(feature, nullptr);
    const auto firstKey = feature->GetLocalProbeDesc(world.GetWorldId(), 0)->SourceKey;
    world.AddComponentImmediate(first, ECS::ComponentDisabled<Components::ReflectionProbe>{});
    system.Update(world, 0);
    EXPECT_EQ(feature->GetLocalProbeCount(world.GetWorldId()), 0u);
    AddProbe(world, 20);
    system.Update(world, 0);
    ASSERT_EQ(feature->GetLocalProbeCount(world.GetWorldId()), 1u);
    const auto* replacement = feature->GetLocalProbeDesc(world.GetWorldId(), 0);
    ASSERT_NE(replacement, nullptr);
    EXPECT_NE(replacement->SourceKey, firstKey);
    EXPECT_EQ(replacement->Position[0], 20);
}
