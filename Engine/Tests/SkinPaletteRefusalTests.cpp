// What a palette producer must do when the atlas REFUSES an allocation.
//
// The bone-palette ring refuses rather than wrapping, and grows only on the
// next visit to the slot that overflowed. A producer that ignores the refusal
// leaves the runtime's CURRENT-frame offset naming last frame's slot — which
// the atlas has since handed to a different runtime. RenderExtractionSystem
// copies that offset straight into GPUInstance::skinPaletteOffset, so the
// entity is skinned with another entity's bones: a visibly wrong pose, not a
// dropped frame.
//
// Atlas slot 0 is the identity block SkinPaletteAtlas::BeginFrame reserves, so
// retiring the offset to it renders bind pose for the cycle the ring takes to
// grow. The first test pins that slot 0 really is that block; the second pins
// the producer behaviour that depends on it.

#include "Components/Animation/SkeletonRef.h"
#include "Components/Animation/AnimatorRef.h"
#include "Assets/AnimationClip.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECS/World.h"
#include "ECSModules/Rendering/SkeletonStore.h"
#include "ECSModules/Rendering/ClipStore.h"
#include "ECSModules/Rendering/Systems/AnimationSystem.h"
#include "ECSModules/Rendering/Systems/SkinningUploadSystem.h"
#include "Engine/Rendering/BonePaletteLayout.h"
#include "Engine/Rendering/PerFrameWritePool.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/SkinPaletteAtlas.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Common/Utils.h"
#include "Rendering/Materials/ShaderCompileService.h"
#include "StagedTestPaths.h"
#include "TestTempDir.h"
#include "TestDeviceHelper.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <vector>

#include <gtest/gtest.h>

using namespace GameEngine;
using namespace GameEngine::Engine::Renderer;
using namespace GameEngine::Rendering;


namespace
{
constexpr uint32_t kTestJointCount = 4;
constexpr float32 kDt = 1.0f / 60.0f;

// BeginFrame's reservation, and therefore the base every caller allocation of
// the frame sits above.
constexpr size_t kIdentityBlockBytes =
    static_cast<size_t>(SkinPaletteAtlas::kIdentityBoneCount)
    * BonePaletteLayout::kFloatsPerBone * sizeof(float);

constexpr size_t kOnePaletteBytes =
    static_cast<size_t>(kTestJointCount)
    * BonePaletteLayout::kFloatsPerBone * sizeof(float);
} // namespace

class SkinPaletteRefusalTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
#ifdef __APPLE__
        DeviceDesc description;
        description.preferredAPI = GraphicsAPI::Metal;
        description.enableDebugLayer = false;
        m_Device = DeviceFactory::CreateDevice(description);
        if (!m_Device || !m_Device->Initialize(description))
            m_Device.reset();
#else
        m_Device = CreateVulkanDeviceFast();
