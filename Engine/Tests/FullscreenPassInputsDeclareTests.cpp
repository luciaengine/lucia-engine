// AddFullscreenPass's input caps. The widest caller, the ocean's underwater overlay with shadowed god rays,
// binds six buffers (params, shadow data, ViewParams, ribbons, materials, ExposureHistory). Over a cap the
// pass is refused whole. Part of RenderPipelineDeclareTests.
#include "Rendering/Core/Device.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Core/RenderGraph/RGFullscreen.h"
#include "Rendering/Core/RenderGraph/RGResourcePool.h"
#include "Rendering/Core/RenderGraph/RGTransientPool.h"
#include "Rendering/Core/RenderGraph/RGUploadRing.h"
#include "SharedHeadlessDevice.h"
#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <vector>

using namespace GameEngine::Rendering;

namespace
{
// The underwater overlay's buffer inputs with shadowed god rays (OceanUnderwater::DeclareForView).
constexpr uint32_t kUnderwaterShadowedBufferInputs = 6;

TextureDesc FullscreenTargetDesc()
{
    TextureDesc desc{};
    desc.width = desc.height = 4;
    desc.depth = desc.mipLevels = desc.arrayLayers = desc.sampleCount = 1;
    desc.format = static_cast<uint32_t>(TextureFormat::R16G16B16A16_FLOAT);
    desc.usage = static_cast<uint32_t>(TextureUsage::RenderTarget | TextureUsage::ShaderResource);
    return desc;
}

struct FullscreenInputsFrame
{
    IDevice* Device = nullptr;
    std::unique_ptr<RenderGraph::RGResourcePool> Persistent;
    std::unique_ptr<RenderGraph::RGTransientPool> Transient;
    std::unique_ptr<RenderGraph::RGUploadRing> Ring;
    std::unique_ptr<RenderGraph::RGFrame> Frame;

    bool Up()
    {
        Device = SharedHeadlessDevice();
        if (!Device)
            return false;
        Persistent = std::make_unique<RenderGraph::RGResourcePool>(Device);
        Transient = std::make_unique<RenderGraph::RGTransientPool>(Device);
        Ring = std::make_unique<RenderGraph::RGUploadRing>(Device, 2, 65536);
        Frame = std::make_unique<RenderGraph::RGFrame>(Device, Persistent.get(), Transient.get(), Ring.get());
        Frame->BeginFrame(0);
        return true;
    }

    // `count` buffer inputs, each declaring a read of its own persistent storage buffer.
    RenderGraph::RGPass Declare(uint32_t count, std::set<std::string>& outBufferNames)
    {
        const TextureDesc td = FullscreenTargetDesc();
        const RenderGraph::RGTexture target = Frame->ImportPersistentTexture("FPI.Target", td);

        std::vector<RenderGraph::RGFullscreenBufferInput> inputs(count);
        for (uint32_t i = 0; i < count; ++i)
        {
            const std::string name = "FPI.Buffer" + std::to_string(i);
            BufferDesc bd{};
            bd.size = 16;
            bd.usage = static_cast<uint32_t>(BufferUsage::Storage);
            bd.memoryUsage = BufferMemoryUsage::DeviceLocal;
            bd.debugName = name.c_str();
            const RenderGraph::RGBuffer buffer = Frame->ImportPersistentBuffer(name.c_str(), bd);
            inputs[i].Binding = i;
            inputs[i].Buffer = Frame->PhysicalBuffer(buffer);
            inputs[i].Size = 16;
            inputs[i].IsStorage = true;
            inputs[i].Declare = buffer;
        }
        RenderGraph::RGFullscreenDesc desc{};
        desc.Name = "FPI.Pass";
        desc.Target = target;
        desc.Buffers = inputs;
        desc.Ops.Load = RenderGraph::RGLoadOp::DontCare;
        desc.Ops.Store = RenderGraph::RGStoreOp::Store;
        const RenderGraph::RGPass declared = RenderGraph::AddFullscreenPass(*Frame, desc);
        const RenderGraph::RGGraph& g = Frame->Graph();
        for (const RenderGraph::RGAccessRecord& a : g.Accesses())
        {
            const char* passName = g.PassName(a.Pass);
            const char* resourceName = g.ResourceName(a.Resource);
            if (passName && resourceName && std::string(passName) == "FPI.Pass" &&
                std::string(resourceName).rfind("FPI.Buffer", 0) == 0)
                outBufferNames.insert(resourceName);
        }
        return declared;
    }

