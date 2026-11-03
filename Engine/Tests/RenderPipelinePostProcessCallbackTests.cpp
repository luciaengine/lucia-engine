#include <gtest/gtest.h>

#include "Engine/Rendering/Pipeline/Nodes/FullscreenShaderNode.h"
#include "Engine/Rendering/Pipeline/RenderPipeline.h"
#include "Engine/Rendering/RenderServices.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "TestDeviceHelper.h"

#include <memory>
#include <string>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Rendering;
using namespace GameEngine::Engine::Renderer;
using namespace GameEngine::Engine::Renderer::Pipeline;

namespace
{
// An effect may own a type name while inheriting the fullscreen color-filter contract.
class NamedFullscreenFilter final : public Nodes::FullscreenShaderNode
{
public:
    explicit NamedFullscreenFilter(std::vector<std::string>& order) : m_Order(order) {}
    const char* GetTypeName() const override { return "NamedFullscreenFilter"; }
    bool Initialize(std::string, std::string, std::string*) override { return true; }
    void DeclareForView(ViewDeclare&) override { m_Order.emplace_back("filter"); }
private:
    std::vector<std::string>& m_Order;
};
}

TEST(RenderPipelinePostProcessCallback, CompositesBeforeAFullscreenEffectWithItsOwnTypeName)
{
    auto device = CreateVulkanDeviceFast();
    if (!device) GTEST_SKIP() << "No Vulkan device available";
    {
        RenderServices services;
        ASSERT_TRUE(services.Initialize(device.get()));
        const auto camera = services.Views().AllocateCamera("NamedFilterCamera");
        const auto view = services.Views().AllocateView("NamedFilterView", camera);
        services.Views().SetViewRenderLayerMask(view, 1);
        services.Views().SetViewTargets(view, 0, 0, 0, ViewClearConfig{});
        RenderGraph::RGResourcePool persistent(device.get());
        RenderGraph::RGTransientPool transient(device.get());
        RenderGraph::RGUploadRing ring(device.get(), 2, 262144);
        RenderGraph::RGFrame frame(device.get(), &persistent, &transient, &ring);
        frame.BeginFrame(0);
        TextureDesc colorDesc{};
        colorDesc.width = colorDesc.height = 16;
        colorDesc.format = static_cast<uint32_t>(TextureFormat::RGBA8_UNORM);
        colorDesc.usage = static_cast<uint32_t>(TextureUsage::RenderTarget);
        const auto color = frame.ImportPersistentTexture("NamedFilter.Color", colorDesc);
        std::vector<std::string> order;
        RenderPipelineNodeRegistry registry;
        ASSERT_TRUE(registry.Register("DepthPixelation", []
            { return std::make_unique<Nodes::DepthPixelationNode>(); }, true));
        ASSERT_TRUE(registry.Register("NamedFullscreenFilter", [&order]
            { return std::make_unique<NamedFullscreenFilter>(order); }, true));
        RenderPipelineInstance pipeline(services, registry);
        RenderPipelineBlueprint blueprint;
        blueprint.pipelineName = "NamedFilterCallback";
        RenderPipelineBlueprint::Pass pixelation;
        pixelation.id = "Bands";
        pixelation.type = "DepthPixelation";
        pixelation.enabled = true;
        pixelation.perView = true;
        pixelation.passJson = R"({"type":"DepthPixelation","source":"View.Color",
            "output":"AfterBands","shaderPkg":"Shaders/depth_pixelation_compose.shaderpkg"})";
        blueprint.passes.push_back(pixelation);
        RenderPipelineBlueprint::Pass pass;
        pass.id = "Filter";
        pass.type = "NamedFullscreenFilter";
        pass.enabled = true;
        pass.perView = true;
        pass.passJson = R"({"type":"NamedFullscreenFilter"})";
        blueprint.passes.push_back(pass);
        pipeline.SetBlueprint(blueprint);
        pipeline.SetBeforePostProcessCallback(&order, [&order, color](ViewDeclare& declare)
        {
            EXPECT_TRUE(declare.ViewColor.IsValid());
            // The real band-composition node must have declared before this
            // callback; a failed/missing node cannot make the opt-out test pass.
            EXPECT_EQ(declare.ResolveTexture("AfterBands").Id, color.Id);
            order.emplace_back("ui");
        });
        const ViewTargetsRG targets{view, color, {}, {}};
        pipeline.Declare(frame, std::span<const ViewTargetsRG>(&targets, 1), services.Views().GetViews());
        EXPECT_EQ(order, (std::vector<std::string>{"ui", "filter"}));
    }
    device->Shutdown();
}
