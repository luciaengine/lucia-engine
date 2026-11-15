// GPU sort coverage for the sorted transparent path (transparency-scale S1/S2).
//
// Two layers:
//  1. CPU cross-checks on the 64-bit key packing (SortedTransparentKey.h) — the
//     monotonic float transform, back-to-front ordering, and the deterministic
//     index tiebreak that keeps equal-depth draws flicker-free (Risk 4 / F4).
//  2. A device-backed dispatch of the real sorted_transparent_sort.comp over a
//     crafted key/value array, asserting the GPU result is byte-identical to a
//     CPU std::sort of the same pairs — the "GPU sort matches the CPU sort"
//     ship-gate, exercised on real hardware.

#include <gtest/gtest.h>

#include "Engine/Rendering/SortedTransparentKey.h"
#include "Engine/Rendering/SortedTransparentRuns.h"

#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/GPUScene.h"
#include "Rendering/Core/Pipeline/PipelineTypes.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"

#include "TestDeviceHelper.h"

#include <algorithm>
#include <cstring>
#include <memory_resource>
#include <random>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Rendering;
namespace R = GameEngine::Engine::Renderer;

namespace
{
// Must match kCapacity in sorted_transparent_sort.comp /
// sorted_transparent_drain.comp (locked C++-side by
// kSortedTransparentSortCapacity).
constexpr uint32_t kCapacity = R::kSortedTransparentSortCapacity;

struct GpuKey
{
    uint32_t lo; // uvec2.x
    uint32_t hi; // uvec2.y
};
} // namespace

// --- CPU key-packing cross-checks -------------------------------------------

TEST(SortedTransparentKey, MonotonicFloatTransformOrdersLikeFloat)
{
    const float samples[] = {-1e30f, -100.0f, -1.5f, -0.0f, 0.0f, 0.5f, 1.0f, 100.0f, 1e30f};
    for (size_t i = 1; i < std::size(samples); ++i)
    {
        // Strictly increasing floats -> non-decreasing sortable uints.
        EXPECT_LE(R::FloatToSortableUint(samples[i - 1]), R::FloatToSortableUint(samples[i]))
            << "i=" << i;
    }
    // -0.0f orders immediately before +0.0f (a harmless, monotonically
    // consistent quirk of the transform), never after.
    EXPECT_LE(R::FloatToSortableUint(-0.0f), R::FloatToSortableUint(0.0f));
}

TEST(SortedTransparentKey, AscendingKeyOrderIsBackToFront)
{
    // Farther (larger viewDepth) must produce a SMALLER key so an ascending sort
    // draws it first (back-to-front).
    const auto near = R::MakeSortedTransparentKey(2.0f, 0, 0);
    const auto mid = R::MakeSortedTransparentKey(5.0f, 0, 0);
    const auto far = R::MakeSortedTransparentKey(9.0f, 0, 0);
    EXPECT_TRUE(R::SortedTransparentKeyLess(far, mid));
    EXPECT_TRUE(R::SortedTransparentKeyLess(mid, near));
}

TEST(SortedTransparentKey, EqualDepthBreaksByIndexDeterministically)
{
    const auto a = R::MakeSortedTransparentKey(4.0f, 0, 10);
    const auto b = R::MakeSortedTransparentKey(4.0f, 0, 20);
    // Same depth+priority -> lower index sorts first, and the order is total.
    EXPECT_TRUE(R::SortedTransparentKeyLess(a, b));
    EXPECT_FALSE(R::SortedTransparentKeyLess(b, a));
}

TEST(SortedTransparentKey, PriorityDominatesDepth)
{
    const auto lowPrioFar = R::MakeSortedTransparentKey(1000.0f, 0, 0);
    const auto highPrioNear = R::MakeSortedTransparentKey(0.1f, 1, 0);
    // Priority is the high field: priority 0 sorts entirely before priority 1
    // regardless of depth.
    EXPECT_TRUE(R::SortedTransparentKeyLess(lowPrioFar, highPrioNear));
}

// --- Device-backed sort ------------------------------------------------------

namespace
{
class GPUSortComputeTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Device = CreateVulkanDeviceFast();
        if (!m_Device)
            GTEST_SKIP() << "No Vulkan device available";

        ShaderPackage pkg{};
        std::string err;
        if (!LoadShaderPkg("Shaders/sorted_transparent_sort.shaderpkg", ShaderSourceKind::SpirV, pkg, &err))
            FAIL() << "sorted_transparent_sort.shaderpkg not available: " << err;
        auto itCs = pkg.stageBytes.find("cs");
        ASSERT_NE(itCs, pkg.stageBytes.end());

        m_Layout.debugName = "GPUSort_SetLayout";
        for (uint32_t i = 0; i < 2u; ++i)
        {
            DescriptorBinding b{};
            b.binding = i;
            b.type = DescriptorType::StorageBuffer;
            b.count = 1u;
            b.shaderStages = kShaderStageCompute;
            m_Layout.bindings.push_back(b);
        }
        ComputePipelineDesc cd{};
        cd.ComputeShader = std::make_shared<const std::vector<uint8_t>>(itCs->second);
        cd.DescriptorSetLayouts.push_back(m_Device->InternDescriptorSetLayout(m_Layout));
        cd.DebugName = "SortedTransparentSort";
        const auto id = m_Device->InternComputePipeline(cd);
        m_Pipeline = m_Device->GetOrCreateComputePipeline(id);
        ASSERT_TRUE(m_Pipeline.IsValid());
    }

    void TearDown() override
    {
        for (BufferHandle& b : m_Buffers)
            if (b.IsValid())
                m_Device->DestroyBuffer(b);
        if (m_Device)
            m_Device->Shutdown();
    }

    BufferHandle MakeHostBuffer(size_t bytes, const char* name)
    {
        BufferDesc d{};
        d.size = std::max<size_t>(bytes, 16);
        d.usage = static_cast<uint32_t>(BufferUsage::Storage) |
                  static_cast<uint32_t>(BufferUsage::TransferDst);
        d.memoryUsage = BufferMemoryUsage::Upload;
        d.debugName = name;
        BufferHandle h = m_Device->CreateBuffer(d);
        m_Buffers.push_back(h);
        return h;
    }

    std::unique_ptr<IDevice> m_Device;
    DescriptorSetLayoutDesc m_Layout{};
    PipelineHandle m_Pipeline{};
    std::vector<BufferHandle> m_Buffers;
};
} // namespace

