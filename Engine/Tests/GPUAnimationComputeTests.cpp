// GPU compute skinning parity: the production AnimationComputePass, declared on a
// real render-graph frame and fed by GPUAnimationDataStore, must write the same
// skin matrices into the palette atlas as the CPU SampleAnimationPose reference,
// from a three-bone chain up to the kernel's 256-bone and 64-level limits, for
// rigs whose nodes outside the joint closure the kernel never evaluates, on the
// desktop kernel and on its compat-profile arm (the one the web cook translates).
// The store's refusal of what the kernel cannot hold needs no device.

#include "Rendering/Core/Device.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Common/Utils.h"
#include "Engine/Rendering/AnimationComputePass.h"
#include "Engine/Rendering/BonePaletteLayout.h"
#include "Engine/Rendering/GPUAnimationDataStore.h"
#include "Engine/Rendering/AnimationSampling.h"
#include "ECSModules/Rendering/SkeletonStore.h"
#include "ECSModules/Rendering/ClipStore.h"
#include "Assets/AnimationClip.h"
#include "Animation/BoneEvaluationPlan.h"
#include "StagedTestPaths.h"
#include "Components/Animation/AnimatorRef.h"
#include "Components/Animation/SkeletonRef.h"
#include "ECS/ECSTemplates.h"
#include "ECSModules/Rendering/Systems/AnimationSystem.h"
#include "ECSModules/Rendering/Systems/SkinningUploadSystem.h"
#include "Engine/Rendering/RenderServices.h"

#if RENDERING_HAS_METAL
#include "../Modules/Rendering/Source/Metal/MetalDevice.h"
#elif RENDERING_HAS_VULKAN
#include "../Modules/Rendering/Source/Vulkan/VulkanDevice.h"
#endif

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <ostream>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <gtest/gtest.h>

using namespace GameEngine;
using namespace GameEngine::Rendering;
using namespace GameEngine::Engine::Renderer;
namespace RG = GameEngine::Rendering::RenderGraph;