#endif
        if (!m_Device)
            GTEST_SKIP() << "No graphics device available";
        RecordProperty("graphics_api", m_Device->GetAPI() == GraphicsAPI::Metal ? "Metal" : "Vulkan");
        ASSERT_TRUE(m_RenderServices.Initialize(m_Device.get()));

        auto& store = SkeletonStore::Instance();
        m_SkeletonId = store.CreateSkeleton(kTestJointCount);
        ASSERT_NE(m_SkeletonId, 0u);
        SkeletonData* skeleton = store.Get(m_SkeletonId);
        ASSERT_NE(skeleton, nullptr);
        skeleton->SkinJointCount = kTestJointCount;
        skeleton->JointNodes = {0, 1, 2, 3};
    }

    void TearDown() override
    {
        if (m_FrameOpen)
        {
            m_Device->FinalizeFrame();
            m_Device->WaitForIdle();
        }
        // One reference per CreateRuntime. This world has no render hooks
        // registered, so entity destruction releases nothing.
        for (uint32_t runtimeId : m_Runtimes)
            SkeletonStore::Instance().ReleaseRuntime(runtimeId);
        m_RenderServices.Shutdown();
        if (m_Device)
            m_Device->Shutdown();
    }

    // Re-provision the pool so the bone-palette ring holds the identity block
    // plus exactly `paletteSlots` caller palettes. Growth is off: a refusal
    // must stay one, so what is measured is the refusal path rather than the
    // ring's recovery from it.
    void SetBonePaletteCapacity(size_t paletteSlots)
    {
        PerFrameWritePoolConfig config{};
        auto& bonePalette = config.usages[static_cast<size_t>(FrameWriteUsage::BonePalette)];
        bonePalette.capacityBytes = kIdentityBlockBytes + paletteSlots * kOnePaletteBytes;
        bonePalette.maxCapacityBytes = 0;

        PerFrameWritePool& pool = m_RenderServices.GetPerFrameWritePool();
        pool.Shutdown();
        ASSERT_TRUE(pool.Initialize(m_Device.get(), config));
    }

    // The render loop's per-frame prologue for the palette path
    // (RenderingLoop.cpp): pool slot, identity block, then the rollover that
    // ages last frame's offsets — all before any producer runs.
    void BeginPaletteFrame(uint32_t frameIndex)
    {
        m_RenderServices.GetPerFrameWritePool().BeginFrame(frameIndex);
        m_RenderServices.GetSkinPaletteAtlas().BeginFrame(
            m_RenderServices.GetPerFrameWritePool());
        SkeletonStore::Instance().RollPaletteOffsets();
    }

    uint32_t MakeRuntime()
    {
        const uint32_t runtimeId = SkeletonStore::Instance().CreateRuntime(m_SkeletonId);
        if (runtimeId != 0)
            m_Runtimes.push_back(runtimeId);
        return runtimeId;
    }

    std::unique_ptr<IDevice> m_Device;
    RenderServices m_RenderServices;
    uint32_t m_SkeletonId = 0;
    std::vector<uint32_t> m_Runtimes;
    bool m_FrameOpen = false;
};

// The premise the refusal fix rests on: offset 0 names the identity block and
// nothing else, and that block holds identity rows.
TEST_F(SkinPaletteRefusalTest, IdentityBlockOwnsAtlasSlotZero)
{
    SetBonePaletteCapacity(4);
    BeginPaletteFrame(0);

    SkinPaletteAtlas& atlas = m_RenderServices.GetSkinPaletteAtlas();

    // A caller palette that is emphatically NOT identity, so the readback below
    // cannot pass by reading this instead of the reserved block.
    std::vector<float> palette(kTestJointCount * 16u, 0.0f);
    for (uint32_t bone = 0; bone < kTestJointCount; ++bone)
    {
        const size_t base = static_cast<size_t>(bone) * 16u;
        palette[base + 0] = 2.0f;
        palette[base + 5] = 3.0f;
        palette[base + 10] = 4.0f;
        palette[base + 15] = 1.0f;
    }

    const PaletteAllocation alloc = atlas.Upload(palette.data(), kTestJointCount);
    ASSERT_TRUE(alloc.valid);
    EXPECT_EQ(alloc.OffsetInBones(), SkinPaletteAtlas::kIdentityBoneCount)
        << "the frame's first caller allocation must land ABOVE the reserved "
           "identity block, or offset 0 is not that block";

    const auto* rows = static_cast<const float*>(m_Device->MapBuffer(atlas.GetBuffer()));
    ASSERT_NE(rows, nullptr);

    // Identity packed to 3 rows (bone_palette.glsl): (1,0,0,0) (0,1,0,0) (0,0,1,0).
    // Blended with weights that sum to 1 this leaves position and normal
    // untouched — i.e. bind pose.
    static constexpr float kIdentityRows[BonePaletteLayout::kFloatsPerBone] = {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f};

    uint32_t firstBadBone = SkinPaletteAtlas::kIdentityBoneCount;
    for (uint32_t bone = 0; bone < SkinPaletteAtlas::kIdentityBoneCount; ++bone)
    {
        const float* boneRows = rows + static_cast<size_t>(bone) * BonePaletteLayout::kFloatsPerBone;
        if (!std::equal(kIdentityRows, kIdentityRows + BonePaletteLayout::kFloatsPerBone, boneRows))
        {
            firstBadBone = bone;
            break;
        }
    }
    EXPECT_EQ(firstBadBone, SkinPaletteAtlas::kIdentityBoneCount)
        << "bone " << firstBadBone << " of the identity block is not identity; an "
           "instance pointed at slot 0 would not render bind pose";
}