TEST_F(GPUSortComputeTest, MatchesCpuSortBackToFront)
{
    // 700 "live" transparents at pseudo-random depths, plus padding to capacity.
    // Live count deliberately exceeds the old 256 ceiling to prove the GPU path
    // has no such limit.
    constexpr uint32_t kLive = 700u;
    std::mt19937 rng(12345);
    std::uniform_real_distribution<float> depthDist(0.5f, 5000.0f);

    std::vector<GpuKey> keys(kCapacity);
    std::vector<uint32_t> values(kCapacity);

    // Reference pairs for the CPU sort.
    std::vector<std::pair<R::SortedTransparentKey, uint32_t>> ref;
    ref.reserve(kLive);

    for (uint32_t i = 0; i < kCapacity; ++i)
    {
        if (i < kLive)
        {
            const float depth = depthDist(rng);
            const R::SortedTransparentKey k =
                R::MakeSortedTransparentKey(depth, 0, static_cast<uint16_t>(i & 0xFFFFu));
            keys[i] = {k.Lo, k.Hi};
            values[i] = i;
            ref.push_back({k, i});
        }
        else
        {
            keys[i] = {0xFFFFFFFFu, 0xFFFFFFFFu}; // padding sorts to the end
            values[i] = 0xFFFFFFFFu;
        }
    }

    std::stable_sort(ref.begin(), ref.end(), [](const auto& a, const auto& b)
                     { return R::SortedTransparentKeyLess(a.first, b.first); });

    BufferHandle keyBuf = MakeHostBuffer(kCapacity * sizeof(GpuKey), "Sort.Keys");
    BufferHandle valBuf = MakeHostBuffer(kCapacity * sizeof(uint32_t), "Sort.Values");
    {
        void* m = m_Device->MapBuffer(keyBuf);
        ASSERT_NE(m, nullptr);
        std::memcpy(m, keys.data(), kCapacity * sizeof(GpuKey));
        m_Device->UnmapBuffer(keyBuf);
        m = m_Device->MapBuffer(valBuf);
        ASSERT_NE(m, nullptr);
        std::memcpy(m, values.data(), kCapacity * sizeof(uint32_t));
        m_Device->UnmapBuffer(valBuf);
    }

    DescriptorSetDesc setDesc{};
    setDesc.layout = m_Layout;
    setDesc.transient = true;
    setDesc.debugName = "Sort.DS";
    DescriptorSetHandle ds = m_Device->CreateDescriptorSet(setDesc);
    m_Device->UpdateStorageBufferBinding(ds, 0, keyBuf, 0, kCapacity * sizeof(GpuKey));
    m_Device->UpdateStorageBufferBinding(ds, 1, valBuf, 0, kCapacity * sizeof(uint32_t));

    auto cl = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
    cl->Begin();
    cl->SetPipeline(m_Pipeline);
    cl->BindDescriptorSet(0, ds, m_Pipeline);
    cl->Dispatch(1, 1, 1); // single-workgroup bitonic
    cl->End();
    m_Device->ExecuteCommandLists({cl.get()});
    m_Device->WaitForIdle();

    std::vector<GpuKey> outKeys(kCapacity);
    std::vector<uint32_t> outVals(kCapacity);
    {
        void* m = m_Device->MapBuffer(keyBuf);
        ASSERT_NE(m, nullptr);
        std::memcpy(outKeys.data(), m, kCapacity * sizeof(GpuKey));
        m_Device->UnmapBuffer(keyBuf);
        m = m_Device->MapBuffer(valBuf);
        ASSERT_NE(m, nullptr);
        std::memcpy(outVals.data(), m, kCapacity * sizeof(uint32_t));
        m_Device->UnmapBuffer(valBuf);
    }

    // Keys must be globally non-decreasing.
    for (uint32_t i = 1; i < kCapacity; ++i)
    {
        const R::SortedTransparentKey a{outKeys[i - 1].lo, outKeys[i - 1].hi};
        const R::SortedTransparentKey b{outKeys[i].lo, outKeys[i].hi};
        ASSERT_FALSE(R::SortedTransparentKeyLess(b, a)) << "keys not ascending at " << i;
    }

    // The first kLive entries must equal the CPU sort element-by-element (keys
    // are distinct via the index tiebreak, so the order is total).
    for (uint32_t i = 0; i < kLive; ++i)
    {
        EXPECT_EQ(outKeys[i].lo, ref[i].first.Lo) << "lo mismatch at " << i;
        EXPECT_EQ(outKeys[i].hi, ref[i].first.Hi) << "hi mismatch at " << i;
        EXPECT_EQ(outVals[i], ref[i].second) << "value/payload mismatch at " << i;
    }
    // Padding stays at the tail.
    for (uint32_t i = kLive; i < kCapacity; ++i)
    {
        EXPECT_EQ(outKeys[i].lo, 0xFFFFFFFFu);
        EXPECT_EQ(outKeys[i].hi, 0xFFFFFFFFu);
    }
}