namespace
{
constexpr uint32 kChainJointCount = 3;
constexpr float kMatrixTolerance = 1e-4f;
constexpr uint32 kUploadRingFramesInFlight = 2;
constexpr uint64_t kUploadRingSlotBytes = 64 * 1024;
constexpr float kCrowdInstanceTimeStep = 0.00025f;

struct FramePools
{
    RG::RGResourcePool Persistent;
    RG::RGTransientPool Transient;
    RG::RGUploadRing Upload;
    explicit FramePools(IDevice* device)
        : Persistent(device), Transient(device),
          Upload(device, kUploadRingFramesInFlight, kUploadRingSlotBytes) {}
};

// Three-bone chain 0 -> 1 -> 2; bones 1 and 2 sit one unit up +Y from their parent.
// Bind pose equals rest pose, so every inverse bind is identity.
uint32 CreateTestSkeleton()
{
    auto& store = SkeletonStore::Instance();
    const uint32 skeletonId = store.CreateSkeleton(kChainJointCount);
    auto* skeleton = store.Get(skeletonId);
    skeleton->BoneCount = kChainJointCount;
    skeleton->SkinJointCount = kChainJointCount;
    skeleton->Parent = {-1, 0, 1};
    skeleton->JointNodes = {0, 1, 2};
    skeleton->RestTranslation = {0, 0, 0, 0, 1, 0, 0, 1, 0};
    skeleton->RestRotation = {0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1};
    skeleton->RestScale = {1, 1, 1, 1, 1, 1, 1, 1, 1};

    const glm::mat4 identity(1.0f);
    skeleton->RestLocalMatrix.resize(kChainJointCount * 16);
    skeleton->InverseBind.resize(kChainJointCount * 16);
    for (uint32 i = 0; i < kChainJointCount; ++i)
    {
        glm::mat4 local(1.0f);
        if (i > 0)
            local = glm::translate(local, glm::vec3(0.0f, 1.0f, 0.0f));
        std::memcpy(&skeleton->RestLocalMatrix[i * 16], glm::value_ptr(local), sizeof(glm::mat4));
        std::memcpy(&skeleton->InverseBind[i * 16], glm::value_ptr(identity), sizeof(glm::mat4));
    }
    std::memcpy(skeleton->MeshRootWorld, glm::value_ptr(identity), sizeof(glm::mat4));
    std::memcpy(skeleton->SkeletonRootWorld, glm::value_ptr(identity), sizeof(glm::mat4));
    skeleton->ComputeTopologicalSort();
    return skeletonId;
}

AnimKeyframe TranslationKey(float time, float x)
{
    AnimKeyframe key{};
    key.time = time;
    key.translation[0] = x;
    key.rotation[3] = 1.0f;
    key.scale[0] = key.scale[1] = key.scale[2] = 1.0f;
    return key;
}

AnimKeyframe RotationKey(float time, const glm::quat& rotation)
{
    AnimKeyframe key{};
    key.time = time;
    key.rotation[0] = rotation.x;
    key.rotation[1] = rotation.y;
    key.rotation[2] = rotation.z;
    key.rotation[3] = rotation.w;
    key.scale[0] = key.scale[1] = key.scale[2] = 1.0f;
    return key;
}

// `node` translates linearly from X = 0 at t = 0 to X = endX at t = 1.
AnimChannel TranslationChannel(uint32 node, float endX)
{
    AnimChannel channel{};
    channel.boneIndex = node;
    channel.path = AnimPath::Translation;
    channel.interp = AnimInterp::Linear;
    channel.keys = {TranslationKey(0.0f, 0.0f), TranslationKey(1.0f, endX)};
    return channel;
}

// `node` turns from identity at t = 0 to 0.6 rad at t = 1, about an axis that changes with the node.
AnimChannel RotationChannel(uint32 node)
{
    const glm::vec3 axis(static_cast<float>(node % 2), 1.0f, static_cast<float>(node % 5) * 0.2f);
    AnimChannel channel{};
    channel.boneIndex = node;
    channel.path = AnimPath::Rotation;
    channel.interp = AnimInterp::Linear;
    channel.keys = {RotationKey(0.0f, glm::quat(1.0f, 0.0f, 0.0f, 0.0f)),
                    RotationKey(1.0f, glm::angleAxis(0.6f, glm::normalize(axis)))};
    return channel;
}

uint32 RegisterClip(std::vector<AnimChannel> channels)
{
    const GUID clipGuid = GUID::Generate();
    auto clip = MakeShared<AnimationClip>(clipGuid, std::filesystem::path("Synthetic://GPUTest"));
    clip->SetChannelsAndDurationForTest(std::move(channels), 1.0f);
    return ClipStore::Instance().RegisterRuntimeClip(clipGuid, std::move(clip));
}

// A skeleton and a clip registered in the stores, and the number of skin joints the skeleton writes.
struct Rig
{
    uint32 SkeletonId = 0;
    uint32 ClipIndex = 0;
    uint32 JointCount = 0;
};

// The three-bone chain; bone 0 translates from X = 0 at t = 0 to X = 2 at t = 1.
Rig CreateChainRig()
{
    return {CreateTestSkeleton(), RegisterClip({TranslationChannel(0, 2.0f)}), kChainJointCount};
}

// Every node sits 0.1 up +Y from its parent, turned 0.05 rad about an axis that changes with the
// node, so the hierarchy composes rotations as well as offsets. Inverse binds are distinct
// translations, so a joint that reads another joint's inverse bind shows in its palette entry.
void FillRigSkeleton(SkeletonData* skeleton, const std::vector<int32>& parent, const std::vector<uint32>& jointNodes)
{
    const uint32 nodeCount = static_cast<uint32>(parent.size());
    skeleton->BoneCount = nodeCount;
    skeleton->Parent = parent;
    skeleton->SkinJointCount = static_cast<uint32>(jointNodes.size());
    skeleton->JointNodes = jointNodes;
    skeleton->RestTranslation.assign(nodeCount * 3, 0.0f);
    skeleton->RestRotation.assign(nodeCount * 4, 0.0f);
    skeleton->RestScale.assign(nodeCount * 3, 1.0f);
    skeleton->RestLocalMatrix.resize(nodeCount * 16);
    skeleton->InverseBind.resize(nodeCount * 16);
    for (uint32 node = 0; node < nodeCount; ++node)
    {
        const glm::vec3 translation(0.0f, 0.1f, 0.0f);
        const glm::vec3 axis(1.0f, static_cast<float>(node % 3), static_cast<float>(node % 7) * 0.1f + 0.1f);
        const glm::quat rotation = glm::angleAxis(0.05f, glm::normalize(axis));
        skeleton->RestTranslation[node * 3 + 1] = translation.y;
        skeleton->RestRotation[node * 4 + 0] = rotation.x;
        skeleton->RestRotation[node * 4 + 1] = rotation.y;
        skeleton->RestRotation[node * 4 + 2] = rotation.z;
        skeleton->RestRotation[node * 4 + 3] = rotation.w;
        const glm::mat4 local = glm::translate(glm::mat4(1.0f), translation) * glm::mat4_cast(rotation);
        const glm::vec3 inverseBindOffset(-0.01f * static_cast<float>(node), -0.02f * static_cast<float>(node % 5), 0.03f);
        const glm::mat4 inverseBind = glm::translate(glm::mat4(1.0f), inverseBindOffset);
        std::memcpy(&skeleton->RestLocalMatrix[node * 16], glm::value_ptr(local), sizeof(glm::mat4));
        std::memcpy(&skeleton->InverseBind[node * 16], glm::value_ptr(inverseBind), sizeof(glm::mat4));
    }
    skeleton->ComputeTopologicalSort();
}

uint32 CreateRigSkeleton(const std::vector<int32>& parent, const std::vector<uint32>& jointNodes)
{
    auto& store = SkeletonStore::Instance();
    const uint32 skeletonId = store.CreateSkeleton(static_cast<uint32>(parent.size()));
    FillRigSkeleton(store.Get(skeletonId), parent, jointNodes);
    return skeletonId;
}

// Node 0 is the root of every other node.
std::vector<int32> FlatParents(uint32 nodeCount)
{
    std::vector<int32> parent(nodeCount, 0);
    parent[0] = -1;
    return parent;
}

// Node n hangs off node n - 1: one hierarchy level per node.
std::vector<int32> ChainParents(uint32 nodeCount)
{
    std::vector<int32> parent(nodeCount);
    for (uint32 node = 0; node < nodeCount; ++node)
        parent[node] = static_cast<int32>(node) - 1;
    return parent;
}

// A skin whose joint j is node j.
std::vector<uint32> EveryNodeAJoint(uint32 nodeCount)
{
    std::vector<uint32> jointNodes(nodeCount);
    for (uint32 node = 0; node < nodeCount; ++node)
        jointNodes[node] = node;
    return jointNodes;
}

// `jointCount` joints in a hand-like tree: joints 0-15 form a spine chain and every further joint
// belongs to a five-joint finger whose first joint hangs off a spine joint, so 256 joints stand
// 21 levels deep. Joint 0 translates and every fourth joint turns.
Rig CreateFingerRig(uint32 jointCount)
{
    constexpr uint32 kSpineJoints = 16;
    constexpr uint32 kFingerJoints = 5;
    std::vector<int32> parent(jointCount, -1);
    std::vector<uint32> jointNodes(jointCount);
    std::vector<AnimChannel> channels = {TranslationChannel(0, 1.0f)};
    for (uint32 joint = 1; joint < jointCount; ++joint)
    {
        const bool fingerRoot = joint >= kSpineJoints && (joint - kSpineJoints) % kFingerJoints == 0;
        parent[joint] = fingerRoot ? static_cast<int32>(((joint - kSpineJoints) / kFingerJoints) % kSpineJoints)
                                   : static_cast<int32>(joint - 1);
        if (joint % 4 == 3)
            channels.push_back(RotationChannel(joint));
    }
    for (uint32 joint = 0; joint < jointCount; ++joint)
        jointNodes[joint] = joint;
    return {CreateRigSkeleton(parent, jointNodes), RegisterClip(std::move(channels)), jointCount};
}

Rig CreateTwoHundredJointRig() { return CreateFingerRig(200); }

Rig CreateTwoHundredFiftySixJointRig() { return CreateFingerRig(256); }

// 300 nodes, 100 of them joints. Node 0 is the armature: no joint, every joint's ancestor, and it
// translates. Joint k is node 1 + 3k; joint 0 hangs off the armature and the other 99 off joint 0,
// one hierarchy level wider than a workgroup. Node 2 + 3k is a socket under joint k and node 3 + 3k
// a root of its own; neither is in the joint closure, and the clip turns socket 2 and moves root 3.
// The skin lists the joints in reverse node order.
Rig CreateDecoratedRig()
{
    constexpr uint32 kNodeCount = 300;
    constexpr uint32 kJointCount = 100;
    std::vector<int32> parent(kNodeCount, -1);
    std::vector<uint32> jointNodes(kJointCount);
    std::vector<AnimChannel> channels = {TranslationChannel(0, 1.0f), RotationChannel(2), TranslationChannel(3, 2.0f)};
    for (uint32 joint = 0; joint < kJointCount; ++joint)
    {
        const uint32 node = 1 + 3 * joint;
        parent[node] = (joint == 0) ? 0 : 1;
        parent[node + 1] = static_cast<int32>(node);
        jointNodes[kJointCount - 1 - joint] = node;
        if (joint % 4 == 0)
            channels.push_back(RotationChannel(node));
    }
    return {CreateRigSkeleton(parent, jointNodes), RegisterClip(std::move(channels)), kJointCount};
}

// 64 joints in one chain, the kernel's level limit. Joint 0 translates and every third joint turns.
Rig CreateSixtyFourLevelChain()
{
    constexpr uint32 kLevels = 64;
    std::vector<AnimChannel> channels = {TranslationChannel(0, 1.0f)};
    for (uint32 node = 2; node < kLevels; node += 3)
        channels.push_back(RotationChannel(node));
    return {CreateRigSkeleton(ChainParents(kLevels), EveryNodeAJoint(kLevels)), RegisterClip(std::move(channels)),
            kLevels};
}

// 180 nodes in which every parent has a higher node index than its child: node n hangs off node
// n + 4 + n % 7 (clamped to the root, node 179). The skin lists its joints in node order, so children
// come first, and leaves out every fifth node, which the joints below it pull into the closure. The
// root translates and every fourth node turns.
Rig CreateReversedNodeOrderRig()
{
    constexpr uint32 kNodeCount = 180;
    std::vector<int32> parent(kNodeCount, -1);
    std::vector<uint32> jointNodes;
    std::vector<AnimChannel> channels = {TranslationChannel(kNodeCount - 1, 1.0f)};
    for (uint32 node = 0; node + 1 < kNodeCount; ++node)
        parent[node] = static_cast<int32>(std::min(kNodeCount - 1, node + 4 + node % 7));
    for (uint32 node = 0; node < kNodeCount; ++node)
    {
        if (node % 5 != 2)
            jointNodes.push_back(node);
        if (node % 4 == 3)
            channels.push_back(RotationChannel(node));
    }
    const uint32 jointCount = static_cast<uint32>(jointNodes.size());
    return {CreateRigSkeleton(parent, jointNodes), RegisterClip(std::move(channels)), jointCount};
}

std::vector<float> SampleCpuReference(uint32 skeletonId, uint32 clipIndex, float time)
{
    const auto* skeleton = SkeletonStore::Instance().Get(skeletonId);
    const auto clip = ClipStore::Instance().Get(clipIndex);
    std::vector<float> skinMatrices;
    PoseSampleWorkspace workspace;
    SampleAnimationPose(*skeleton, clip.get(), time, nullptr, &skinMatrices, workspace);
    return skinMatrices;
}

// Mirrors AnimationSystem's GPU instance for a runtime whose palette starts at atlas offset 0.
GPUAnimInstance MakeInstance(uint32 skeletonGpuIndex, uint32 clipGpuIndex, float time)
{
    GPUAnimInstance instance{};
    instance.skeletonGPUIndex = skeletonGpuIndex;
    instance.clipGPUIndex = clipGpuIndex;
    instance.time = time;
    instance.atlasOutputOffset = 0;
    return instance;
}

constexpr float kLODInstanceTimeStep = 0.125f;

void AddTestInstances(GPUAnimationDataStore& store, uint32 skeletonGpuIndex, uint32 clipGpuIndex,
    float time, uint32 jointCount, uint32 population, float instanceTimeStep = kCrowdInstanceTimeStep)
{
    for (uint32 instanceIndex = 0u; instanceIndex < population; ++instanceIndex)
    {
        auto instance = MakeInstance(skeletonGpuIndex, clipGpuIndex,
            time + instanceTimeStep * static_cast<float>(instanceIndex));
        instance.atlasOutputOffset = instanceIndex * jointCount;
        ASSERT_TRUE(store.TryAddInstance(instance));
    }
}

BufferHandle CreateReadbackAtlas(IDevice& device, size_t sizeBytes)
{
    BufferDesc desc{};
    desc.size = sizeBytes;
    desc.usage = static_cast<uint32_t>(BufferUsage::Storage | BufferUsage::TransferSrc | BufferUsage::TransferDst);
    desc.memoryUsage = BufferMemoryUsage::Readback;
    desc.flags = BufferCreateFlags::PersistentlyMapped;
    desc.debugName = "GPUAnimationComputeTest.Atlas";
    return device.CreateBuffer(desc);
}

// The atlas stores each bone as three packed rows; the CPU reference is column-major mat4.
std::vector<float> UnpackAtlas(const std::vector<float>& packedRows, uint32 jointCount)
{
    using namespace BonePaletteLayout;
    std::vector<float> matrices(jointCount * 16);
    for (uint32 joint = 0; joint < jointCount; ++joint)
        UnpackRowsToMat4(packedRows.data() + joint * kFloatsPerBone, matrices.data() + joint * 16);
    return matrices;
}

struct SampleCase
{
    const char* Name;
    Rig (*CreateRig)();
    float Time;
};

// A build of animation_skinning.comp staged under <build>/Shaders.
struct Kernel
{
    const char* Name;
    const char* File;
};

// The kernel every desktop profile loads, and its GE_COMPAT_PROFILE arm: the constant-bound, masked
// level loop the web cook translates to WGSL, built for Vulkan so this suite runs it.
constexpr Kernel kDesktopKernel{"Desktop", "animation_skinning.comp.spv"};
constexpr Kernel kCompatKernel{"Compat", "Tests/animation_skinning_compat.comp.spv"};

using KernelCase = std::tuple<SampleCase, Kernel>;
using LODCase = std::tuple<uint32, Kernel>;

void PrintTo(const SampleCase& sampleCase, std::ostream* os) { *os << sampleCase.Name; }

void PrintTo(const Kernel& kernel, std::ostream* os) { *os << kernel.Name; }

std::string KernelCaseName(const ::testing::TestParamInfo<KernelCase>& info)
{
    return std::string(std::get<0>(info.param).Name) + "_" + std::get<1>(info.param).Name;
}

constexpr uint32 kUnknownSkeletonId = 0x7FFFFFF0u;

} // namespace