    void Down()
    {
        Frame.reset();
        Device->WaitForIdle();
    }
};
} // namespace

TEST(FullscreenPassInputs, TheUnderwaterOverlaysSixBuffersAreAllDeclared)
{
    FullscreenInputsFrame f;
    if (!f.Up())
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_GE(RenderGraph::kMaxFullscreenBufferInputs, kUnderwaterShadowedBufferInputs)
        << "the cap must hold the underwater overlay's shadowed inputs";
    std::set<std::string> read;
    const RenderGraph::RGPass pass = f.Declare(kUnderwaterShadowedBufferInputs, read);
    EXPECT_TRUE(pass.IsValid());
    EXPECT_EQ(read.size(), kUnderwaterShadowedBufferInputs) << "every buffer input declares its read";
    f.Down();
}

TEST(FullscreenPassInputs, ADeclarationOverTheCapIsRefusedWhole)
{
    FullscreenInputsFrame f;
    if (!f.Up())
        GTEST_SKIP() << "No Vulkan device available";
    std::set<std::string> read;
    const RenderGraph::RGPass pass = f.Declare(RenderGraph::kMaxFullscreenBufferInputs + 1, read);
    EXPECT_FALSE(pass.IsValid()) << "over the cap no pass is declared";
    EXPECT_TRUE(read.empty()) << "no input of a refused pass is half-declared";
    f.Down();
}

TEST(FullscreenPassInputs, TheSecondOutputIsAttachedAndDeclaredWritten)
{
    FullscreenInputsFrame f;
    if (!f.Up())
        GTEST_SKIP() << "No Vulkan device available";
    RenderGraph::RGFullscreenDesc desc{};
    desc.Name = "FPI.TwoOutputs";
    desc.Target = f.Frame->CreateTexture("FPI.Color", FullscreenTargetDesc());
    desc.SecondTarget = f.Frame->CreateTexture("FPI.Delta", FullscreenTargetDesc());
    const RenderGraph::RGPass pass = RenderGraph::AddFullscreenPass(*f.Frame, desc);
    ASSERT_TRUE(pass.IsValid());
    EXPECT_TRUE(f.Frame->Graph().HasWriteAccess(pass.Id, desc.Target.Id));
    EXPECT_TRUE(f.Frame->Graph().HasWriteAccess(pass.Id, desc.SecondTarget.Id));
    bool secondAttached = false;
    for (const auto& attachment : f.Frame->Attachments())
        secondAttached |= attachment.Pass == pass.Id && attachment.Slot == 1u &&
                          attachment.Tex == desc.SecondTarget.Id && !attachment.IsDepth;
    EXPECT_TRUE(secondAttached);
    f.Down();
}

TEST(FullscreenPassInputs, ASecondOutputWithADifferentExtentOrSampleCountIsRefusedWhole)
{
    FullscreenInputsFrame f;
    if (!f.Up())
        GTEST_SKIP() << "No Vulkan device available";
    const auto target = f.Frame->CreateTexture("FPI.Color", FullscreenTargetDesc());
    for (uint32_t mismatch = 0; mismatch < 3u; ++mismatch)
    {
        SCOPED_TRACE(mismatch);
        TextureDesc second = FullscreenTargetDesc();
        if (mismatch == 0u)
            second.width *= 2u;
        else if (mismatch == 1u)
            second.height *= 2u;
        else
            second.sampleCount *= 2u;
        RenderGraph::RGFullscreenDesc desc{};
        desc.Name = "FPI.MismatchedOutputs";
        desc.Target = target;
        const std::string name = "FPI.Second" + std::to_string(mismatch);
        desc.SecondTarget = f.Frame->CreateTexture(name.c_str(), second);
        const auto pass = RenderGraph::AddFullscreenPass(*f.Frame, desc);
        EXPECT_FALSE(pass.IsValid());
        EXPECT_EQ(f.Frame->Graph().PassCount(), 0u);
        EXPECT_TRUE(f.Frame->Attachments().empty());
    }
    f.Down();
}
