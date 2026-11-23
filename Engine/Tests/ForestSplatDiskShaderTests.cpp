// The splat disk of a packed forest tree on a headless device. A compute probe compiled against the
// shipped Shaders/Includes tree runs GE_ForestSplatDiskFor (forest_splat_disk.glsl) on crafted cells
// and the test reads back the disk normal and growth. Skips without a shader compiler or a device.

#include <gtest/gtest.h>

#include "Mathematics/Vector3.h"
#include "Mathematics/Vector4.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/Pipeline/PipelineTypes.h"
#include "Rendering/Materials/Shader/ShaderCompileService.h"

#include "TestDeviceHelper.h"
#include "TestTempDir.h"

#include <cmath>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#ifndef GE_RENDERER_REPO_ROOT
#error "GE_RENDERER_REPO_ROOT must be defined by CMake (source anchoring for shader contracts)"
#endif

using namespace GameEngine;
using namespace GameEngine::Rendering;
using Mathematics::Vector3;
using Mathematics::Vector4;

namespace
{
namespace fs = std::filesystem;

constexpr float kTolerance = 1e-4f;
// Must match kForestSplatOverheadGrowth in forest_splat_disk.glsl.
constexpr float kOverheadGrowth = 1.15f;

const char* kProbeComputeSource = R"(#version 450
layout(local_size_x = 1) in;

#include "forest_splat_disk.glsl"

struct ProbeCell
{
    vec4 normal;   // xyz: the baked disk normal.
    vec4 viewAxis; // xyz: the view axis.
    vec4 shape;    // x: bend, y: leaf fraction.
};

layout(std430, set = 0, binding = 0) readonly buffer Cells { ProbeCell cells[]; };
layout(std430, set = 0, binding = 1) writeonly buffer Disks { vec4 disks[]; };

void main()
{
    const ProbeCell cell = cells[gl_GlobalInvocationID.x];
    const GE_ForestSplatDisk disk =
        GE_ForestSplatDiskFor(cell.normal.xyz, cell.viewAxis.xyz, cell.shape.x, cell.shape.y);
    disks[gl_GlobalInvocationID.x] = vec4(disk.normal, disk.growth);
}
)";

// The std430 layout of ProbeCell.
struct ProbeCell
{
    Vector4 Normal;
    Vector4 ViewAxis;
    Vector4 Shape;
};
static_assert(sizeof(ProbeCell) == 48);

struct SplatDisk
{
    Vector3 Normal;
    float Growth = 0.0f;
};

ProbeCell MakeCell(const Vector3& normal, const Vector3& viewAxis, float bend, float leafFraction)
{
    return {Vector4(normal.Normalize(), 0.0f), Vector4(viewAxis.Normalize(), 0.0f),
            Vector4(bend, leafFraction, 0.0f, 0.0f)};
}

class ForestSplatDiskShaderTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        if (!ShaderCompileService::IsCompilerAvailable())
            GTEST_SKIP() << "No shader compiler in this build";
        m_Device = CreateVulkanDeviceFast();
        if (!m_Device)
            GTEST_SKIP() << "No Vulkan device available";

        std::vector<uint8_t> computeSpv;
        std::string error;
        ASSERT_TRUE(CompileProbe(computeSpv, error)) << "probe compile failed: " << error;

        m_Layout.debugName = "ForestSplatDiskProbe_SetLayout";
        for (uint32_t binding = 0; binding < 2u; ++binding)
        {
            DescriptorBinding b{};
            b.binding = binding;
            b.type = DescriptorType::StorageBuffer;
            b.count = 1u;
            b.shaderStages = kShaderStageCompute;
            m_Layout.bindings.push_back(b);
        }
        ComputePipelineDesc desc{};
        desc.ComputeShader = std::make_shared<const std::vector<uint8_t>>(std::move(computeSpv));
        desc.DescriptorSetLayouts.push_back(m_Device->InternDescriptorSetLayout(m_Layout));
        desc.DebugName = "ForestSplatDiskProbe";
        m_Pipeline = m_Device->GetOrCreateComputePipeline(m_Device->InternComputePipeline(desc));
        ASSERT_TRUE(m_Pipeline.IsValid());
    }

    void TearDown() override
    {
        for (BufferHandle& buffer : m_Buffers)
            if (buffer.IsValid())
                m_Device->DestroyBuffer(buffer);
        if (m_Device)
            m_Device->Shutdown();
    }

    static bool CompileProbe(std::vector<uint8_t>& outSpv, std::string& outError)
    {
        const fs::path shaderRoot =
            fs::path(GE_RENDERER_REPO_ROOT) / "Engine" / "Modules" / "Rendering" / "Shaders";
        const TestUtils::ScopedTempDir cacheDirectory{
            TestUtils::MakeUniqueTempDirectory("ge_forest_splat_disk_probe_cache")};
        ShaderProgramCompileRequest request{};
        request.debugName = "ForestSplatDiskProbe";
        request.baseDirectory = shaderRoot;
        request.cacheRoot = cacheDirectory.Path();
        request.includeDirs = {shaderRoot / "Includes"};
        ShaderStageCompileSpec compute{};
        compute.stage = "cs";
        compute.sourcePath = shaderRoot / "forest_splat_disk_probe.comp";
        compute.inlineSource = kProbeComputeSource;
        request.stages = {compute};

        ShaderProgramCompileResult result{};
        if (!ShaderCompileService::CompileProgramToCache(request, ShaderSourceKind::SpirV, result, &outError))
            return false;
        const auto it = result.stageBytes.find("cs");
        if (it == result.stageBytes.end())
        {
            outError = "probe program produced no compute stage";
            return false;
        }
        outSpv = it->second;
        return true;
    }

    BufferHandle MakeHostBuffer(size_t bytes, const char* name)
    {
        BufferDesc desc{};
        desc.size = bytes;
        desc.usage = static_cast<uint32_t>(BufferUsage::Storage) | static_cast<uint32_t>(BufferUsage::TransferDst);
        desc.memoryUsage = BufferMemoryUsage::Upload;
        desc.debugName = name;
        const BufferHandle buffer = m_Device->CreateBuffer(desc);
        m_Buffers.push_back(buffer);
        return buffer;
    }

    // Runs the probe on `cells` and returns one disk per cell.
    std::vector<SplatDisk> Run(const std::vector<ProbeCell>& cells)
    {
        const uint32_t count = static_cast<uint32_t>(cells.size());
        const size_t cellBytes = count * sizeof(ProbeCell);
        const size_t diskBytes = count * sizeof(Vector4);
        const BufferHandle cellBuffer = MakeHostBuffer(cellBytes, "ForestSplatDiskProbe.Cells");
        const BufferHandle diskBuffer = MakeHostBuffer(diskBytes, "ForestSplatDiskProbe.Disks");
        void* mappedCells = m_Device->MapBuffer(cellBuffer);
        void* mappedDisks = m_Device->MapBuffer(diskBuffer);
        if (!mappedCells || !mappedDisks)
        {
            ADD_FAILURE() << "probe buffer map failed";
            return {};
        }
        std::memcpy(mappedCells, cells.data(), cellBytes);
        m_Device->UnmapBuffer(cellBuffer);
        // A NaN fill shows a cell the probe did not write.
        std::vector<Vector4> poison(count, Vector4(NAN, NAN, NAN, NAN));
        std::memcpy(mappedDisks, poison.data(), diskBytes);
        m_Device->UnmapBuffer(diskBuffer);

        DescriptorSetDesc setDesc{};
        setDesc.layout = m_Layout;
        setDesc.transient = true;
        setDesc.debugName = "ForestSplatDiskProbe.DS";
        const DescriptorSetHandle set = m_Device->CreateDescriptorSet(setDesc);
        m_Device->UpdateStorageBufferBinding(set, 0, cellBuffer, 0, cellBytes);
        m_Device->UpdateStorageBufferBinding(set, 1, diskBuffer, 0, diskBytes);

        auto commands = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
        commands->Begin();
        commands->SetPipeline(m_Pipeline);
        commands->BindDescriptorSet(0, set, m_Pipeline);
        commands->Dispatch(count, 1, 1);
        commands->End();
        m_Device->ExecuteCommandLists({commands.get()});
        m_Device->WaitForIdle();

        std::vector<Vector4> raw(count);
        mappedDisks = m_Device->MapBuffer(diskBuffer);
        if (!mappedDisks)
        {
            ADD_FAILURE() << "probe readback map failed";
            return {};
        }
        std::memcpy(raw.data(), mappedDisks, diskBytes);
        m_Device->UnmapBuffer(diskBuffer);

        std::vector<SplatDisk> disks(count);
        for (uint32_t i = 0; i < count; ++i)
            disks[i] = {Vector3(raw[i].x, raw[i].y, raw[i].z), raw[i].w};
        return disks;
    }

    SplatDisk RunOne(const ProbeCell& cell)
    {
        const std::vector<SplatDisk> disks = Run({cell});
        return disks.empty() ? SplatDisk{Vector3(NAN, NAN, NAN), NAN} : disks.front();
    }

    std::unique_ptr<IDevice> m_Device;
    DescriptorSetLayoutDesc m_Layout{};
    PipelineHandle m_Pipeline{};
    std::vector<BufferHandle> m_Buffers;
};