class GPUAnimationComputeFixture : public ::testing::Test
{
protected:
    virtual GraphicsAPI RequestedAPI() const
    {
#ifdef __APPLE__
        return GraphicsAPI::Metal;
#else
        return GraphicsAPI::Vulkan;
#endif
    }
    virtual bool RequiresRequestedAPI() const { return false; }
    virtual std::unique_ptr<IDevice> CreateDevice(const DeviceDesc& description)
    {
        return DeviceFactory::CreateDevice(description);
    }

    void SetUp() override
    {
        DeviceDesc desc{};
        desc.applicationName = "GPUAnimationComputeTest";
        desc.preferredAPI = RequestedAPI();
        desc.enableDebugLayer = true;
        m_Device = CreateDevice(desc);
        if (!m_Device || !m_Device->Initialize(desc))
            m_Device.reset();
        if (m_Device)
        {
            RecordProperty("graphics_api", static_cast<int>(m_Device->GetAPI()));
            RecordProperty("native_metal", m_Device->GetAPI() == GraphicsAPI::Metal ? "true" : "false");
            if (RequiresRequestedAPI() && m_Device->GetAPI() != RequestedAPI())
                GTEST_SKIP() << "Native animation coverage requires API " << static_cast<int>(RequestedAPI())
                    << "; the backend override selected API " << static_cast<int>(m_Device->GetAPI());
        }
    }

    void TearDown() override
    {
        FinishDeviceFrame();
        m_DataStore.Shutdown();
        if (m_Device)
            m_Device->Shutdown();
        m_Device.reset();
    }

    bool BeginDeviceFrame()
    {
        if (!m_FrameOpen)
            m_FrameOpen = m_Device->BeginFrame();
        return m_FrameOpen;
    }

    void FinishDeviceFrame()
    {
        if (!m_FrameOpen)
            return;
        m_Device->FinalizeFrame();
        m_Device->WaitForIdle();
        m_FrameOpen = false;
    }