// A producer whose upload is refused must retire the runtime's offset to the
// identity block. Leaving it stale points the entity at the slot the atlas has
// just handed to the newcomer that displaced it.
TEST_F(SkinPaletteRefusalTest, RefusedUploadRetiresOffsetToIdentityBlock)
{
    // Room for the identity block and exactly one caller palette.
    SetBonePaletteCapacity(1);

    const uint32_t runtimeA = MakeRuntime();
    const uint32_t runtimeB = MakeRuntime();
    ASSERT_NE(runtimeA, 0u);
    ASSERT_NE(runtimeB, 0u);
    ASSERT_NE(runtimeA, runtimeB);

    // SkinningUploadSystem walks runtime ids ascending, so the lower id wins the
    // single slot. Which id that is depends on the store's free list, not on the
    // order these were created in.
    const uint32_t winner = (std::min)(runtimeA, runtimeB);
    const uint32_t displaced = (std::max)(runtimeA, runtimeB);

    ECS::World world(nullptr);
    SkinningUploadSystem uploadSystem(&m_RenderServices);
    SkeletonStore& store = SkeletonStore::Instance();

    // Frame 1: only one character in the scene, so it settles on the slot.
    ECS::Entity displacedEntity = world.Create();
    displacedEntity.Set(Components::SkeletonRef{m_SkeletonId, displaced});
    world.ProcessCommands();

    BeginPaletteFrame(0);
    uploadSystem.Update(world, kDt);

    ASSERT_NE(store.GetRuntime(displaced), nullptr);
    const uint32_t settledOffset = store.GetRuntime(displaced)->AtlasPaletteOffsetBones;
    ASSERT_EQ(settledOffset, SkinPaletteAtlas::kIdentityBoneCount)
        << "premise: with room for one palette, the only character in the scene "
           "gets the slot immediately above the identity block";

    // Frame 2: a second character enters. It sorts first and takes the one slot,
    // so the settled character's upload is refused.
    ECS::Entity winnerEntity = world.Create();
    winnerEntity.Set(Components::SkeletonRef{m_SkeletonId, winner});
    world.ProcessCommands();

    BeginPaletteFrame(1);
    uploadSystem.Update(world, kDt);

    ASSERT_NE(store.GetRuntime(winner), nullptr);
    ASSERT_EQ(store.GetRuntime(winner)->AtlasPaletteOffsetBones, settledOffset)
        << "premise: the newcomer took the one slot";

    ASSERT_NE(store.GetRuntime(displaced), nullptr);
    EXPECT_NE(store.GetRuntime(displaced)->AtlasPaletteOffsetBones, settledOffset)
        << "the refused character kept the offset the atlas has since handed to "
           "another runtime — it would be skinned with that runtime's bones";
    EXPECT_EQ(store.GetRuntime(displaced)->AtlasPaletteOffsetBones,
              SkinPaletteAtlas::kIdentityPaletteOffsetBones)
        << "a refused allocation must retire the offset to the identity block so "
           "the frame renders bind pose";
}

