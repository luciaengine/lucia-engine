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

// foliage_wind.glsl reads its material properties from Props (base and height, sway and
// flutter meters, the pivot switch), the clock, the model matrix and the sector from
// InstanceData, the weather wind volumes from Light, and the leaf pivot and card radius
// from aUV6 and aUV7. One wind volume covers the world and sets the wind, so the modifier
// sees the full wind strength. One invocation is one sample: x walks the clock and the
// pivot's horizontal place, y the wind direction and speed, z the flutter and sway meters,
// their scale, the pivot offset, the card radius (exact, loose and malformed), the pivot
// height and the pivot switch. The result is the largest
// |displacement| / |foliageWindMotion| as float bits and the number of samples taken.
constexpr uint32_t kFoliageTimeSamples = 2048u;
constexpr uint32_t kFoliageWindSamples = 24u;
constexpr uint32_t kFoliageShapeSamples = 4u * 2u * 4u * 3u * 2u * 2u;

const char* const kFoliageWindPrologue = R"(#version 450
layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

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
struct FoliageWindProps
{
    vec2 foliageWindHeight;
    vec2 foliageWindMotion;
    float foliageLeafPivots;
};
FoliageWindProps Props;
vec2 aUV6;
vec2 aUV7;
)";

// The leaf pivot path compiles only where the mesh has UV6 and UV7.
const char* const kFoliageLeafPivotDefines = "#define HAS_UV6\n#define HAS_UV7\n";

const char* const kFoliageWindSampler = R"(
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
    const float speeds[3] = float[3](3.0, 10.0, 25.0);
    vec3 velocity = vec3(cos(azimuth), 0.0, sin(azimuth)) * speeds[windIndex / 8u];
    Light.uWeatherWindCount = vec4(1.0, 0.0, 0.0, 0.0);
    Light.uWeatherWind[0] = vec4(0.0, 0.0, 0.0, 1.0);
    Light.uWeatherWind[1] = vec4(1.0, 0.0, 0.0, 1.0);
    Light.uWeatherWind[2] = vec4(0.0, 1.0, 0.0, 1.0);
    Light.uWeatherWind[3] = vec4(0.0, 0.0, 1.0, 1.0);
    Light.uWeatherWind[4] = vec4(velocity, 1.0);
    Light.uWeatherWind[5] = vec4(0.0, 0.7, 70.0, 1.0);
    Light.uWeatherWind[6] = vec4(0.0, 1.0, 1.0, 0.0);

    const vec2 motions[4] = vec2[4](vec2(1.0, 0.0), vec2(0.0, 1.0), vec2(1.0, 1.0), vec2(0.3, 1.0));
    const float scales[2] = float[2](0.05, 1.0);
    const vec3 offsets[4] = vec3[4](vec3(0.0, 0.2, 0.0), vec3(0.2, 0.0, 0.0), vec3(0.12, 0.12, 0.05), vec3(0.0));
    const float radiusScales[3] = float[3](1.0, 2.0, 0.5);
    const float pivotHeights[2] = float[2](0.5, 1.5);
    vec2 motion = motions[shapeIndex % 4u] * scales[(shapeIndex / 4u) % 2u];
    vec3 offset = offsets[(shapeIndex / 8u) % 4u];
    float offsetLength = length(offset);
    float radius = offsetLength > 0.0 ? offsetLength * radiusScales[(shapeIndex / 32u) % 3u] : 0.1;
    Props.foliageWindHeight = vec2(0.0, 1.0);
    Props.foliageWindMotion = motion;
    Props.foliageLeafPivots = float(shapeIndex / 192u);

    vec3 pivot = vec3(float(timeIndex % 7u) * 1.3 - 4.0, pivotHeights[(shapeIndex / 96u) % 2u],
                      float(timeIndex % 5u) * 1.7 - 3.0);
    aUV6 = pivot.xy;
    aUV7 = vec2(pivot.z, radius);

    InstanceData inst;
    inst.deformationTimeSeconds = float(timeIndex) * 0.7371;
    inst.modelMatrix = mat4(1.0);
    inst.sector = ivec3(0);

    vec3 position = pivot + offset;
    float ratio = length(ModifyVertex(position, inst) - position) / length(motion);
    atomicMax(maxRatioBits, floatBitsToUint(ratio));
    atomicAdd(sampleCount, 1u);
}
)";

