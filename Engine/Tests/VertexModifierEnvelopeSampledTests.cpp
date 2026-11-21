// The sampled bound check for the deformation envelopes the shipped vertex
// modifiers declare.
//
// Culling trusts an @deformationEnvelope: a deforming instance's sphere is
// widened by the declared distance and nothing else. A declaration below what
// the modifier really does lets deformed geometry be culled while on screen, so
// each shipped declaration is checked against the modifier's own GLSL: the
// modifier source is spliced into a compute kernel that supplies the inputs it
// reads, and the kernel runs it over a grid of clock values, wind vectors,
// material parameters, positions, texture coordinates and instance seeds,
// keeping the largest displacement over the scale property's magnitude. That
// ratio must stay at or under the declared meters.
//
// What is real here and what is not: the modifier text and the declaration are
// the shipped file, read from the source tree, and the declaration is read by
// the production parser (BuildShaderPropertyTable). The kernel around the
// modifier is written here; it stands in for the composed vertex stage only in
// the inputs the modifier reads. A sample grid finds the largest displacement it
// visits, not the true supremum, so the check catches a declaration that is too
// low, not one that is merely loose.

#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/Pipeline/PipelineTypes.h"
#include "Rendering/Materials/Shader/ShaderCompileService.h"
#include "Rendering/Materials/Shader/ShaderPropertyTable.h"

#include "TestDeviceHelper.h"

#include <gtest/gtest.h>

#include <bit>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#ifndef GE_RENDERER_REPO_ROOT
#error "GE_RENDERER_REPO_ROOT must be defined by CMake (source anchoring for shader contracts)"
#endif

using namespace GameEngine::Rendering;