    // One production dispatch: the data store uploads skeleton, clip and instance,
    // AnimationComputePass declares its pass on a render-graph frame, the frame executes.
    std::vector<float> DispatchProductionPass(const Kernel& kernel, const Rig& rig, float time,
                                              bool retryAfterClearing = false,
                                              const SharedPtr<const Animation::BoneEvaluationPlan>& plan = {}, uint32 population = 1u,
                                              float instanceTimeStep = kCrowdInstanceTimeStep)
    {
        const bool frameOpen = BeginDeviceFrame();
        EXPECT_TRUE(frameOpen);
        if (!frameOpen)
            return {};
        const std::vector<uint8_t> shader = Utils::ReadFile(
            (TestPaths::StagedRoot() / "Shaders" / kernel.File).string());
        EXPECT_FALSE(shader.empty()) << kernel.File << " is not staged under <build>/Shaders";
        if (shader.empty())
            return {};

        EXPECT_TRUE(m_DataStore.Initialize(m_Device.get()));
        m_DataStore.BeginFrame(0);
        const uint32 skeletonGpuIndex = m_DataStore.EnsureSkeletonUploaded(rig.SkeletonId, plan);
        EXPECT_NE(skeletonGpuIndex, GPUAnimationDataStore::kNotUploaded);
        if (skeletonGpuIndex == GPUAnimationDataStore::kNotUploaded)
            return {};
        const uint32 clipGpuIndex = m_DataStore.EnsureClipUploaded(rig.ClipIndex);

        std::vector<float> packedRows(rig.JointCount * population * BonePaletteLayout::kFloatsPerBone);
        const size_t atlasBytes = packedRows.size() * sizeof(float);
        const BufferHandle atlas = CreateReadbackAtlas(*m_Device, atlasBytes);
        EXPECT_TRUE(atlas.IsValid());
        if (!atlas.IsValid())
            return {};

        AnimationComputePass pass;
        pass.SetComputeShader(shader);
        pass.SetDataStore(&m_DataStore);
        {
            FramePools pools(m_Device.get());
            RG::RGFrame frame(m_Device.get(), &pools.Persistent, &pools.Transient, &pools.Upload);
            frame.BeginFrame(0);
            const RG::RGBuffer atlasResource = frame.ImportExternalBuffer("SkinPaletteAtlas", atlas, atlasBytes);
            pass.Declare(frame, atlasResource, RG::RGBuffer{});
            // Preview worlds append instances after the main graph is declared.
            // Preparation must observe these late arrivals before any recording.
            AddTestInstances(m_DataStore, skeletonGpuIndex, clipGpuIndex, time, rig.JointCount, population, instanceTimeStep);
            EXPECT_EQ(frame.Graph().PassCount(), 1u) << "AnimationComputePass declared no pass";
            if (retryAfterClearing)
            {
                constexpr float kSentinel = -37.0f;
                const std::vector<float> sentinel(packedRows.size(), kSentinel);
                m_Device->UpdateBuffer(atlas, 0, atlasBytes, sentinel.data());
                bool abortPreparation = true;
                uint32_t records = 0;
                frame.AddPass("AnimationRetry.Gate", PassPhase::kEarlySetup + 1,
                    [atlasResource, &abortPreparation](RG::RGPassBuilder& builder)
                    {
                        builder.Read(atlasResource);
                        builder.PreventCulling();
                        builder.Prepare([&abortPreparation](RG::RGPrepareContext&) { return !abortPreparation; });
                    },
                    [&records](RG::RGContext& context)
                    {
                        context.Cmd->Barrier(ResourceBarrier::CreateMemoryBarrier(
                            static_cast<uint64_t>(PipelineStageMask::ComputeShader),
                            static_cast<uint64_t>(PipelineStageMask::Host),
                            static_cast<uint64_t>(ResourceAccessMask::ShaderWrite),
                            static_cast<uint64_t>(ResourceAccessMask::HostRead)));
                        ++records;
                    });
                frame.MarkOutput(atlasResource);
                const uint64_t submissions = frame.SubmittedFrameCount();
                frame.Execute();
                EXPECT_EQ(frame.SubmittedFrameCount(), submissions);
                EXPECT_EQ(frame.Stats().PreparedPasses, 2u);
                EXPECT_EQ(frame.Stats().PreparationFailures, 1u);
                EXPECT_EQ(records, 0u);

                m_DataStore.BeginFrame(0);
                EXPECT_EQ(m_DataStore.GetInstanceCount(), 0u);
                abortPreparation = false;
                frame.Execute();
                m_Device->WaitForIdle();
                EXPECT_EQ(frame.SubmittedFrameCount(), submissions + 1u);
                EXPECT_EQ(records, 1u);
                const void* unchanged = m_Device->MapBuffer(atlas);
                EXPECT_NE(unchanged, nullptr);
                if (unchanged)
                    EXPECT_EQ(std::memcmp(unchanged, sentinel.data(), atlasBytes), 0)
                        << "An empty retry replayed the dispatch prepared before the abort";
                m_Device->UnmapBuffer(atlas);

                // The same graph must still write valid matrices when work returns.
                AddTestInstances(m_DataStore, skeletonGpuIndex, clipGpuIndex, time, rig.JointCount, population, instanceTimeStep);
                frame.Execute();
                m_Device->WaitForIdle();
                EXPECT_EQ(records, 2u);
            }
            else
            {
                frame.Execute();
                m_Device->WaitForIdle();
            }
        }

        if (const void* mapped = m_Device->MapBuffer(atlas))
            std::memcpy(packedRows.data(), mapped, atlasBytes);
        m_Device->UnmapBuffer(atlas);
        m_Device->DestroyBuffer(atlas);
        return UnpackAtlas(packedRows, rig.JointCount * population);
    }

    std::unique_ptr<IDevice> m_Device;
    GPUAnimationDataStore m_DataStore;
private:
    bool m_FrameOpen = false;
};

class GPUAnimationComputeTest : public GPUAnimationComputeFixture,
                                public ::testing::WithParamInterface<KernelCase>
{
};

class GPUAnimationLODTest : public GPUAnimationComputeFixture,
                            public ::testing::WithParamInterface<LODCase>
{
protected:
    bool RequiresRequestedAPI() const override { return true; }
};

TEST_P(GPUAnimationLODTest, ReducedSelectionMatchesIndependentRestClampedCpuReference)
{
    if (!m_Device)
        GTEST_SKIP() << "Requested native device unavailable";
    Rig rig = CreateFingerRig(std::get<0>(GetParam()));
    auto* skeleton = SkeletonStore::Instance().Get(rig.SkeletonId);
    std::reverse(skeleton->JointNodes.begin(), skeleton->JointNodes.end());
    const glm::mat4 meshRoot = glm::translate(glm::mat4(1.0f), glm::vec3(3.0f, -2.0f, 1.0f));
    std::memcpy(skeleton->MeshRootWorld, glm::value_ptr(meshRoot), sizeof(glm::mat4));
    skeleton->RestLocalMatrix[33u * 16u + 4u] += 0.25f;
    skeleton->ComputeTopologicalSort();
    auto plan = MakeShared<Animation::BoneEvaluationPlan>();
    const uint32 pinned[] = {18u, 25u};
    ASSERT_TRUE(plan->Build(*skeleton, pinned));
    ASSERT_LT(plan->Nodes().size(), skeleton->JointClosure.size());

    std::vector<AnimChannel> channels;
    std::vector<AnimChannel> referenceChannels;
    for (uint32 node = 0; node < skeleton->BoneCount; ++node)
    {
        channels.push_back(TranslationChannel(node, 0.1f * static_cast<float>(node + 1u)));
        channels.push_back(RotationChannel(node));
        if (plan->EvaluatedAncestors()[node] == node)
        {
            referenceChannels.push_back(channels[channels.size() - 2u]);
            referenceChannels.push_back(channels.back());
        }
    }
    rig.ClipIndex = RegisterClip(std::move(channels));
    const uint32 referenceClip = RegisterClip(std::move(referenceChannels));
    constexpr float kSampleTime = 0.375f;
    constexpr uint32 kPopulation = 2u;
    std::vector<float> expected;
    for (uint32 instance = 0u; instance < kPopulation; ++instance)
    {
        const auto palette = SampleCpuReference(rig.SkeletonId, referenceClip,
            kSampleTime + kLODInstanceTimeStep * static_cast<float>(instance));
        expected.insert(expected.end(), palette.begin(), palette.end());
    }
    PoseSampleWorkspace workspace;
    std::vector<float> cpuReduced;
    const auto clip = ClipStore::Instance().Get(rig.ClipIndex);
    for (uint32 instance = 0u; instance < kPopulation; ++instance)
    {
        std::vector<float> palette;
        SampleAnimationPose(*skeleton, clip.get(), kSampleTime + kLODInstanceTimeStep * static_cast<float>(instance),
            nullptr, &palette, workspace, plan.get());
        cpuReduced.insert(cpuReduced.end(), palette.begin(), palette.end());
    }
    EXPECT_EQ(workspace.ChangedTranslation[33u], 0u);
    EXPECT_EQ(workspace.ChangedRotation[33u], 0u);
    const auto actual = DispatchProductionPass(std::get<1>(GetParam()), rig, kSampleTime, false, plan, kPopulation, kLODInstanceTimeStep);
    ASSERT_EQ(actual.size(), expected.size());
    ASSERT_EQ(cpuReduced.size(), expected.size());
    for (size_t element = 0; element < expected.size(); ++element)
    {
        EXPECT_NEAR(cpuReduced[element], expected[element], kMatrixTolerance) << element;
        EXPECT_NEAR(actual[element], expected[element], kMatrixTolerance) << element;
    }
}

INSTANTIATE_TEST_SUITE_P(Kernels, GPUAnimationLODTest,
    ::testing::Combine(::testing::Values(48u, 300u, 1024u), ::testing::Values(kDesktopKernel, kCompatKernel)),
    [](const ::testing::TestParamInfo<LODCase>& info)
    { return "Joints" + std::to_string(std::get<0>(info.param)) + "_" + std::get<1>(info.param).Name; });

#if RENDERING_HAS_METAL || RENDERING_HAS_VULKAN
class GPUAnimationCrowdTest : public GPUAnimationComputeFixture,
                            public testing::WithParamInterface<std::tuple<uint32, Kernel>>
{
protected:
    GraphicsAPI RequestedAPI() const override
    {
#if RENDERING_HAS_METAL
        return GraphicsAPI::Metal;
#else
        return GraphicsAPI::Vulkan;
#endif
    }

    void SetUp() override
    {
        GPUAnimationComputeFixture::SetUp();
        ASSERT_TRUE(m_Device);
        ASSERT_EQ(m_Device->GetAPI(), RequestedAPI());
        RecordProperty("graphics_api", m_Device->GetAPI() == GraphicsAPI::Metal ? "Metal" : "Vulkan");
        ASSERT_TRUE(BeginDeviceFrame());
    }

};

