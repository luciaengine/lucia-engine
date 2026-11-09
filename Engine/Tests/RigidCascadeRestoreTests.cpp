// A rejected depth-layer copy must clear the composed cascade, while a
// successful copy must preserve its rigid depth. Read the real GPU image;
// recording calls alone cannot prove the layer contains the right values.
#include "Engine/Rendering/DepthDrawRecorder.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/ViewReadbackUtils.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Core/RenderGraph/RGResourcePool.h"
#include "Rendering/Core/RenderGraph/RGTransientPool.h"
#include "Rendering/Core/RenderGraph/RGUploadRing.h"
#include "TestDeviceHelper.h"

#include <gtest/gtest.h>

#include <cstring>

using namespace GameEngine;
using namespace GameEngine::Rendering;
using namespace GameEngine::Engine::Renderer;

namespace
{
constexpr uint32_t kSize = 16u;
constexpr float kOtherLayerDepth = 0.25f;
constexpr float kRigidDepth = 0.6f;
constexpr float kStaleDepth = 0.95f;

void ExpectDepth(const ViewReadbackResult& result, float expected)
{
    ASSERT_EQ(result.width, kSize);
    ASSERT_EQ(result.height, kSize);
    ASSERT_EQ(result.pixels.size(), kSize * kSize * sizeof(float));
    for (size_t offset = 0; offset < result.pixels.size(); offset += sizeof(float))
    {
        float depth = -1.0f;
        std::memcpy(&depth, result.pixels.data() + offset, sizeof(float));
        ASSERT_FLOAT_EQ(depth, expected) << "pixel " << offset / sizeof(float);
    }
}

void RunRestore(bool expectCopy)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    RenderServices services;
    ASSERT_TRUE(services.Initialize(device.get()));
    RenderGraph::RGResourcePool persistent(device.get());
    RenderGraph::RGTransientPool transient(device.get());
    RenderGraph::RGUploadRing ring(device.get(), 2u, 65536u);
    RenderGraph::RGFrame frame(device.get(), &persistent, &transient, &ring);
    frame.BeginFrame(1u);

    TextureDesc desc{};
    desc.width = kSize;
    desc.height = kSize;
    desc.arrayLayers = 2u;
    desc.format = static_cast<uint32_t>(TextureFormat::D32_FLOAT);
    desc.usage = static_cast<uint32_t>(TextureUsage::DepthStencil | TextureUsage::ShaderResource |
                                       TextureUsage::TransferSrc | TextureUsage::TransferDst);
    desc.flags = TextureCreateFlags::ForceArrayView;
    const auto composed = frame.ImportPersistentTexture("RigidRestore.Composed", desc);
    // The failure is real: a one-layer source cannot supply layer 1. The
    // command list rejects it before issuing any Vulkan copy command.
    desc.arrayLayers = expectCopy ? 2u : 1u;
    const auto rigid = frame.ImportPersistentTexture("RigidRestore.Source", desc);
    ASSERT_TRUE(composed.IsValid());
    ASSERT_TRUE(rigid.IsValid());

    auto seed = [&](const char* name, RenderGraph::RGTexture target, uint32_t layer, float value)
    {
        frame.AddPass(name, PassPhase::kEarlySetup,
            [&](RenderGraph::RGPassBuilder& builder)
            {
                RenderGraph::RGAttachmentOps ops{};
                ops.Load = RenderGraph::RGLoadOp::Clear;
                ops.Store = RenderGraph::RGStoreOp::Store;
                ops.Clear.Depth = value;
                builder.AttachDepth(target, ops, RenderGraph::RGDepthAccess::ReadWrite,
                                    {.BaseLayer = layer, .LayerCount = 1u});
                // A declared copy overwrites this seed only if it succeeds.
                builder.PreventCulling();
            }, [](RenderGraph::RGContext&) {});
    };
    seed("RigidRestore.SeedOther", composed, 0u, kOtherLayerDepth);
    seed("RigidRestore.SeedStale", composed, 1u, kStaleDepth);
    seed("RigidRestore.SeedSource", rigid, expectCopy ? 1u : 0u, kRigidDepth);

    bool copied = false;
    frame.AddPass("RigidRestore.Copy", PassPhase::kEarlySetup,
        [&](RenderGraph::RGPassBuilder& builder)
        {
            builder.Read(rigid, RenderGraph::RGTextureRead::CopySrc,
                         {.BaseLayer = expectCopy ? 1u : 0u, .LayerCount = 1u});
            builder.Write(composed, RenderGraph::RGTextureWrite::CopyDst,
                          {.BaseLayer = 1u, .LayerCount = 1u});
        },
        [&](RenderGraph::RGContext& context)
        {
            copied = context.Cmd->CopyDepthArrayLayer(
                context.GetTexture(rigid), context.GetTexture(composed), 1u);
        });

    // No live casters: this exercises the depth composition without allowing
    // a draw to hide an uncleared stale texel. This test isolates attachment
    // contents; the full-caster replay needs a separate populated scene.
    DepthOnlyPassParamsRG params{};
    params.PassType = DepthPassType::ShadowCascade;
    params.Casters = CascadeCasters::All;
    params.CascadeIndex = 1u;
    auto* prepared = AllocatePreparedDepthOnlyPass(frame, params);
    frame.AddPass("RigidRestore.Compose", PassPhase::kEarlySetup,
        [&](RenderGraph::RGPassBuilder& builder)
        {
            builder.Prepare([&](RenderGraph::RGPrepareContext& context)
            {
                return PrepareDepthOnlyPass(context, *prepared, services.MakeDepthDrawServices());
            });
            RenderGraph::RGAttachmentOps ops{};
            ops.Load = RenderGraph::RGLoadOp::Load;
            ops.Store = RenderGraph::RGStoreOp::Store;
            builder.AttachDepth(composed, ops, RenderGraph::RGDepthAccess::ReadWrite,
                                {.BaseLayer = 1u, .LayerCount = 1u});
        },
        [&](RenderGraph::RGContext& context)
        {
            RecordPreparedCascadeDepth(context, *prepared, context.GetTexture(composed), 1u, copied);
        });

    const auto other = RequestTextureSubresourceReadbackRG(
        device.get(), frame, composed, 0u, 0u, 0u, 0u, kSize, kSize, "RigidRestore.OtherLayer");
    const auto changed = RequestTextureSubresourceReadbackRG(
        device.get(), frame, composed, 0u, 1u, 0u, 0u, kSize, kSize, "RigidRestore.ChangedLayer");
    ASSERT_TRUE(other);
    ASSERT_TRUE(changed);
    frame.Execute();
    OnFrameSubmittedReadbacksRG(frame, frame.SubmissionToken());
    device->WaitForIdle();
    ASSERT_EQ(copied, expectCopy);
    ViewReadbackResult otherResult{};
    ViewReadbackResult changedResult{};
    ASSERT_TRUE(other->TryGet(otherResult));
    ASSERT_TRUE(changed->TryGet(changedResult));
    ExpectDepth(otherResult, kOtherLayerDepth);
    ExpectDepth(changedResult, expectCopy ? kRigidDepth : 0.0f);
    services.Shutdown();
}
} // namespace

TEST(RigidCascadeRestore, FailedCopyClearsOnlyTheComposedLayer)
{
    RunRestore(false);
}

TEST(RigidCascadeRestore, SuccessfulCopyPreservesRigidDepth)
{
    RunRestore(true);
}