namespace
{
namespace fs = std::filesystem;

const fs::path kRepoRoot = fs::path(GE_RENDERER_REPO_ROOT);

// The modifiers this file samples, relative to the repository root.
const char* const kEzTreeWind = "Packages/eztree/Assets/Shaders/VertexModifiers/ez_tree_wind.glsl";
const char* const kFoliageWind = "Engine/Modules/Rendering/Shaders/VertexModifiers/foliage_wind.glsl";

std::string ReadText(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

DeformationEnvelopeDeclaration ReadDeclaration(const std::string& modifierText, const std::string& file)
{
    const ShaderPropertyTable table = BuildShaderPropertyTable(
        {{"", "surface.glsl", ShaderPropertyOrigin::Surface}, {modifierText, file, ShaderPropertyOrigin::VertexModifier}});
    return table.DeformationEnvelope;
}

// ez_tree_wind.glsl reads its wind vector from Mat.uParams1 (xyz, w = strength scale),
// its frequency, spatial scale, tree height and base height from Mat.uParams2,
// the clock, the instance index, the model matrix and the sector from InstanceData,
// the weather wind volumes from Light, and the leaf UV and seed from aUV0 and aUV1.
// The samples place no wind volume: the declared envelope bounds the authored wind,
// and the tree extraction adds the bound of the volumes. One invocation is one sample: x walks the clock, y the
// wind direction and magnitude, z the height, the UV, the leaf seed, the
// instance and the spatial scale. The result is the largest
// |displacement| / |windStrength.xyz| as float bits (positive floats order as
// their bits) and the number of samples taken.
constexpr uint32_t kTimeSamples = 2048u;
constexpr uint32_t kWindSamples = 32u;
constexpr uint32_t kShapeSamples = 6u * 4u * 4u * 4u * 2u;

const char* const kEzTreeWindPrologue = R"(#version 450
layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

#include "Includes/instance_sector.glsl"
struct InstanceData
{
    float deformationTimeSeconds;
    uint instanceIndex;
    mat4 modelMatrix;
    ivec3 sector;
};
struct WeatherLightLanes
{
    vec4 uWeatherWindCount;
    vec4 uWeatherWind[1];
};
WeatherLightLanes Light;
struct LegacyMaterialLanes
{
    vec4 uParams1;
    vec4 uParams2;
};
LegacyMaterialLanes Mat;
vec2 aUV0;
vec2 aUV1;
#define HAS_UV0
#define HAS_UV1
#line 1
)";

const char* const kEzTreeWindSampler = R"(
layout(std430, set = 0, binding = 0) buffer SampledBound
{
    uint maxRatioBits;
    uint sampleCount;
};

void main()
{
    uint timeIndex = gl_GlobalInvocationID.x;
    uint windIndex = gl_GlobalInvocationID.y;
    uint shapeIndex = gl_GlobalInvocationID.z;

    float azimuth = float(windIndex % 8u) * (6.2831853 / 8.0) + 0.1;
    float elevation = float((windIndex / 8u) % 2u) * 0.8 - 0.4;
    float magnitude = (windIndex / 16u) == 0u ? 0.34 : 2.0;
    vec3 windDir = vec3(cos(elevation) * cos(azimuth), sin(elevation), cos(elevation) * sin(azimuth));

    const float heights[6] = float[6](0.0, 3.0, 6.0, 8.5, 10.0, 14.0);
    const vec2 uvs[4] = vec2[4](vec2(0.0, 0.0), vec2(0.5, 0.5), vec2(1.0, 0.37), vec2(0.93, 0.08));
    uint heightIndex = shapeIndex % 6u;
    uint uvIndex = (shapeIndex / 6u) % 4u;
    uint seedIndex = (shapeIndex / 24u) % 4u;
    uint instanceIndex = (shapeIndex / 96u) % 4u;
    uint spatialIndex = shapeIndex / 384u;

    Light.uWeatherWindCount = vec4(0.0);
    Light.uWeatherWind[0] = vec4(0.0);
    Mat.uParams1 = vec4(windDir * magnitude, 1.0);
    Mat.uParams2 = vec4(1.0, spatialIndex == 0u ? 1.0 : 4.0, 10.0, 0.0);
    aUV0 = uvs[uvIndex];
    aUV1 = vec2(float(seedIndex) * 0.23 + 0.05, 0.0);

    InstanceData inst;
    inst.deformationTimeSeconds = float(timeIndex) * 0.7371;
    inst.instanceIndex = instanceIndex * 37u + 3u;
    inst.modelMatrix = mat4(1.0);
    inst.modelMatrix[3] = vec4(float(instanceIndex) * 9.0, 0.0, 0.0, 1.0);
    inst.sector = ivec3(0);

    vec3 position = vec3(float(timeIndex % 7u) * 1.3 - 4.0, heights[heightIndex],
                         float(timeIndex % 5u) * 1.7 - 3.0);
    float ratio = length(ModifyVertex(position, inst) - position) / magnitude;
    atomicMax(maxRatioBits, floatBitsToUint(ratio));
    atomicAdd(sampleCount, 1u);
}
)";

constexpr uint32_t kFoliageShapeSamples = 8u;
const char* const kFoliageWindPrologue = R"(#version 450
layout(local_size_x = 64) in;
#define HAS_UV6
#define HAS_UV7
vec2 aUV6;
vec2 aUV7;

#include "Includes/instance_sector.glsl"
struct InstanceData
{
    float deformationTimeSeconds;
    mat4 modelMatrix;
    ivec3 sector;
};
struct WeatherLightLanes
{
    vec4 uWeatherWindCount;
    vec4 uWeatherWind[7];
};
WeatherLightLanes Light;
struct FoliageProperties
{
    vec2 foliageWindHeight;
    vec2 foliageWindMotion;
    float foliageLeafPivots;
};
FoliageProperties Props;
#line 1
)";

const char* const kFoliageWindSampler = R"(
layout(std430, set = 0, binding = 0) buffer SampledBound
{
    uint maxRatioBits;
    uint sampleCount;
    uint flutterSamples;
    uint invalidSamples;
};