TEST_P(GPUAnimationCrowdTest, EveryBoundaryPaletteMatchesItsOwnSampleTime)
{
    const auto& [population, kernel] = GetParam();
    const Rig rig = CreateFingerRig(48u);
    constexpr float kSampleTime = 0.375f;
    const auto actual = DispatchProductionPass(kernel, rig, kSampleTime, false, {}, population);
    ASSERT_EQ(actual.size(), population * rig.JointCount * 16u);
    const std::array<uint32, 4u> probes{0u, (std::min)(population - 1u, 1023u),
        (std::min)(population - 1u, 1024u), population - 1u};
    for (uint32 instanceIndex : probes)
    {
        SCOPED_TRACE(testing::Message() << "instance " << instanceIndex);
        const auto expected = SampleCpuReference(rig.SkeletonId, rig.ClipIndex,
            kSampleTime + kCrowdInstanceTimeStep * static_cast<float>(instanceIndex));
        ASSERT_EQ(expected.size(), rig.JointCount * 16u);
        const size_t offset = static_cast<size_t>(instanceIndex) * expected.size();
        for (size_t element = 0u; element < expected.size(); ++element)
            ASSERT_NEAR(actual[offset + element], expected[element], kMatrixTolerance) << element;
    }
}

INSTANTIATE_TEST_SUITE_P(Crowds, GPUAnimationCrowdTest,
    testing::Combine(testing::Values(1024u, 1025u, 2000u), testing::Values(kDesktopKernel, kCompatKernel)),
    [](const testing::TestParamInfo<std::tuple<uint32, Kernel>>& information)
    {
        return "Instances" + std::to_string(std::get<0>(information.param)) + "_" +
            std::get<1>(information.param).Name;
    });

namespace
{
#if RENDERING_HAS_METAL
using NativeAnimationDevice = MetalDevice;
constexpr GraphicsAPI kNativeAnimationAPI = GraphicsAPI::Metal;
#else
using NativeAnimationDevice = VulkanDevice;
constexpr GraphicsAPI kNativeAnimationAPI = GraphicsAPI::Vulkan;
#endif

class RefusingAnimationDevice : public NativeAnimationDevice
{
public:
    BufferHandle CreateBuffer(const BufferDesc& description) override
    {
        const bool instanceBuffer = description.debugName
            && std::string_view(description.debugName).starts_with("GPUAnimInstances");
        if (instanceBuffer && RefuseInstanceCreation)
            return {};
        const BufferHandle buffer = NativeAnimationDevice::CreateBuffer(description);
        if (instanceBuffer && buffer.IsValid())
            m_InstanceBuffers.push_back(buffer);
        return buffer;
    }

    void* MapBuffer(BufferHandle buffer) override
    {
        if (RefuseInstanceMapping
            && std::find(m_InstanceBuffers.begin(), m_InstanceBuffers.end(), buffer) != m_InstanceBuffers.end())
            return nullptr;
        return NativeAnimationDevice::MapBuffer(buffer);
    }

    const RenderingDeviceCapabilities& GetCapabilities() const override
    {
        return m_HasStorageBindingLimit ? m_ReportedCapabilities : NativeAnimationDevice::GetCapabilities();
    }

    // Configure once after device initialization, before the store uses it.
    void SetStorageBindingLimit(size_t bytes)
    {
        m_ReportedCapabilities = NativeAnimationDevice::GetCapabilities();
        m_ReportedCapabilities.maxStorageBufferBindingSize = bytes;
        m_HasStorageBindingLimit = true;
    }

    bool RefuseInstanceCreation = false;
    bool RefuseInstanceMapping = false;

private:
    std::vector<BufferHandle> m_InstanceBuffers;
    RenderingDeviceCapabilities m_ReportedCapabilities;
    bool m_HasStorageBindingLimit = false;
};
} // namespace

class GPUAnimationAdmissionTest : public GPUAnimationComputeFixture
{
protected:
    GraphicsAPI RequestedAPI() const override { return kNativeAnimationAPI; }
    std::unique_ptr<IDevice> CreateDevice(const DeviceDesc&) override
    {
        return std::make_unique<RefusingAnimationDevice>();
    }
    void SetUp() override
    {
        GPUAnimationComputeFixture::SetUp();
        ASSERT_TRUE(m_Device);
        ASSERT_EQ(m_Device->GetAPI(), kNativeAnimationAPI);
        RecordProperty("graphics_api", m_Device->GetAPI() == GraphicsAPI::Metal ? "Metal" : "Vulkan");
    }
    void TearDown() override
    {
        FinishDeviceFrame();
        GPUAnimationComputeFixture::TearDown();
    }
    RefusingAnimationDevice& Device() { return static_cast<RefusingAnimationDevice&>(*m_Device); }

    void CheckGrowthRefusal(bool refuseMapping)
    {
        ASSERT_TRUE(m_DataStore.Initialize(m_Device.get()));
        m_DataStore.BeginFrame(0);
        const BufferHandle acceptedBuffer = m_DataStore.GetInstanceBuffer();
        const GPUAnimInstance instance = MakeInstance(0u, 0u, 0.375f);
        for (uint32 instanceIndex = 0; instanceIndex < 1024u; ++instanceIndex)
            ASSERT_TRUE(m_DataStore.TryAddInstance(instance));
        Device().RefuseInstanceCreation = !refuseMapping;
        Device().RefuseInstanceMapping = refuseMapping;
        EXPECT_FALSE(m_DataStore.TryAddInstance(instance));
        EXPECT_EQ(m_DataStore.GetInstanceCount(), 1024u);
        ASSERT_EQ(m_DataStore.GetInstanceBuffer(), acceptedBuffer);
        ASSERT_TRUE(m_DataStore.FlushInstances());
        Device().RefuseInstanceCreation = Device().RefuseInstanceMapping = false;
        const void* mapped = m_Device->MapBuffer(acceptedBuffer);
        ASSERT_NE(mapped, nullptr);
        const auto* rows = static_cast<const GPUAnimInstance*>(mapped);
        EXPECT_EQ(std::memcmp(&rows[0], &instance, sizeof(instance)), 0);
        EXPECT_EQ(std::memcmp(&rows[1023], &instance, sizeof(instance)), 0);
        m_Device->UnmapBuffer(acceptedBuffer);

        m_DataStore.BeginFrame(1);
        for (uint32 instanceIndex = 0; instanceIndex < 2000u; ++instanceIndex)
            ASSERT_TRUE(m_DataStore.TryAddInstance(instance));
        EXPECT_TRUE(m_DataStore.FlushInstances());
        EXPECT_EQ(m_DataStore.GetInstanceCount(), 2000u);
    }

};

TEST_F(GPUAnimationAdmissionTest, InvalidBufferInitializationLeavesNoCapacity)
{
    BufferRing ring;
    EXPECT_FALSE(ring.Initialize(nullptr, sizeof(GPUAnimInstance)));
    EXPECT_FALSE(ring.Initialize(m_Device.get(), 0u));
    EXPECT_EQ(ring.GetCapacity(), 0u);
    EXPECT_FALSE(ring.GetBuffer().IsValid());
    EXPECT_EQ(ring.GetBasePtr(), nullptr);
}