struct SampledBound
{
    float MaxRatio = 0.0f;
    uint32_t Samples = 0;
    std::string SkipReason;
};

// A sampler kernel: one compute shader whose invocations each write one displacement
// ratio into binding 0, and its dispatch size in workgroups.
struct SamplerKernel
{
    std::string DebugName;
    fs::path BaseDirectory;
    std::string Source;
    uint32_t GroupsX = 0;
    uint32_t GroupsY = 0;
    uint32_t GroupsZ = 0;
};

SampledBound RunSampler(const SamplerKernel& kernel)
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
    req.debugName = kernel.DebugName;
    req.baseDirectory = kernel.BaseDirectory;
    req.cacheRoot = fs::temp_directory_path() / "ge_modifier_envelope_sampled_cache";
    req.includeDirs = {kRepoRoot / "Engine" / "Modules" / "Rendering" / "Shaders"};
    ShaderStageCompileSpec cs{};
    cs.stage = "cs";
    cs.sourcePath = req.baseDirectory / (kernel.DebugName + ".comp");
    cs.inlineSource = kernel.Source;
    req.stages = {cs};
    ShaderProgramCompileResult compiled{};
    std::string error;
    EXPECT_TRUE(ShaderCompileService::CompileProgramToCache(req, ShaderSourceKind::SpirV, compiled, &error)) << error;
    const auto spirv = compiled.stageBytes.find("cs");
    if (spirv == compiled.stageBytes.end())
        return out;

    // The descriptors keep a pointer to their debug names, so the names live for the whole run.
    const std::string layoutName = kernel.DebugName + ".Layout";
    const std::string setName = kernel.DebugName + ".Set";
    const std::string resultName = kernel.DebugName + ".Result";

    DescriptorSetLayoutDesc layout{};
    layout.debugName = layoutName.c_str();
    DescriptorBinding binding{};
    binding.binding = 0;
    binding.type = DescriptorType::StorageBuffer;
    binding.count = 1;
    binding.shaderStages = kShaderStageCompute;
    layout.bindings.push_back(binding);

    ComputePipelineDesc pipelineDesc{};
    pipelineDesc.ComputeShader = std::make_shared<const std::vector<uint8_t>>(spirv->second);
    pipelineDesc.DescriptorSetLayouts.push_back(device->InternDescriptorSetLayout(layout));
    pipelineDesc.DebugName = kernel.DebugName;
    const PipelineHandle pipeline =
        device->GetOrCreateComputePipeline(device->InternComputePipeline(pipelineDesc));
    EXPECT_TRUE(pipeline.IsValid());

    BufferDesc resultDesc{};
    resultDesc.size = 16;
    resultDesc.usage = static_cast<uint32_t>(BufferUsage::Storage) | static_cast<uint32_t>(BufferUsage::TransferDst);
    resultDesc.memoryUsage = BufferMemoryUsage::Readback;
    resultDesc.debugName = resultName.c_str();
    const BufferHandle result = device->CreateBuffer(resultDesc);
    std::memset(device->MapBuffer(result), 0, resultDesc.size);
    device->UnmapBuffer(result);

    DescriptorSetDesc setDesc{};
    setDesc.layout = layout;
    setDesc.transient = true;
    setDesc.debugName = setName.c_str();
    const DescriptorSetHandle set = device->CreateDescriptorSet(setDesc);
    device->UpdateStorageBufferBinding(set, 0, result, 0, resultDesc.size);

    auto commands = device->CreateCommandList(IDevice::QueueType::Graphics);
    commands->Begin();
    commands->SetPipeline(pipeline);
    commands->BindDescriptorSet(0, set, pipeline);
    commands->Dispatch(kernel.GroupsX, kernel.GroupsY, kernel.GroupsZ);
    commands->End();
    device->ExecuteCommandLists({commands.get()});
    device->WaitForIdle();

    uint32_t words[2]{};
    std::memcpy(words, device->MapBuffer(result), sizeof(words));
    device->UnmapBuffer(result);
    device->DestroyBuffer(result);
    device->Shutdown();
    out.MaxRatio = std::bit_cast<float>(words[0]);
    out.Samples = words[1];
    return out;
}