void main()
{
    uint windIndex = gl_GlobalInvocationID.y;
    uint shape = gl_GlobalInvocationID.z;
    float angle = float(windIndex % 8u) * (6.2831853 / 8.0);
    const float speeds[4] = float[4](0.0, 4.0, 10.0, 40.0);
    vec3 wind = vec3(cos(angle), 0.0, sin(angle)) * speeds[windIndex / 8u];
    // One global override volume with the production velocity, gust and mode lanes.
    Light.uWeatherWindCount = vec4(1.0, 0.0, 0.0, 0.0);
    Light.uWeatherWind[0] = vec4(0.0);
    Light.uWeatherWind[1] = vec4(1.0, 0.0, 0.0, 1.0);
    Light.uWeatherWind[2] = vec4(0.0, 1.0, 0.0, 1.0);
    Light.uWeatherWind[3] = vec4(0.0, 0.0, 1.0, 1.0);
    Light.uWeatherWind[4] = vec4(wind, 1.0);
    Light.uWeatherWind[5] = vec4(0.5, 0.53, 70.0, 1.0);
    Light.uWeatherWind[6] = vec4(0.0, 1.0, 1.0, 0.0);

    Props.foliageWindHeight = vec2(-2.0, 12.0);
    Props.foliageWindMotion = shape < 4u ? vec2(0.4, 0.0) : vec2(1.2, 0.18);
    Props.foliageLeafPivots = 1.0;

    InstanceData inst;
    inst.deformationTimeSeconds = float(gl_GlobalInvocationID.x) * 0.07371;
    inst.modelMatrix = mat4(1.0);
    if (shape >= 4u)
    {
        inst.modelMatrix[0] = vec4(0.0, 0.0, 2.0, 0.0);
        inst.modelMatrix[2] = vec4(-0.5, 0.0, 0.0, 0.0);
    }
    inst.modelMatrix[3] = vec4(8.0, 0.0, 32.0, 1.0);
    inst.sector = ivec3(0);
    const float heights[4] = float[4](-8.0, -2.0, 4.0, 16.0);
    vec3 position = vec3(2.0, heights[shape % 4u], -3.0);
    aUV6 = position.xy - vec2(0.7, 0.28);
    aUV7 = vec2(position.z + 0.6, 1.0);
    mat3 rotation;
    vec3 deformed = ModifyVertexFrame(position, inst, rotation);
    vec3 displacement = deformed - position;
    float ratio = length(displacement) / length(Props.foliageWindMotion);
    // The leaf stays rigid around the swaying hinge even at zero flutter amplitude.
    vec3 pivot = vec3(aUV6, aUV7.x);
    vec3 hinged = ModifyVertex(pivot, inst);
    vec3 swayedPivot = GE_ApplyFoliageSway(pivot, inst, Props.foliageWindHeight, Props.foliageWindMotion.x);
    vec3 reference = swayedPivot + rotation * (position - pivot);
    if (distance(hinged, swayedPivot) > 0.00001 ||
        distance(deformed, reference) > 0.00001 ||
        abs(distance(hinged, deformed) - distance(pivot, position)) > 0.00001)
        atomicAdd(invalidSamples, 1u);
    if (abs(displacement.y) > 0.00001)
    {
        atomicAdd(flutterSamples, 1u);
        if (shape < 4u || position.y <= Props.foliageWindHeight.x || windIndex < 8u)
            atomicAdd(invalidSamples, 1u);
    }
    atomicMax(maxRatioBits, floatBitsToUint(ratio));
    atomicAdd(sampleCount, 1u);
}
)";

struct SampledBound
{
    float MaxRatio = 0.0f;
    uint32_t Samples = 0;
    uint32_t FlutterSamples = 0;
    uint32_t InvalidSamples = 0;
    std::string SkipReason;
};