TEST_F(GPUAnimationAdmissionTest, FailedBufferCreationHasNoCapacityAndCanRetry)
{
    BufferRing ring;
    constexpr size_t kCapacityBytes = 64u;
    Device().RefuseInstanceCreation = true;
    EXPECT_FALSE(ring.Initialize(m_Device.get(), kCapacityBytes, BufferUsage::Storage,
        sizeof(GPUAnimInstance), "GPUAnimInstances.CreationRefusal"));
    EXPECT_EQ(ring.GetCapacity(), 0u) << "unmapped capacity must not satisfy admission";
    EXPECT_FALSE(ring.GetBuffer().IsValid());
    EXPECT_EQ(ring.GetBasePtr(), nullptr);
    const auto refused = ring.Allocate(sizeof(GPUAnimInstance));
    EXPECT_EQ(refused.Ptr, nullptr);
    EXPECT_EQ(refused.Size, 0u);
    Device().RefuseInstanceCreation = false;
    const bool initialized = ring.Initialize(m_Device.get(), kCapacityBytes, BufferUsage::Storage,
        sizeof(GPUAnimInstance), "GPUAnimInstances.CreationRetry");
    EXPECT_TRUE(initialized);
    if (initialized)
    {
        const auto allocation = ring.Allocate(sizeof(GPUAnimInstance));
        EXPECT_NE(allocation.Ptr, nullptr);
        EXPECT_EQ(allocation.Size, sizeof(GPUAnimInstance));
    }
    ring.Shutdown();
}

TEST_F(GPUAnimationAdmissionTest, RefusedReprovisionDoesNotAdmitRowsAndCanRecover)
{
    FrameBufferAllocator allocator;
    constexpr size_t kRowBytes = sizeof(GPUAnimInstance);
    ASSERT_TRUE(allocator.Initialize(m_Device.get(), kRowBytes, BufferUsage::Storage,
        1u, kRowBytes, "GPUAnimInstances.Reprovision", 2u * kRowBytes));
    allocator.BeginFrame(0);
    const BufferHandle oldBuffer = allocator.GetCurrentBuffer();
    // Invalidate the owned buffer before the allocator handles device loss.
    m_Device->UnmapBuffer(oldBuffer);
    m_Device->DestroyBuffer(oldBuffer);
    Device().RefuseInstanceCreation = true;
    allocator.ReprovisionAfterDeviceRebuild();
    allocator.BeginFrame(1);
    EXPECT_EQ(allocator.GetCapacity(), 0u);
    EXPECT_FALSE(allocator.GetCurrentBuffer().IsValid());
    EXPECT_FALSE(allocator.Reserve(kRowBytes));
    EXPECT_FALSE(allocator.Allocate(kRowBytes).IsValid());
    Device().RefuseInstanceCreation = false;
    EXPECT_TRUE(allocator.Reserve(kRowBytes));
    const auto allocation = allocator.Allocate(kRowBytes);
    EXPECT_TRUE(allocation.IsValid());
    EXPECT_TRUE(allocation.buffer.IsValid());
}

TEST_F(GPUAnimationAdmissionTest, FailedCreationPreservesAcceptedUploadAndRetriesNextFrame)
{
    CheckGrowthRefusal(false);
}

TEST_F(GPUAnimationAdmissionTest, FailedMappingPreservesAcceptedUploadAndRetriesNextFrame)
{
    CheckGrowthRefusal(true);
}

TEST_F(GPUAnimationAdmissionTest, StorageBindingLimitRefusesBeforeAppend)
{
    const size_t alignment = m_Device->GetCapabilities().minStorageBufferOffsetAlignment;
    const size_t bindingLimit = (std::max)(2u * sizeof(GPUAnimInstance), alignment);
    Device().SetStorageBindingLimit(bindingLimit);
    const uint32 acceptedCount = static_cast<uint32>(bindingLimit / sizeof(GPUAnimInstance));
    ASSERT_TRUE(m_DataStore.Initialize(m_Device.get()));
    m_DataStore.BeginFrame(0);
    const GPUAnimInstance instance = MakeInstance(0u, 0u, 0.5f);
    for (uint32 instanceIndex = 0; instanceIndex < acceptedCount; ++instanceIndex)
        ASSERT_TRUE(m_DataStore.TryAddInstance(instance));
    EXPECT_FALSE(m_DataStore.TryAddInstance(instance));
    EXPECT_EQ(m_DataStore.GetInstanceCount(), acceptedCount);
    EXPECT_TRUE(m_DataStore.FlushInstances());
}

TEST_F(GPUAnimationAdmissionTest, DispatchCountLimitAndOverflowRefuseBeforeAppend)
{
    ASSERT_TRUE(m_DataStore.Initialize(m_Device.get()));
    m_DataStore.BeginFrame(0);
    ASSERT_TRUE(m_DataStore.TryReserveInstanceCapacity(65535u));
    EXPECT_FALSE(m_DataStore.TryReserveInstanceCapacity(65536u));
    EXPECT_FALSE(m_DataStore.TryReserveInstanceCapacity(UINT32_MAX));
    EXPECT_EQ(m_DataStore.GetInstanceCount(), 0u);
}

TEST_F(GPUAnimationAdmissionTest, UnchangedFlushReusesRangeAndAppendedRowsPreserveEarlierUpload)
{
    ASSERT_TRUE(m_DataStore.Initialize(m_Device.get()));
    m_DataStore.BeginFrame(0);
    const GPUAnimInstance first = MakeInstance(1u, 2u, 0.25f);
    const GPUAnimInstance second = MakeInstance(3u, 4u, 0.75f);
    ASSERT_TRUE(m_DataStore.TryAddInstance(first));
    ASSERT_TRUE(m_DataStore.FlushInstances());
    const BufferHandle firstBuffer = m_DataStore.GetInstanceBuffer();
    const size_t firstOffset = m_DataStore.GetInstanceBufferOffset();
    for (uint32 retry = 0; retry < 2000u; ++retry)
    {
        ASSERT_TRUE(m_DataStore.FlushInstances());
        ASSERT_EQ(m_DataStore.GetInstanceBufferOffset(), firstOffset);
    }
    ASSERT_TRUE(m_DataStore.TryAddInstance(second));
    ASSERT_TRUE(m_DataStore.FlushInstances());
    const size_t secondOffset = m_DataStore.GetInstanceBufferOffset();
    EXPECT_EQ(m_DataStore.GetInstanceBuffer(), firstBuffer);
    EXPECT_GE(secondOffset, firstOffset + sizeof(first));
    const size_t alignment = (std::max)(sizeof(GPUAnimInstance),
        m_Device->GetCapabilities().minStorageBufferOffsetAlignment);
    EXPECT_EQ(secondOffset % alignment, 0u);
    const auto* mapped = static_cast<const unsigned char*>(m_Device->MapBuffer(firstBuffer));
    ASSERT_NE(mapped, nullptr);
    EXPECT_EQ(std::memcmp(mapped + firstOffset, &first, sizeof(first)), 0);
    EXPECT_EQ(std::memcmp(mapped + secondOffset, &first, sizeof(first)), 0);
    EXPECT_EQ(std::memcmp(mapped + secondOffset + sizeof(first), &second, sizeof(second)), 0);
    m_Device->UnmapBuffer(firstBuffer);
}

TEST_F(GPUAnimationAdmissionTest, UpdatePhaseReservationRotatesThroughPacingPlusOneSlots)
{
    ASSERT_TRUE(m_DataStore.Initialize(m_Device.get()));
    const uint32 slots = m_Device->GetFramesInFlight() + 1u;
    std::vector<BufferHandle> buffers;
    for (uint32 frameIndex = 0; frameIndex <= slots; ++frameIndex)
    {
        m_DataStore.BeginFrame(m_Device->GetFrameIndex());
        EXPECT_EQ(m_DataStore.GetInstanceCount(), 0u);
        EXPECT_FALSE(m_DataStore.IsGPUHandled(9u));
        ASSERT_TRUE(m_DataStore.TryReserveInstanceCapacity(2000u));
        const auto buffer = m_DataStore.GetInstanceBuffer();
        if (frameIndex < slots)
        {
            EXPECT_EQ(std::find(buffers.begin(), buffers.end(), buffer), buffers.end());
            buffers.push_back(buffer);
        }
        else
            EXPECT_EQ(buffer, buffers.front());
        ASSERT_TRUE(BeginDeviceFrame());
        ASSERT_TRUE(m_DataStore.TryAddInstance(MakeInstance(0u, 0u, static_cast<float>(frameIndex))));
        ASSERT_TRUE(m_DataStore.FlushInstances());
        m_DataStore.MarkGPUHandled(9u);
        FinishDeviceFrame();
    }
    m_Device->WaitForIdle();
}