void ExpectNormalNear(const Vector3& actual, const Vector3& expected)
{
    EXPECT_NEAR(actual.x, expected.x, kTolerance);
    EXPECT_NEAR(actual.y, expected.y, kTolerance);
    EXPECT_NEAR(actual.z, expected.z, kTolerance);
}

const Vector3 kUpright(1.0f, 0.0f, 0.0f);
const Vector3 kStraightDown(0.0f, -1.0f, 0.0f);

} // namespace

// From straight above, a leaf disk turns fully onto the view axis and takes the full growth.
TEST_F(ForestSplatDiskShaderTest, AStraightDownViewTurnsALeafDiskOntoTheViewAxisAndGrowsIt)
{
    const SplatDisk disk = RunOne(MakeCell(kUpright, kStraightDown, 1.0f, 1.0f));
    EXPECT_NEAR(std::abs(Vector3::Dot(disk.Normal, kStraightDown)), 1.0f, kTolerance)
        << "normal (" << disk.Normal.x << ", " << disk.Normal.y << ", " << disk.Normal.z << ")";
    EXPECT_NEAR(disk.Growth, kOverheadGrowth, kTolerance);
}

// |view axis Y| = 0.5 is below the overhead start: the disk keeps its baked normal and size.
TEST_F(ForestSplatDiskShaderTest, AShallowViewKeepsTheBakedDisk)
{
    const SplatDisk disk = RunOne(MakeCell(kUpright, Vector3(0.8660254f, -0.5f, 0.0f), 1.0f, 1.0f));
    ExpectNormalNear(disk.Normal, kUpright);
    EXPECT_NEAR(disk.Growth, 1.0f, kTolerance);
}

TEST_F(ForestSplatDiskShaderTest, ABarkCellKeepsTheBakedDiskFromAbove)
{
    const SplatDisk disk = RunOne(MakeCell(kUpright, kStraightDown, 1.0f, 0.25f));
    ExpectNormalNear(disk.Normal, kUpright);
    EXPECT_NEAR(disk.Growth, 1.0f, kTolerance);
}

TEST_F(ForestSplatDiskShaderTest, AnUnbentCellKeepsTheBakedDiskFromAbove)
{
    const SplatDisk disk = RunOne(MakeCell(kUpright, kStraightDown, 0.0f, 1.0f));
    ExpectNormalNear(disk.Normal, kUpright);
    EXPECT_NEAR(disk.Growth, 1.0f, kTolerance);
}

// The turned normal stays on the side of the baked normal: a disk that leans up turns up, a disk
// that leans down turns down, so the baked lighting side does not flip.
TEST_F(ForestSplatDiskShaderTest, TheTurnedNormalFacesTheSameSideAsTheBakedNormal)
{
    const Vector3 leansUp = Vector3(1.0f, 0.3f, 0.0f).Normalize();
    const Vector3 leansDown = Vector3(1.0f, -0.3f, 0.0f).Normalize();
    const std::vector<SplatDisk> disks =
        Run({MakeCell(leansUp, kStraightDown, 1.0f, 1.0f), MakeCell(leansDown, kStraightDown, 1.0f, 1.0f)});
    ASSERT_EQ(disks.size(), 2u);
    ExpectNormalNear(disks[0].Normal, Vector3(0.0f, 1.0f, 0.0f));
    ExpectNormalNear(disks[1].Normal, Vector3(0.0f, -1.0f, 0.0f));
    EXPECT_GT(Vector3::Dot(disks[0].Normal, leansUp), 0.0f);
    EXPECT_GT(Vector3::Dot(disks[1].Normal, leansDown), 0.0f);
}