SampledBound SampleModifier(const fs::path& modifier, std::string source, uint32_t shapeSamples)
{
    SampledBound out{};
    if (!ShaderCompileService::IsCompilerAvailable())
    {
        out.SkipReason = "no shader compiler in this build";
        return out;
    }
    std::unique_ptr<IDevice> device = CreateVulkanDeviceFast();
    if (!device)
    {
        out.SkipReason = "no Vulkan device available";
        return out;
    }

    ShaderProgramCompileRequest req{};
    req.debugName = modifier.stem().string() + "_sampled_bound";
    req.baseDirectory = modifier.parent_path().parent_path();
    req.cacheRoot = fs::temp_directory_path() / "ge_modifier_envelope_sampled_cache";
    req.includeDirs = {kRepoRoot / "Engine" / "Modules" / "Rendering" / "Shaders"};
    ShaderStageCompileSpec cs{};
    cs.stage = "cs";
    cs.sourcePath = req.baseDirectory / (req.debugName + ".comp");
    cs.inlineSource = std::move(source);
    req.stages = {cs};
    ShaderProgramCompileResult compiled{};
    std::string error;
    EXPECT_TRUE(ShaderCompileService::CompileProgramToCache(req, ShaderSourceKind::SpirV, compiled, &error)) << error;
    const auto spirv = compiled.stageBytes.find("cs");
    if (spirv == compiled.stageBytes.end())
        return out;

    DescriptorSetLayoutDesc layout{};
    layout.debugName = "SampledDeformationBound.Layout";
    DescriptorBinding binding{};
    binding.binding = 0;
    binding.type = DescriptorType::StorageBuffer;
    binding.count = 1;
    binding.shaderStages = kShaderStageCompute;
    layout.bindings.push_back(binding);

    ComputePipelineDesc pipelineDesc{};
    pipelineDesc.ComputeShader = std::make_shared<const std::vector<uint8_t>>(spirv->second);
    pipelineDesc.DescriptorSetLayouts.push_back(device->InternDescriptorSetLayout(layout));
    pipelineDesc.DebugName = req.debugName;
    const PipelineHandle pipeline =
        device->GetOrCreateComputePipeline(device->InternComputePipeline(pipelineDesc));
    EXPECT_TRUE(pipeline.IsValid());

    BufferDesc resultDesc{};
    resultDesc.size = 16;
    resultDesc.usage = static_cast<uint32_t>(BufferUsage::Storage) | static_cast<uint32_t>(BufferUsage::TransferDst);
    resultDesc.memoryUsage = BufferMemoryUsage::Readback;
    resultDesc.debugName = "SampledDeformationBound.Result";
    const BufferHandle result = device->CreateBuffer(resultDesc);
    std::memset(device->MapBuffer(result), 0, resultDesc.size);
    device->UnmapBuffer(result);

    DescriptorSetDesc setDesc{};
    setDesc.layout = layout;
    setDesc.transient = true;
    setDesc.debugName = "EzTreeWindSampledBound.Set";
    const DescriptorSetHandle set = device->CreateDescriptorSet(setDesc);
    device->UpdateStorageBufferBinding(set, 0, result, 0, resultDesc.size);

    auto commands = device->CreateCommandList(IDevice::QueueType::Graphics);
    commands->Begin();
    commands->SetPipeline(pipeline);
    commands->BindDescriptorSet(0, set, pipeline);
    commands->Dispatch(kTimeSamples / 64u, kWindSamples, shapeSamples);
    commands->End();
    device->ExecuteCommandLists({commands.get()});
    device->WaitForIdle();

    uint32_t words[4]{};
    std::memcpy(words, device->MapBuffer(result), sizeof(words));
    device->UnmapBuffer(result);
    device->DestroyBuffer(result);
    device->Shutdown();
    out.MaxRatio = std::bit_cast<float>(words[0]);
    out.Samples = words[1];
    out.FlutterSamples = words[2];
    out.InvalidSamples = words[3];
    return out;
}

} // namespace