TEST_F(GPUAnimationAdmissionTest, RefusedInstanceUsesCpuSamplingAndUploadsItsOwnPalette)
{
    RenderServices services;
    ASSERT_TRUE(services.Initialize(m_Device.get()));
    FramePools pools(m_Device.get());
    RG::RGFrame frame(m_Device.get(), &pools.Persistent, &pools.Transient, &pools.Upload);
    frame.BeginFrame(0);
    services.GetPerFrameWritePool().BeginFrame(0);
    services.GetSkinPaletteAtlas().BeginFrame(services.GetPerFrameWritePool());
    auto& store = services.GetGPUAnimationDataStore();
    store.BeginFrame(0);
    services.ScheduleGpuSkinningAndRetarget(frame);
    ASSERT_TRUE(services.IsComputeSkinningReady());
    const Rig rig = CreateFingerRig(48u);
    const uint32 skeletonIndex = store.EnsureSkeletonUploaded(rig.SkeletonId);
    const uint32 clipIndex = store.EnsureClipUploaded(rig.ClipIndex);
    AddTestInstances(store, skeletonIndex, clipIndex, 0.0f, rig.JointCount, 1024u);
    Device().RefuseInstanceCreation = true;
    const uint32 runtimeId = SkeletonStore::Instance().CreateRuntime(rig.SkeletonId);
    ASSERT_NE(runtimeId, 0u);
    ECS::World world;
    const auto entity = world.CreateEntity();
    Components::AnimatorRef animator{};
    animator.ClipIndex = rig.ClipIndex;
    animator.Time = 0.375f;
    animator.Flags = Components::AnimatorRef::kFlag_Paused;
    Components::SkeletonRef skeletonReference{};
    skeletonReference.skeletonId = rig.SkeletonId;
    skeletonReference.runtimeId = runtimeId;
    world.AddComponentImmediate(entity, animator);
    world.AddComponentImmediate(entity, skeletonReference);
    AnimationSystem animation(&services);
    animation.Update(world, 0.0f);
    EXPECT_EQ(store.GetInstanceCount(), 1024u);
    EXPECT_FALSE(store.IsGPUHandled(runtimeId));
    const auto expected = SampleCpuReference(rig.SkeletonId, rig.ClipIndex, animator.Time);
    auto* runtime = SkeletonStore::Instance().GetRuntime(runtimeId);
    ASSERT_NE(runtime, nullptr);
    ASSERT_EQ(runtime->CompactSkinMatrices.size(), expected.size());
    for (size_t element = 0; element < expected.size(); ++element)
        ASSERT_NEAR(runtime->CompactSkinMatrices[element], expected[element], kMatrixTolerance);
    SkinningUploadSystem upload(&services);
    upload.Update(world, 0.0f);
    ASSERT_GT(runtime->AtlasPaletteOffsetBones, SkinPaletteAtlas::kIdentityPaletteOffsetBones);
    const auto* mapped = static_cast<const float*>(m_Device->MapBuffer(services.GetSkinPaletteAtlas().GetBuffer()));
    ASSERT_NE(mapped, nullptr);
    const size_t rowOffset = static_cast<size_t>(runtime->AtlasPaletteOffsetBones) * BonePaletteLayout::kFloatsPerBone;
    std::vector<float> rows(mapped + rowOffset,
        mapped + rowOffset + rig.JointCount * BonePaletteLayout::kFloatsPerBone);
    m_Device->UnmapBuffer(services.GetSkinPaletteAtlas().GetBuffer());
    const auto actual = UnpackAtlas(rows, rig.JointCount);
    for (size_t element = 0; element < expected.size(); ++element)
        ASSERT_NEAR(actual[element], expected[element], kMatrixTolerance);
    Device().RefuseInstanceCreation = false;
    store.BeginFrame(1);
    services.GetPerFrameWritePool().BeginFrame(1);
    services.GetSkinPaletteAtlas().BeginFrame(services.GetPerFrameWritePool());
    constexpr float kRetryTime = 0.75f;
    world.GetComponentForWrite<Components::AnimatorRef>(entity)->Time = kRetryTime;
    const auto gpuExpected = SampleCpuReference(rig.SkeletonId, rig.ClipIndex, kRetryTime);
    ASSERT_NE(gpuExpected, expected) << "GPU retry must not match the prior CPU pose";
    animation.Update(world, 0.0f);
    EXPECT_TRUE(store.IsGPUHandled(runtimeId)) << "the same character must retry GPU animation";
    EXPECT_EQ(store.GetInstanceCount(), 1u);
    ASSERT_TRUE(BeginDeviceFrame());
    frame.BeginFrame(1);
    services.ScheduleGpuSkinningAndRetarget(frame);
    const uint64_t submissions = frame.SubmittedFrameCount();
    const RG::RGBuffer atlasResource = services.FrameRG().SkinPaletteAtlas;
    frame.AddPass("AdmissionRetry.HostRead", PassPhase::kEarlySetup + 1,
        [atlasResource](RG::RGPassBuilder& builder)
        {
            builder.Read(atlasResource);
            builder.PreventCulling();
        },
        [](RG::RGContext& context)
        {
            context.Cmd->Barrier(ResourceBarrier::CreateMemoryBarrier(
                static_cast<uint64_t>(PipelineStageMask::ComputeShader),
                static_cast<uint64_t>(PipelineStageMask::Host),
                static_cast<uint64_t>(ResourceAccessMask::ShaderWrite),
                static_cast<uint64_t>(ResourceAccessMask::HostRead)));
        });
    frame.MarkOutput(atlasResource);
    frame.Execute();
    EXPECT_EQ(frame.SubmittedFrameCount(), submissions + 1u);
    FinishDeviceFrame();
    const auto* gpuMapped = static_cast<const float*>(m_Device->MapBuffer(services.GetSkinPaletteAtlas().GetBuffer()));
    ASSERT_NE(gpuMapped, nullptr);
    const size_t gpuOffset = static_cast<size_t>(runtime->AtlasPaletteOffsetBones) * BonePaletteLayout::kFloatsPerBone;
    rows.assign(gpuMapped + gpuOffset,
        gpuMapped + gpuOffset + rig.JointCount * BonePaletteLayout::kFloatsPerBone);
    m_Device->UnmapBuffer(services.GetSkinPaletteAtlas().GetBuffer());
    const auto gpuActual = UnpackAtlas(rows, rig.JointCount);
    ASSERT_EQ(gpuActual.size(), gpuExpected.size());
    for (size_t element = 0; element < gpuExpected.size(); ++element)
        ASSERT_NEAR(gpuActual[element], gpuExpected[element], kMatrixTolerance);
    SkeletonStore::Instance().ReleaseRuntime(runtimeId);
    services.Shutdown();
}

#endif // Native backend admission tests.

class GPUAnimationPreparationRetryTest : public GPUAnimationComputeFixture,
                                         public ::testing::WithParamInterface<GraphicsAPI>
{
protected:
    GraphicsAPI RequestedAPI() const override { return GetParam(); }
    bool RequiresRequestedAPI() const override { return true; }
};

TEST_P(GPUAnimationPreparationRetryTest, EmptyRetryPreservesAtlasAndLaterWorkStillRuns)
{
    if (!m_Device)
        GTEST_SKIP() << "Requested native device unavailable";
    const Rig rig = CreateChainRig();
    constexpr float kSampleTime = 0.5f;
    const auto expected = SampleCpuReference(rig.SkeletonId, rig.ClipIndex, kSampleTime);
    const auto actual = DispatchProductionPass(kDesktopKernel, rig, kSampleTime, true);
    ASSERT_EQ(actual.size(), expected.size());
    for (size_t element = 0; element < expected.size(); ++element)
        EXPECT_NEAR(actual[element], expected[element], kMatrixTolerance) << element;
}