TEST_F(SkinPaletteRefusalTest, MissingJointTableUploadsCompleteAdjacentLargePalettes)
{
    constexpr uint32 kJoints = 300u;
    auto* skeleton = SkeletonStore::Instance().Get(m_SkeletonId);
    skeleton->BoneCount = kJoints;
    skeleton->SkinJointCount = 1u;
    skeleton->JointNodes.clear();
    SetBonePaletteCapacity(2u * kJoints / kTestJointCount);
    ECS::World world;
    std::array<uint32, 2u> runtimes{MakeRuntime(), MakeRuntime()};
    for (uint32 index = 0u; index < runtimes.size(); ++index)
    {
        auto* runtime = SkeletonStore::Instance().GetRuntime(runtimes[index]);
        ASSERT_NE(runtime, nullptr);
        EXPECT_EQ(runtime->CompactSkinMatrices.size(), kJoints * 16u);
        runtime->CompactSkinMatrices.assign(kJoints * 16u, 0.0f);
        for (uint32 joint = 0u; joint < kJoints; ++joint)
        {
            float* matrix = runtime->CompactSkinMatrices.data() + joint * 16u;
            matrix[0] = matrix[5] = matrix[10] = matrix[15] = 1.0f;
            matrix[12] = static_cast<float>(index * kJoints + joint);
        }
        world.AddComponentImmediate(world.CreateEntity(), Components::SkeletonRef{m_SkeletonId, runtimes[index]});
    }
    BeginPaletteFrame(0u);
    SkinningUploadSystem(&m_RenderServices).Update(world, kDt);
    auto& atlas = m_RenderServices.GetSkinPaletteAtlas();
    EXPECT_EQ(atlas.GetCurrentFrameAllocCount(), 2u);
    EXPECT_EQ(atlas.GetCurrentFrameTotalBones(), 2u * kJoints);
    const uint32 first = SkeletonStore::Instance().GetRuntime(runtimes[0])->AtlasPaletteOffsetBones;
    const uint32 second = SkeletonStore::Instance().GetRuntime(runtimes[1])->AtlasPaletteOffsetBones;
    EXPECT_GE((std::max)(first, second) - (std::min)(first, second), kJoints);
    const auto* rows = static_cast<const float*>(m_Device->MapBuffer(atlas.GetBuffer()));
    ASSERT_NE(rows, nullptr);
    for (uint32 index = 0u; index < runtimes.size(); ++index)
    {
        const uint32 offset = SkeletonStore::Instance().GetRuntime(runtimes[index])->AtlasPaletteOffsetBones;
        EXPECT_FLOAT_EQ(rows[(offset + kJoints - 1u) * BonePaletteLayout::kFloatsPerBone + 3u],
            static_cast<float>((index + 1u) * kJoints - 1u));
    }
    m_Device->UnmapBuffer(atlas.GetBuffer());
}

TEST_F(SkinPaletteRefusalTest, MissingJointTableReservesCompleteAdjacentGpuPalettes)
{
    m_FrameOpen = m_Device->BeginFrame();
    ASSERT_TRUE(m_FrameOpen);
    auto* skeleton = SkeletonStore::Instance().Get(m_SkeletonId);
    skeleton->SkinJointCount = 1u;
    skeleton->JointNodes.clear();
    skeleton->Parent = {-1, 0, 0, 0};
    skeleton->RestTranslation.assign(12u, 0.0f);
    skeleton->RestRotation.assign(16u, 0.0f);
    skeleton->RestScale.assign(12u, 1.0f);
    skeleton->RestLocalMatrix.assign(64u, 0.0f);
    for (uint32 node = 0u; node < kTestJointCount; ++node)
    {
        skeleton->RestRotation[node * 4u + 3u] = 1.0f;
        for (uint32 diagonal : {0u, 5u, 10u, 15u})
        {
            skeleton->RestLocalMatrix[node * 16u + diagonal] = 1.0f;
            skeleton->InverseBind[node * 16u + diagonal] = 1.0f;
        }
    }
    skeleton->ComputeTopologicalSort();
    auto clip = MakeShared<AnimationClip>(GUID::Generate(), "Synthetic://JointTableReservation");
    AnimChannel channel;
    channel.boneIndex = 3u;
    channel.path = AnimPath::Translation;
    channel.keys.resize(2u);
    channel.keys[1].time = 1.0f;
    channel.keys[1].translation[0] = 2.0f;
    clip->SetChannelsAndDurationForTest({channel}, 1.0f);
    const uint32 clipIndex = ClipStore::Instance().RegisterRuntimeClip(clip->GetGUID(), clip);
    ECS::World world;
    const std::array<uint32, 2u> runtimes{MakeRuntime(), MakeRuntime()};
    for (uint32 runtime : runtimes)
    {
        const auto entity = world.CreateEntity();
        world.AddComponentImmediate(entity, Components::SkeletonRef{m_SkeletonId, runtime});
        Components::AnimatorRef playback;
        playback.ClipIndex = clipIndex;
        playback.Time = 0.5f;
        playback.Flags = Components::AnimatorRef::kFlag_Paused;
        world.AddComponentImmediate(entity, playback);
    }
    SetBonePaletteCapacity(2u);
    BeginPaletteFrame(0u);
    RenderGraph::RGResourcePool persistent(m_Device.get());
    RenderGraph::RGTransientPool transient(m_Device.get());
    RenderGraph::RGUploadRing upload(m_Device.get(), 2u, 65536u);
    RenderGraph::RGFrame frame(m_Device.get(), &persistent, &transient, &upload);
    frame.BeginFrame(0u);
    struct ShaderResolverScope
    {
        Utils::ShaderPathResolverFunc Previous = Utils::GetShaderPathResolver();
        ~ShaderResolverScope() { Utils::SetShaderPathResolver(Previous); }
    } resolver;
    Utils::SetShaderPathResolver(+[](const std::filesystem::path& relative) {
        return TestPaths::StagedRoot() / relative;
    });
    m_RenderServices.ScheduleGpuSkinningAndRetarget(frame);
    ASSERT_TRUE(m_RenderServices.IsComputeSkinningReady());
    AnimationSystem(&m_RenderServices).Update(world, kDt);
    auto& atlas = m_RenderServices.GetSkinPaletteAtlas();
    ASSERT_EQ(m_RenderServices.GetGPUAnimationDataStore().GetInstanceCount(), 2u);
    EXPECT_EQ(atlas.GetCurrentFrameTotalBones(), 2u * kTestJointCount);
    const uint32 first = SkeletonStore::Instance().GetRuntime(runtimes[0])->AtlasPaletteOffsetBones;
    const uint32 second = SkeletonStore::Instance().GetRuntime(runtimes[1])->AtlasPaletteOffsetBones;
    EXPECT_GE((std::max)(first, second) - (std::min)(first, second), kTestJointCount);
    frame.Execute();
    m_Device->WaitForIdle();
    const auto* rows = static_cast<const float*>(m_Device->MapBuffer(atlas.GetBuffer()));
    ASSERT_NE(rows, nullptr);
    for (uint32 offset : {first, second})
        EXPECT_NEAR(rows[(offset + 3u) * BonePaletteLayout::kFloatsPerBone + 3u], 1.0f, 0.0001f);
    m_Device->UnmapBuffer(atlas.GetBuffer());
}

