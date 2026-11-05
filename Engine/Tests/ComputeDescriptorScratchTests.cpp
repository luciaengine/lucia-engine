#include "Engine/Rendering/Pipeline/Nodes/ComputeDescriptorScratch.h"
#include "Memory/AllocationCountScope.h"

#include <cstdint>

#include <gtest/gtest.h>

using namespace GameEngine;
using namespace GameEngine::Rendering;
using GameEngine::Engine::Renderer::Pipeline::Nodes::Detail::ComputeDescriptorScratch;

namespace
{
bool FillBatch(ComputeDescriptorScratch& scratch, uint64_t resource)
{
    scratch.Reset();
    auto& buffer = scratch.Add(7, DescriptorType::StorageBuffer);
    buffer.buffers = {BufferHandle(resource)};
    buffer.bufferOffsets = {256};
    buffer.bufferRanges = {128};
    auto& texture = scratch.Add(4096, DescriptorType::CombinedImageSampler);
    texture.textures = {TextureHandle(resource + 1)};
    texture.samplers = {SamplerHandle(resource + 2)};
    return scratch.ContainsBinding(7) && scratch.ContainsBinding(4096) && !scratch.ContainsBinding(19);
}
} // namespace

TEST(ComputeDescriptorScratchTests, ReusedSlotsDiscardPreviousDescriptorState)
{
    ComputeDescriptorScratch scratch;
    auto& previous = scratch.Add(4096, DescriptorType::AccelerationStructure);
    previous.arrayElement = 3;
    previous.buffers = {BufferHandle(11)};
    previous.textures = {TextureHandle(12)};
    previous.textureViews = {TextureViewHandle(13)};
    previous.samplers = {SamplerHandle(14)};
    previous.bufferOffsets = {256};
    previous.bufferRanges = {128};
    previous.accelerationStructures = {TlasSlotHandle(15)};

    scratch.Reset();
    auto& current = scratch.Add(7, DescriptorType::StorageImage);
    EXPECT_EQ(current.binding, 7u);
    EXPECT_EQ(current.type, DescriptorType::StorageImage);
    EXPECT_EQ(current.arrayElement, 0u);
    EXPECT_TRUE(current.buffers.empty());
    EXPECT_TRUE(current.textures.empty());
    EXPECT_TRUE(current.textureViews.empty());
    EXPECT_TRUE(current.samplers.empty());
    EXPECT_TRUE(current.bufferOffsets.empty());
    EXPECT_TRUE(current.bufferRanges.empty());
    EXPECT_TRUE(current.accelerationStructures.empty());
}

TEST(ComputeDescriptorScratchTests, ShrinkingBatchDoesNotExposeOldBindings)
{
    ComputeDescriptorScratch scratch;
    FillBatch(scratch, 11);
    EXPECT_TRUE(scratch.ContainsBinding(4096));
    scratch.Reset();
    EXPECT_TRUE(scratch.Updates().empty());
    EXPECT_FALSE(scratch.ContainsBinding(7));
    auto& update = scratch.Add(17, DescriptorType::Sampler);
    update.samplers = {SamplerHandle(23)};
    ASSERT_EQ(scratch.Updates().size(), 1u);
    EXPECT_FALSE(scratch.ContainsBinding(4096));
    EXPECT_FALSE(scratch.ContainsBinding(7));
    EXPECT_TRUE(scratch.ContainsBinding(17));
}

TEST(ComputeDescriptorScratchTests, IndependentRecordingScratchKeepsItsOwnResources)
{
    ComputeDescriptorScratch first;
    ComputeDescriptorScratch second;
    FillBatch(first, 11);
    FillBatch(second, 21);
    FillBatch(second, 31);
    ASSERT_EQ(first.Updates().size(), 2u);
    ASSERT_EQ(second.Updates().size(), 2u);
    EXPECT_EQ(first.Updates()[0].buffers[0], BufferHandle(11));
    EXPECT_EQ(first.Updates()[1].textures[0], TextureHandle(12));
    EXPECT_EQ(second.Updates()[0].buffers[0], BufferHandle(31));
}

TEST(ComputeDescriptorScratchTests, MembershipQueriesPreserveDuplicateWriteOrder)
{
    ComputeDescriptorScratch scratch;
    scratch.Add(4096, DescriptorType::StorageBuffer).buffers = {BufferHandle(11)};
    scratch.Add(7, DescriptorType::StorageBuffer).buffers = {BufferHandle(12)};
    scratch.Add(7, DescriptorType::StorageBuffer).buffers = {BufferHandle(13)};
    EXPECT_TRUE(scratch.ContainsBinding(7));
    scratch.Add(19, DescriptorType::StorageBuffer).buffers = {BufferHandle(14)};
    EXPECT_TRUE(scratch.ContainsBinding(19));
    const auto updates = scratch.Updates();
    ASSERT_EQ(updates.size(), 4u);
    EXPECT_EQ(updates[0].binding, 4096u);
    EXPECT_EQ(updates[1].buffers[0], BufferHandle(12));
    EXPECT_EQ(updates[2].buffers[0], BufferHandle(13));
}

#if GE_DESCRIPTOR_ALLOCATION_COUNTING
TEST(ComputeDescriptorScratchTests, WarmBatchesReuseOuterAndResourceArrayStorage)
{
    ComputeDescriptorScratch scratch;
    uint64_t coldAllocations = 0;
    {
        Memory::AllocationCountScope allocations(Memory::CountWindow::ThisThread);
        FillBatch(scratch, 11);
        coldAllocations = allocations.Count();
    }
    ASSERT_GT(coldAllocations, 0u) << "Allocation counter did not observe cold descriptor storage";

    uint64_t warmAllocations = 0;
    bool validBindings = true;
    {
        Memory::AllocationCountScope allocations(Memory::CountWindow::ThisThread);
        for (uint64_t resource = 21; resource < 85; ++resource)
            validBindings = FillBatch(scratch, resource) && validBindings;
        warmAllocations = allocations.Count();
    }
    EXPECT_EQ(warmAllocations, 0u);
    EXPECT_TRUE(validBindings);
    ASSERT_EQ(scratch.Updates().size(), 2u);
    EXPECT_EQ(scratch.Updates()[0].buffers[0], BufferHandle(84));
    EXPECT_EQ(scratch.Updates()[1].textures[0], TextureHandle(85));
}
#endif