// Every shipped modifier that declares an envelope is sampled here. A new one
// fails this test until a sampler for it is added beside ez_tree_wind's.
TEST(VertexModifierEnvelopeSampled, EveryShippedDeclarationHasASampler)
{
    std::set<std::string> declaring;
    for (const char* tree : {"Engine/Modules", "Packages"})
    {
        std::error_code ec;
        for (fs::recursive_directory_iterator it(kRepoRoot / tree, fs::directory_options::skip_permission_denied, ec), end;
             it != end; it.increment(ec))
        {
            if (ec || !it->is_regular_file() || it->path().extension() != ".glsl")
                continue;
            const std::string relative = fs::relative(it->path(), kRepoRoot).generic_string();
            if (ReadDeclaration(ReadText(it->path()), relative).Declared)
                declaring.insert(relative);
        }
    }
    EXPECT_EQ(declaring, (std::set<std::string>{kEzTreeWind, kFoliageWind}));
}

TEST(VertexModifierEnvelopeSampled, EzTreeWindStaysInsideItsDeclaredEnvelope)
{
    const std::string modifierText = ReadText(kRepoRoot / kEzTreeWind);
    ASSERT_FALSE(modifierText.empty()) << "not found: " << (kRepoRoot / kEzTreeWind).string();
    const DeformationEnvelopeDeclaration declared = ReadDeclaration(modifierText, kEzTreeWind);
    ASSERT_TRUE(declared.Usable()) << declared.Refusal;
    ASSERT_EQ(declared.ScaledBy, "windStrength") << "the sampler divides by the wind vector's length";

    const SampledBound sampled = SampleModifier(kRepoRoot / kEzTreeWind,
        std::string(kEzTreeWindPrologue) + modifierText + kEzTreeWindSampler, kShapeSamples);
    if (!sampled.SkipReason.empty())
        GTEST_SKIP() << sampled.SkipReason;
    ASSERT_EQ(sampled.Samples, kTimeSamples * kWindSamples * kShapeSamples) << "every sample must run";
    ::testing::Test::RecordProperty("sampledMaxRatio", std::to_string(sampled.MaxRatio));
    // The grid must reach the large displacements, or a pass proves nothing:
    // every term peaks at height01 = 1 and a UV corner, which the grid visits.
    // The declaration adds the terms' largest lengths; the terms point along the
    // wind, across it and up, so the largest displacement the grid finds sits
    // well under the declared sum, but never under half of it.
    EXPECT_GT(sampled.MaxRatio, 0.5f * declared.Meters) << "the sampler never moved a vertex far";
    EXPECT_LE(sampled.MaxRatio, declared.Meters)
        << "ez_tree_wind.glsl moves a vertex " << sampled.MaxRatio << " x |windStrength.xyz|, past its declared "
        << "@deformationEnvelope " << declared.Meters << ": raise the declaration";
}

TEST(VertexModifierEnvelopeSampled, FoliageWindStaysInsideItsDeclaredEnvelope)
{
    const std::string modifierText = ReadText(kRepoRoot / kFoliageWind);
    ASSERT_FALSE(modifierText.empty());
    const auto declared = ReadDeclaration(modifierText, kFoliageWind);
    ASSERT_TRUE(declared.Usable()) << declared.Refusal;
    ASSERT_EQ(declared.ScaledBy, "foliageWindMotion");

    const auto sampled = SampleModifier(kRepoRoot / kFoliageWind,
        std::string(kFoliageWindPrologue) + modifierText + kFoliageWindSampler, kFoliageShapeSamples);
    if (!sampled.SkipReason.empty())
        GTEST_SKIP() << sampled.SkipReason;
    ASSERT_EQ(sampled.Samples, kTimeSamples * kWindSamples * kFoliageShapeSamples);
    EXPECT_GT(sampled.FlutterSamples, 0u);
    EXPECT_EQ(sampled.InvalidSamples, 0u);
    ::testing::Test::RecordProperty("sampledMaxRatio", std::to_string(sampled.MaxRatio));
    EXPECT_GT(sampled.MaxRatio, 0.99f);
    EXPECT_LE(sampled.MaxRatio, declared.Meters + 0.00001f);
}
