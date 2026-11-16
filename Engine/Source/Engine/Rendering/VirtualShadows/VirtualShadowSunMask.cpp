#include "Engine/Rendering/VirtualShadows/VirtualShadowSunMask.h"

#include "Engine/Rendering/Pipeline/Nodes/PipelineNodeUtils.h"
#include "Engine/Rendering/VirtualShadows/VirtualShadowViewInputs.h"
#include "Logger/Logger.h"
#include "Mathematics/MatrixOps.h"
#include "Mathematics/Vector4.h"
#include "Rendering/Camera/CameraTypes.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/NamedDescriptorWriter.h"
#include "Rendering/Core/PassPhase.h"
#include "Rendering/Materials/MaterialHelper.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"

#include <format>
#include <string>

namespace GameEngine::Engine::Renderer
{
namespace
{
namespace RenderGraph = Rendering::RenderGraph;

constexpr const char* kShader = "Shaders/virtual_shadow_sun_mask.shaderpkg";
constexpr uint32_t kPushConstantBudget = 128;
constexpr uint32_t kGroupSize = 8; // virtual_shadow_sun_mask.comp local_size_xy

// Mirrors VirtualShadowSunMaskParams in virtual_shadow_sun_mask.comp (std140, 16-byte members only).
struct SunMaskParams
{
    Mathematics::Matrix4x4 ClipToPosition;
    Mathematics::Vector4 ViewDepth;
    Mathematics::Vector4 CameraPosition;
    Mathematics::Vector4 TowardLight;
    int32_t Viewport[4]; // ivec4: origin xy, size zw in pixels
};
static_assert(sizeof(SunMaskParams) == 128, "must match VirtualShadowSunMaskParams");

constexpr uint32_t kMaskUsage = static_cast<uint32_t>(Rendering::TextureUsage::UnorderedAccess) |
                                static_cast<uint32_t>(Rendering::TextureUsage::ShaderResource);

SunMaskParams BuildParams(const VirtualShadowSunMaskRequest& request, uint32_t width, uint32_t height)
{
    const Rendering::CameraData& camera = *request.Camera;
    SunMaskParams params{};
    params.ClipToPosition = Mathematics::Inverse(camera.viewProjRel);
    // Row 2 of the view matrix gives the view-space depth (the forward lighting's linearDepth).
    const Mathematics::Matrix4x4& view = camera.viewRel;
    params.ViewDepth = Mathematics::Vector4(view[0][2], view[1][2], view[2][2], view[3][2]);
    const Mathematics::Matrix4x4 viewToPosition = Mathematics::Inverse(view);
    params.CameraPosition =
        Mathematics::Vector4(viewToPosition[3][0], viewToPosition[3][1], viewToPosition[3][2], 1.0f);
    params.TowardLight = Mathematics::Vector4(request.TowardLight.x, request.TowardLight.y, request.TowardLight.z, 0.0f);
    const ViewLetterbox& letterbox = request.Letterbox;
    const bool letterboxed = letterbox.active && letterbox.width > 0 && letterbox.height > 0;
    params.Viewport[0] = letterboxed ? static_cast<int32_t>(letterbox.x) : 0;
    params.Viewport[1] = letterboxed ? static_cast<int32_t>(letterbox.y) : 0;
    params.Viewport[2] = static_cast<int32_t>(letterboxed ? letterbox.width : width);
    params.Viewport[3] = static_cast<int32_t>(letterboxed ? letterbox.height : height);
    return params;
}
} // namespace

VirtualShadowSunMask::VirtualShadowSunMask(Rendering::IDevice* device) : m_Device(device) {}

VirtualShadowSunMask::~VirtualShadowSunMask()
{
    if (m_Device && m_Sampler.IsValid())
        m_Device->DestroySampler(m_Sampler);
}

void VirtualShadowSunMask::OnDeviceRebuilt(Rendering::IDevice* device)
{
    m_Sampler = {};
    m_Device = device;
}

bool VirtualShadowSunMask::Load()
{
    if (!m_Device)
        return false;
    if (!m_Pipeline.IsValid())
    {
        Rendering::ShaderPackage package{};
        std::string error;
        if (!Rendering::LoadShaderPkg(kShader, m_Device->PreferredShaderSource(), package, &error))
        {
            if (!m_LoggedLoadFailure)
                Logger::Log::Warning("VirtualShadowSunMask: {}", error);
            m_LoggedLoadFailure = true;
            return false;
        }
        auto stage = package.stageBytes.find("cs");
        if (stage == package.stageBytes.end() || stage->second.empty())
            return false;
        m_Meta = std::make_unique<Rendering::ShaderMeta>(std::move(package.meta));
        Rendering::ComputePipelineDesc desc{};
        desc.ComputeShader = std::make_shared<const std::vector<uint8_t>>(std::move(stage->second));
        desc.DebugName = "VirtualShadowSunMask";
        if (!Rendering::MaterialHelper::ApplyShaderMetaToComputeDesc(
                *m_Device, *m_Meta, desc, Rendering::MaterialBuilder::MergeMode::Auto, {true, kPushConstantBudget},
                [this](uint32_t set, Rendering::DescriptorSetLayoutDesc& layout)
                {
                    if (set == 0)
                        m_Layout = layout;
                },
                &error))
        {
            if (!m_LoggedLoadFailure)
                Logger::Log::Error("VirtualShadowSunMask: {}", error);
            m_LoggedLoadFailure = true;
            return false;
        }
        m_Pipeline = m_Device->InternComputePipeline(std::move(desc));
    }
    if (!m_Pipeline.IsValid() || !m_Device->GetOrCreateComputePipeline(m_Pipeline).IsValid())
        return false;
    if (!m_Sampler.IsValid())
        m_Sampler = m_Device->CreateSampler(Rendering::SamplerDesc::PointClamp("VirtualShadowSunMask"));
    return m_Sampler.IsValid();
}

RenderGraph::RGTexture VirtualShadowSunMask::Declare(RenderGraph::RGFrame& frame,
                                                     const VirtualShadowSunMaskRequest& request)
{
    if (!m_Device || !request.Depth.IsValid() || !request.Camera || !request.Inputs || !request.Inputs->IsValid() ||
        !request.ShadowData.IsValid())
        return {};
    // A thumbnail camera before its first update has a zero projection, and its inverse is NaN.
    if (request.Camera->proj[0][0] == 0.0f || request.Camera->proj[1][1] == 0.0f)
        return {};
    const auto& source = frame.Graph().ResourceDesc(request.Depth.Id);
    if (source.Width == 0 || source.Height == 0 || source.SampleCount > 1)
        return {};
    if (!m_Device->IsTextureFormatSupported(Rendering::TextureFormat::R32_UINT, kMaskUsage) || !Load())
        return {};

    char name[64]{};
    std::format_to_n(name, sizeof(name) - 1, "VirtualShadowSunMask.View{}", request.ViewId);
    Rendering::TextureDesc desc{};
    desc.width = source.Width;
    desc.height = source.Height;
    desc.depth = desc.mipLevels = desc.arrayLayers = desc.sampleCount = 1;
    desc.format = static_cast<uint32_t>(Rendering::TextureFormat::R32_UINT);
    desc.usage = kMaskUsage;
    desc.debugName = name;
    // A persistent import: the world pass binds the mask by name at declaration time.
    const RenderGraph::RGTexture mask = frame.ImportPersistentTexture(name, desc);
    if (!mask.IsValid() || !frame.PhysicalTexture(mask).IsValid())
        return {};
    auto params = frame.AllocUpload<SunMaskParams>();
    if (!params.Valid())
        return {};
    *params.Ptr = BuildParams(request, source.Width, source.Height);

    const VirtualShadowViewInputs inputs = *request.Inputs;
    const Pipeline::PipelineBufferBindingRG shadowData = request.ShadowData;
    const RenderGraph::RGTexture depth = request.Depth;
    frame.AddComputePass(name, Rendering::PassPhase::kDefault,
        [this, depth, inputs, shadowData, mask](RenderGraph::RGPassBuilder& pass)
        {
            pass.UsePipeline(m_Pipeline);
            pass.Read(depth, RenderGraph::RGTextureRead::Sampled);
            pass.Read(inputs.Atlas, RenderGraph::RGTextureRead::Sampled);
            pass.Read(inputs.PageTable, RenderGraph::RGBufferRead::Storage);
            if (shadowData.Graph.IsValid())
                pass.Read(shadowData.Graph, RenderGraph::RGBufferRead::Uniform);
            pass.Write(mask, RenderGraph::RGTextureWrite::Storage);
        },
        [this, depth, mask, inputs, shadowData, paramsBuffer = params.Buffer, paramsOffset = params.Offset,
         width = source.Width, height = source.Height](RenderGraph::RGContext& context)
        {
            auto* device = context.GetDevice();
            auto* command = context.Cmd;
            if (!device || !command)
                return;
            const auto pipeline = context.GetOrCreatePipelineVariant(m_Pipeline);
            const auto depthTexture = context.GetTexture(depth);
            const auto maskTexture = context.GetTexture(mask);
            const auto atlas = context.GetTexture(inputs.Atlas);
            const auto pageTable = context.GetBuffer(inputs.PageTable);
            if (!pipeline.IsValid() || !depthTexture.IsValid() || !maskTexture.IsValid() || !atlas.IsValid() ||
                !pageTable.IsValid())
                return;
            Rendering::DescriptorSetDesc setDesc{};
            setDesc.layout = m_Layout;
            setDesc.transient = true;
            setDesc.debugName = "VirtualShadowSunMask.Set0";
            const auto set = device->CreateDescriptorSet(setDesc);
            Rendering::NamedDescriptorWriter writer(device, set, *m_Meta, 0);
            const bool bound =
                writer.TryAddCombinedImageSampler("uDepth", depthTexture, m_Sampler) &&
                writer.TryAddUniformBuffer("VirtualShadowSunMaskParams", paramsBuffer, paramsOffset,
                                           sizeof(SunMaskParams)) &&
                writer.TryAddUniformBuffer("ShadowData", shadowData.Buffer, shadowData.Offset, shadowData.Size) &&
                writer.TryAddStorageBuffer("VirtualShadowPageTable", pageTable, 0, inputs.PageTableBytes) &&
                writer.TryAddStorageBuffer("VirtualShadowProjections", inputs.Projections, inputs.ProjectionsOffset,
                                           inputs.ProjectionsBytes) &&
                writer.TryAddCombinedImageSampler("ge_virtualShadowDepth", atlas, inputs.DepthSampler);
            if (!bound)
            {
                Logger::Log::Error("VirtualShadowSunMask: {} lost a set-0 binding; the pass is skipped", kShader);
                return;
            }
            if (writer.Has("ge_virtualShadowDepthCompare"))
                writer.TryAddCombinedImageSampler("ge_virtualShadowDepthCompare", atlas, inputs.CompareSampler);
            writer.Flush();
            Pipeline::Nodes::Detail::BindStorageImageByName(device, set, *m_Meta, "uMask", maskTexture);
            command->SetMarker("VirtualShadowSunMask");
            command->SetPipeline(pipeline);
            command->BindDescriptorSet(0, set, pipeline);
            command->Dispatch((width + kGroupSize - 1) / kGroupSize, (height + kGroupSize - 1) / kGroupSize, 1);
        });
    return mask;
}
} // namespace GameEngine::Engine::Renderer