TEST_F(SkinPaletteRefusalTest, RefusedLargePaletteReadsIdentityForEveryJointAndPreviousPose)
{
    m_FrameOpen = m_Device->BeginFrame();
    ASSERT_TRUE(m_FrameOpen);
    SetBonePaletteCapacity(1024u);
    BeginPaletteFrame(0u);
    auto& atlas = m_RenderServices.GetSkinPaletteAtlas();
    std::vector<float> neighbor(4096u * 16u, 7.0f);
    const auto allocation = atlas.Upload(neighbor.data(), 4096u);
    ASSERT_TRUE(allocation.valid);
    ASSERT_EQ(atlas.Reserve(1024u), UINT32_MAX);

    TestUtils::ScopedTempDir temporary{TestUtils::MakeUniqueTempDirectory("bone_palette_fallback")};
    ShaderProgramCompileRequest request;
    request.debugName = "BonePaletteFallbackProbe";
    request.baseDirectory = std::filesystem::path(GE_RENDERER_REPO_ROOT) / "Engine/Modules/Rendering/Shaders";
    request.includeDirs = {request.baseDirectory};
    request.cacheRoot = temporary.Path();
    ShaderStageCompileSpec stage;
    stage.stage = "cs";
    stage.sourcePath = request.baseDirectory / "bone_palette_fallback_probe.comp";
    stage.inlineSource = R"GLSL(#version 450
#extension GL_GOOGLE_include_directive : require
layout(local_size_x=1) in;
layout(std430,set=0,binding=0) readonly buffer Palette { vec4 rows[]; } BonePaletteAtlas;
layout(std430,set=0,binding=1) readonly buffer Previous { vec4 rows[]; } PrevBonePaletteAtlas;
layout(std430,set=0,binding=2) buffer Output { vec4 values[]; } result;
#define GE_BONE_PALETTE_READABLE
#define GE_BONE_PALETTE_PREV_READABLE
#include "Includes/bone_palette.glsl"
void main() {
    uint joints[5] = uint[5](255u,256u,299u,1023u,4095u);
    for (uint index=0u; index<5u; ++index) {
        vec3 position=vec3(2,3,4), normal=vec3(0,1,0);
        ge_ApplyBonePalette(position,normal,uvec4(joints[index]),vec4(1,0,0,0),0u);
        result.values[index*4u] = vec4(position,1);
        result.values[index*4u+1u] = vec4(normal,0);
        result.values[index*4u+2u] = vec4(ge_ApplyBonePalettePosition(vec3(2,3,4),uvec4(joints[index]),vec4(1,0,0,0),0u),1);
        result.values[index*4u+3u] = vec4(ge_ApplyPrevBonePalettePosition(vec3(2,3,4),uvec4(joints[index]),vec4(1,0,0,0),0u),1);
    }
})GLSL";
    request.stages = {stage};
    ShaderProgramCompileResult compiled;
    std::string error;
    ASSERT_TRUE(ShaderCompileService::CompileProgramToCache(request, m_Device->PreferredShaderSource(), compiled, &error)) << error;
    PipelineDesc description;
    description.type = PipelineType::Compute;
    description.computeShader = compiled.stageBytes.at("cs");
    DescriptorSetLayoutDesc layout;
    for (uint32 binding = 0u; binding < 3u; ++binding)
    {
        DescriptorBinding entry;
        entry.binding = binding;
        entry.type = DescriptorType::StorageBuffer;
        entry.count = 1u;
        entry.shaderStages = kShaderStageCompute;
        layout.bindings.push_back(entry);
    }
    description.descriptorSetLayouts = {layout};
    const auto pipeline = m_Device->CreatePipeline(description);
    ASSERT_TRUE(pipeline.IsValid());
    // Each std430 shader result is one four-float vec4 record.
    std::array<std::array<float, 4u>, 20u> actual{};
    BufferDesc outputDescription;
    outputDescription.size = sizeof(actual);
    outputDescription.usage = static_cast<uint32>(BufferUsage::Storage);
    outputDescription.memoryUsage = BufferMemoryUsage::Readback;
    const auto output = m_Device->CreateBuffer(outputDescription);
    ASSERT_TRUE(output.IsValid());
    DescriptorSetDesc setDescription;
    setDescription.layout = layout;
    const auto set = m_Device->CreateDescriptorSet(setDescription);
    ASSERT_TRUE(set.IsValid());
    for (uint32 binding = 0u; binding < 3u; ++binding)
    {
        DescriptorSetUpdate update;
        update.binding = binding;
        update.type = DescriptorType::StorageBuffer;
        update.buffers = {binding < 2u ? atlas.GetBuffer() : output};
        update.bufferOffsets = {0u};
        update.bufferRanges = {binding < 2u ? atlas.GetBufferBytes() : sizeof(actual)};
        m_Device->UpdateDescriptorSet(set, update);
    }
    auto commands = m_Device->CreateCommandList(IDevice::QueueType::Compute);
    ASSERT_TRUE(commands);
    commands->Begin();
    commands->SetPipeline(pipeline);
    commands->BindDescriptorSet(0u, set, pipeline);
    commands->Dispatch(1u, 1u, 1u);
    commands->End();
    m_Device->ExecuteCommandLists({commands.get()});
    m_Device->WaitForIdle();
    const void* mapped = m_Device->MapBuffer(output);
    ASSERT_NE(mapped, nullptr);
    std::memcpy(actual.data(), mapped, sizeof(actual));
    m_Device->UnmapBuffer(output);
    for (uint32 index = 0u; index < actual.size(); ++index)
    {
        const std::array<float, 4u> expected = index % 4u == 1u
            ? std::array<float, 4u>{0.0f, 1.0f, 0.0f, 0.0f} : std::array<float, 4u>{2.0f, 3.0f, 4.0f, 1.0f};
        EXPECT_EQ(actual[index], expected) << "probe " << index;
    }
    m_Device->DestroyDescriptorSet(set);
    m_Device->DestroyPipeline(pipeline);
    m_Device->DestroyBuffer(output);
}