namespace
{
constexpr GraphicsAPI kPreparationRetryAPIs[] = {
    GraphicsAPI::Vulkan,
#ifdef __APPLE__
    GraphicsAPI::Metal,
#endif
};
}

INSTANTIATE_TEST_SUITE_P(NativeAPIs, GPUAnimationPreparationRetryTest, ::testing::ValuesIn(kPreparationRetryAPIs));

TEST_P(GPUAnimationComputeTest, SkinMatricesMatchCpuReference)
{
    if (!m_Device)
        GTEST_SKIP() << "Requested native device unavailable";

    const auto& [sampleCase, kernel] = GetParam();
    const Rig rig = sampleCase.CreateRig();
    const float time = sampleCase.Time;

    const std::vector<float> expected = SampleCpuReference(rig.SkeletonId, rig.ClipIndex, time);
    ASSERT_EQ(expected.size(), rig.JointCount * 16u);
    const std::vector<float> actual = DispatchProductionPass(kernel, rig, time);
    ASSERT_EQ(actual.size(), expected.size());

    for (size_t i = 0; i < expected.size(); ++i)
    {
        EXPECT_NEAR(actual[i], expected[i], kMatrixTolerance)
            << "joint " << (i / 16) << ", column " << (i % 16) / 4 << ", row " << (i % 4);
    }
}

// RestPose: at t = 0 the clip leaves bone 0 at the origin, so the output is the rest chain.
// AnimatedTranslation: at t = 0.5 bone 0 has moved +1 in X and its children follow.
// TwoHundredJoints and TwoHundredFiftySixJoints: finger rigs past 128 bones and at the kernel's
// 256-bone limit. DecorationNodesOutsideTheJointClosure: 300 nodes skinned through 101 bones.
// SixtyFourLevelChain: the kernel's level limit. ReversedNodeOrder: every parent after its child in
// node order, children first in the skin, and nodes outside the skin inside the closure.
INSTANTIATE_TEST_SUITE_P(SampleTimes, GPUAnimationComputeTest,
                         ::testing::Combine(
                             ::testing::Values(SampleCase{"RestPose", CreateChainRig, 0.0f},
                                               SampleCase{"AnimatedTranslation", CreateChainRig, 0.5f},
                                               SampleCase{"TwoHundredJoints", CreateTwoHundredJointRig, 0.5f},
                                               SampleCase{"TwoHundredFiftySixJoints", CreateTwoHundredFiftySixJointRig, 0.5f},
                                               SampleCase{"DecorationNodesOutsideTheJointClosure", CreateDecoratedRig, 0.5f},
                                               SampleCase{"SixtyFourLevelChain", CreateSixtyFourLevelChain, 0.5f},
                                               SampleCase{"ReversedNodeOrder", CreateReversedNodeOrderRig, 0.5f}),
                             ::testing::Values(kDesktopKernel, kCompatKernel)),
                         KernelCaseName);

// Without a device the store packs a skeleton and uploads nothing, so its answers need no GPU.
TEST(GPUAnimationDataStoreTest, RefusesSkeletonsTheKernelCannotHold)
{
    GPUAnimationDataStore store;
    const uint32 atBoneLimit = CreateRigSkeleton(FlatParents(256), EveryNodeAJoint(256));
    EXPECT_NE(store.EnsureSkeletonUploaded(atBoneLimit), GPUAnimationDataStore::kNotUploaded);
    EXPECT_EQ(store.EnsureSkeletonUploaded(CreateRigSkeleton(FlatParents(257), EveryNodeAJoint(257))),
              GPUAnimationDataStore::kNotUploaded);
    EXPECT_EQ(store.EnsureSkeletonUploaded(CreateRigSkeleton(ChainParents(65), EveryNodeAJoint(65))),
              GPUAnimationDataStore::kNotUploaded);
    EXPECT_EQ(store.EnsureSkeletonUploaded(kUnknownSkeletonId), GPUAnimationDataStore::kNotUploaded)
        << "a missing skeleton must not answer with another skeleton's slot";

    // A reload refills the skeleton under its id. The store answers from its cache until the
    // skeleton is invalidated, then packs it again and refuses it.
    auto& skeletons = SkeletonStore::Instance();
    skeletons.EnsureSizes(atBoneLimit, 300);
    FillRigSkeleton(skeletons.Get(atBoneLimit), FlatParents(300), EveryNodeAJoint(300));
    EXPECT_NE(store.EnsureSkeletonUploaded(atBoneLimit), GPUAnimationDataStore::kNotUploaded);
    store.InvalidateSkeleton(atBoneLimit);
    EXPECT_EQ(store.EnsureSkeletonUploaded(atBoneLimit), GPUAnimationDataStore::kNotUploaded);
}

TEST(GPUAnimationDataStoreTest, ReducedUploadUsesSelectedKernelLimitsAndKeepsFullPalette)
{
    const uint32 skeletonId = CreateRigSkeleton(FlatParents(300u), EveryNodeAJoint(300u));
    const auto* skeleton = SkeletonStore::Instance().Get(skeletonId);
    auto plan = MakeShared<Animation::BoneEvaluationPlan>();
    const uint32 requested[] = {1u, 299u};
    ASSERT_TRUE(plan->Build(*skeleton, requested));
    ASSERT_EQ(plan->Nodes().size(), 3u);
    GPUAnimationDataStore store;
    EXPECT_EQ(store.EnsureSkeletonUploaded(skeletonId), GPUAnimationDataStore::kNotUploaded);
    const uint32 reduced = store.EnsureSkeletonUploaded(skeletonId, plan);
    EXPECT_NE(reduced, GPUAnimationDataStore::kNotUploaded);
    EXPECT_EQ(store.EnsureSkeletonUploaded(skeletonId, plan), reduced);
    EXPECT_EQ(store.GetSkeletonHeaderUsedBytes(), sizeof(GPUSkeletonHeader));
    EXPECT_EQ(plan->JointCorrections().size(), 300u);
    store.InvalidateSkeleton(skeletonId);
    EXPECT_NE(store.EnsureSkeletonUploaded(skeletonId, plan), reduced);
}

TEST(GPUAnimationDataStoreTest, VariantCacheOwnsPlanUntilSkeletonInvalidation)
{
    const uint32 skeletonId = CreateTestSkeleton();
    const uint32 foreignId = CreateTestSkeleton();
    auto plan = MakeShared<Animation::BoneEvaluationPlan>();
    ASSERT_TRUE(plan->Build(*SkeletonStore::Instance().Get(skeletonId), {}));
    GPUAnimationDataStore store;
    EXPECT_EQ(store.EnsureSkeletonUploaded(foreignId, plan), GPUAnimationDataStore::kNotUploaded);
    const uint32 full = store.EnsureSkeletonUploaded(skeletonId);
    const uint32 reduced = store.EnsureSkeletonUploaded(skeletonId, plan);
    EXPECT_NE(full, reduced);
    EXPECT_EQ(store.EnsureSkeletonUploaded(skeletonId), full);
    std::weak_ptr<const Animation::BoneEvaluationPlan> lifetime = plan;
    plan.reset();
    EXPECT_FALSE(lifetime.expired());
    store.InvalidateSkeleton(skeletonId);
    EXPECT_TRUE(lifetime.expired());
}

TEST(GPUAnimationDataStoreTest, DeviceFreeShutdownReleasesVariantPlanOwnership)
{
    const uint32 skeletonId = CreateTestSkeleton();
    auto plan = MakeShared<Animation::BoneEvaluationPlan>();
    ASSERT_TRUE(plan->Build(*SkeletonStore::Instance().Get(skeletonId), {}));
    GPUAnimationDataStore store;
    ASSERT_NE(store.EnsureSkeletonUploaded(skeletonId, plan), GPUAnimationDataStore::kNotUploaded);
    std::weak_ptr<const Animation::BoneEvaluationPlan> lifetime = plan;
    plan.reset();
    ASSERT_FALSE(lifetime.expired());
    store.Shutdown();
    EXPECT_TRUE(lifetime.expired());
}