SampledBound SampleEzTreeWind(const std::string& modifierText)
{
    return RunSampler({"ez_tree_wind_sampled_bound", kRepoRoot / "Packages" / "eztree" / "Assets" / "Shaders",
                       std::string(kEzTreeWindPrologue) + modifierText + kEzTreeWindSampler, kTimeSamples / 64u,
                       kWindSamples, kShapeSamples});
}

// leafPivots compiles the UV6 and UV7 path (sway, or authored pivots with flutter);
// without it the modifier compiles the sway-only path.
SampledBound SampleFoliageWind(const std::string& modifierText, bool leafPivots)
{
    const std::string defines = leafPivots ? kFoliageLeafPivotDefines : "";
    return RunSampler({leafPivots ? "foliage_wind_pivots_sampled_bound" : "foliage_wind_sway_sampled_bound",
                       kRepoRoot / "Engine" / "Modules" / "Rendering" / "Shaders",
                       std::string(kFoliageWindPrologue) + defines + "#line 1\n" + modifierText + kFoliageWindSampler,
                       kFoliageTimeSamples / 64u, kFoliageWindSamples, kFoliageShapeSamples});
}

} // namespace

// Every shipped modifier that declares an envelope is sampled here. A new one
// fails this test until a sampler for it is added beside the others.
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

    const SampledBound sampled = SampleEzTreeWind(modifierText);
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

// Sway moves a vertex at most foliageWindMotion.x along the wind. Flutter turns the
// leaf around its pivot by an angle limited to motion.y / radius, so the chord is at
// most motion.y. The sum x + y is at most 1.414214 x |foliageWindMotion|.
static void ExpectFoliageWindInsideItsDeclaredEnvelope(bool leafPivots)
{
    const std::string modifierText = ReadText(kRepoRoot / kFoliageWind);
    ASSERT_FALSE(modifierText.empty()) << "not found: " << (kRepoRoot / kFoliageWind).string();
    const DeformationEnvelopeDeclaration declared = ReadDeclaration(modifierText, kFoliageWind);
    ASSERT_TRUE(declared.Usable()) << declared.Refusal;
    ASSERT_EQ(declared.ScaledBy, "foliageWindMotion") << "the sampler divides by the motion vector's length";

    const SampledBound sampled = SampleFoliageWind(modifierText, leafPivots);
    if (!sampled.SkipReason.empty())
        GTEST_SKIP() << sampled.SkipReason;
    ASSERT_EQ(sampled.Samples, kFoliageTimeSamples * kFoliageWindSamples * kFoliageShapeSamples)
        << "every sample must run";
    ::testing::Test::RecordProperty("sampledMaxRatio", std::to_string(sampled.MaxRatio));
    // Sway alone reaches a ratio near 1 at full wind and a gust peak, so a pass is
    // only proof if the grid gets past half of the declaration.
    EXPECT_GT(sampled.MaxRatio, 0.5f * declared.Meters) << "the sampler never moved a vertex far";
    EXPECT_LE(sampled.MaxRatio, declared.Meters)
        << "foliage_wind.glsl moves a vertex " << sampled.MaxRatio << " x |foliageWindMotion|, past its declared "
        << "@deformationEnvelope " << declared.Meters << ": raise the declaration";
}

TEST(VertexModifierEnvelopeSampled, FoliageWindSwayStaysInsideItsDeclaredEnvelope)
{
    ExpectFoliageWindInsideItsDeclaredEnvelope(false);
}

TEST(VertexModifierEnvelopeSampled, FoliageWindLeafFlutterStaysInsideItsDeclaredEnvelope)
{
    ExpectFoliageWindInsideItsDeclaredEnvelope(true);
}
