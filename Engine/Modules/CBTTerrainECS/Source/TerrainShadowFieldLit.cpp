#include "CBTTerrainECS/TerrainShadowFieldLit.h"

#include "CBTTerrain/TerrainShadowGrid.h"
#include "Engine/Rendering/Pipeline/Nodes/PipelineNodeUtils.h"
#include "Engine/Rendering/RenderServices.h"
#include "Logger/Logger.h"
#include "Mathematics/Vector4.h"
#include "Rendering/Core/BarrierMapping.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/NamedDescriptorWriter.h"
#include "Rendering/Core/PassPhase.h"
#include "Rendering/Materials/MaterialHelper.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"

#include <algorithm>
#include <bit>
#include <span>
#include <string>

namespace GameEngine::CBTTerrainECS
{

namespace
{
namespace RG = Rendering::RenderGraph;

// After the clearance bake (TerrainShadowBake, kEarlySetup + 2), ahead of every shading pass.
constexpr int32_t kRebuildPhase = Rendering::PassPhase::kEarlySetup + 3;
constexpr uint32_t kGroupSide = 8u; // terrain_shadow_field_lit.comp local size

// GLSL mirror: TerrainShadowFieldLitParams in terrain_shadow_field_lit.comp (std140).
struct LevelParamsUBO
{
    uint32_t Level[4]; // the level written; the map's samples per line and lines in use; unused
    Mathematics::Vector4 Map; // the map's texel (meters between samples); unused
};
static_assert(sizeof(LevelParamsUBO) == 32, "must match terrain_shadow_field_lit.comp's TerrainShadowFieldLitParams");

// Writes the storage view `view` into the set-0 binding the shader names `name`.
bool BindStorageViewByName(Rendering::IDevice& device, Rendering::DescriptorSetHandle set0,
                           const Rendering::ShaderMeta& meta, const std::string& name,
                           Rendering::TextureViewHandle view)
{
    uint32_t binding = 0;
    Rendering::DescriptorType reflected{};
    if (!view.IsValid() ||
        !Engine::Renderer::Pipeline::Nodes::Detail::TryGetSet0BindingByName(meta, name, binding, reflected))
        return false;
    Rendering::DescriptorSetUpdate u{};
    u.binding = binding;
    u.type = Rendering::DescriptorType::StorageImage;
    u.textureViews = {view};
    device.UpdateDescriptorSet(set0, u);
    return true;
}

void RecordLitField(RG::RGContext& context, RG::RGTexture map, Rendering::ComputePipelineId pipelineId,
                    const Rendering::ShaderMeta& metadata, const Rendering::DescriptorSetLayoutDesc& layout,
                    std::span<const Rendering::TextureViewHandle> levelViews,
                    std::span<const RG::RGFrame::TypedUpload<LevelParamsUBO>> params, uint32_t side)
{
    Rendering::IDevice* device = context.GetDevice();
    auto* commands = context.Cmd;
    const Rendering::TextureHandle clearanceTexture = context.GetTexture(map);
    if (!device || !commands || !clearanceTexture.IsValid())
        return;
    const Rendering::PipelineHandle pipeline = context.GetOrCreatePipelineVariant(pipelineId);
    if (!pipeline.IsValid())
        return;
    commands->SetMarker("TerrainShadow.FieldLit");
    commands->SetPipeline(pipeline);
    for (uint32_t level = 0; level < levelViews.size(); ++level)
    {
        Rendering::DescriptorSetDesc descriptorDesc{};
        descriptorDesc.layout = layout;
        descriptorDesc.transient = true;
        descriptorDesc.debugName = "TerrainShadow.FieldLit.Set0";
        const Rendering::DescriptorSetHandle descriptor = device->CreateDescriptorSet(descriptorDesc);
        Rendering::NamedDescriptorWriter writer(device, descriptor, metadata, 0);
        writer.AddUniformBuffer("TerrainShadowFieldLitParams", params[level].Buffer, params[level].Offset,
                            sizeof(LevelParamsUBO));
        writer.Flush();
        const Rendering::TextureViewHandle source = levelViews[level == 0u ? 0u : level - 1u];
        if (!Engine::Renderer::Pipeline::Nodes::Detail::BindStorageImageByName(device, descriptor, metadata, "uClearance",
                                                                               clearanceTexture) ||
            !BindStorageViewByName(*device, descriptor, metadata, "uSource", source) ||
            !BindStorageViewByName(*device, descriptor, metadata, "uTarget", levelViews[level]))
        {
            Logger::Log::Error("TerrainShadowFieldLit: terrain_shadow_field_lit.comp does not reflect "
                               "uClearance, uSource and uTarget; the lit field is not rebuilt");
            return;
        }
        commands->BindDescriptorSet(0, descriptor, pipeline);
        const uint32_t levelSide = std::max(side >> level, 1u);
        const uint32_t groups = (levelSide + kGroupSide - 1u) / kGroupSide;
        commands->Dispatch(groups, groups, 1);
        // The next level reads this one.
        commands->Barrier(Rendering::ResourceBarrier::CreateMemoryBarrier(
            static_cast<uint64_t>(Rendering::PipelineStageMask::ComputeShader),
            static_cast<uint64_t>(Rendering::PipelineStageMask::ComputeShader),
            static_cast<uint64_t>(Rendering::ResourceAccessMask::ShaderWrite),
            static_cast<uint64_t>(Rendering::ResourceAccessMask::ShaderRead)));
    }
}
} // namespace

TerrainShadowFieldLit::TerrainShadowFieldLit() = default;
TerrainShadowFieldLit::~TerrainShadowFieldLit() = default;

bool TerrainShadowFieldLit::Declare(Engine::Renderer::RenderServices& services, RG::RGFrame& frame, RG::RGTexture map,
                                    uint32_t mapSide, const CBTTerrain::TerrainShadowGrid& grid, bool mapBaked)
{
    m_Field = {};
    Rendering::IDevice* device = services.GetDevice();
    if (!device || mapSide < 2u || !EnsurePipeline(*device))
        return false;

    const uint32_t side = mapSide / 2u;
    Rendering::TextureDesc desc{};
    desc.width = side;
    desc.height = side;
    desc.depth = 1;
    desc.mipLevels = static_cast<uint32_t>(std::bit_width(side));
    desc.arrayLayers = 1;
    desc.sampleCount = 1;
    desc.format = static_cast<uint32_t>(Rendering::TextureFormat::R16G16_FLOAT);
    desc.usage = static_cast<uint32_t>(Rendering::TextureUsage::UnorderedAccess |
                                       Rendering::TextureUsage::ShaderResource);
    // GENERAL for its whole lifetime, as the clearance map: the rebuild stores to it and every
    // receiver samples it through the bindless array.
    desc.sampledInGeneralLayout = true;
    desc.initialState = Rendering::ResourceState::UnorderedAccess;
    desc.debugName = "TerrainShadow.FieldLit";
    bool fresh = false;
    const RG::RGTexture field = frame.ImportPersistentTexture("TerrainShadow.FieldLit", desc, &fresh);
    if (!field.IsValid())
        return false;

    const Rendering::TextureHandle physical = frame.PhysicalTexture(field);
    if (physical != m_Texture)
    {
        Release(services);
        m_Texture = physical;
        m_BindlessIndex = physical.IsValid() ? services.Textures().GetBindlessIndex(physical) : 0u;
        for (uint32_t level = 0; physical.IsValid() && level < desc.mipLevels; ++level)
        {
            Rendering::TextureViewDesc view{};
            view.baseMip = level;
            view.levelCount = 1;
            view.debugName = "TerrainShadow.FieldLit.Level";
            const auto levelView = device->CreateTextureView(physical, view);
            if (!levelView.IsValid())
            {
                Release(services);
                return false;
            }
            m_LevelViews.push_back(levelView);
        }
        fresh = true;
    }
    m_Side = side;
    m_Levels = desc.mipLevels;
    if (m_BindlessIndex == 0u || m_LevelViews.size() != m_Levels)
        return false;

    if (fresh || mapBaked)
    {
        if (!DeclareRebuildPass(frame, map, field, grid))
            return false;
        frame.MarkPersistentTextureInitialized(field);
    }
    m_Field = field;
    return true;
}

bool TerrainShadowFieldLit::DeclareRebuildPass(RG::RGFrame& frame, RG::RGTexture map, RG::RGTexture field,
                                               const CBTTerrain::TerrainShadowGrid& grid)
{
    std::vector<RG::RGFrame::TypedUpload<LevelParamsUBO>> params;
    params.reserve(m_Levels);
    for (uint32_t level = 0; level < m_Levels; ++level)
    {
        auto upload = frame.AllocUpload<LevelParamsUBO>();
        if (!upload.Valid())
            return false;
        *upload.Ptr = LevelParamsUBO{{level, grid.SamplesU, grid.SamplesV, 0u}, {grid.Texel, 0.0f, 0.0f, 0.0f}};
        params.push_back(upload);
    }
    frame.AddPass(
        "TerrainShadow.FieldLit", kRebuildPhase,
        [this, map, field](RG::RGPassBuilder& pass)
        {
            pass.UsePipeline(m_PipelineId);
            pass.Read(map, RG::RGTextureRead::Storage);
            pass.Write(field, RG::RGTextureWrite::Storage);
        },
        [this, map, params = std::move(params)](RG::RGContext& context)
        {
            RecordLitField(context, map, m_PipelineId, *m_Meta, m_Set0Layout, m_LevelViews, params, m_Side);
        });
    return true;
}

void TerrainShadowFieldLit::Release(Engine::Renderer::RenderServices& services)
{
    // The slot is dropped before the pool may age the texture out, so it never describes a freed
    // image; the views go with it.
    if (m_Texture.IsValid())
        services.Textures().InvalidateBindless(m_Texture);
    if (Rendering::IDevice* device = services.GetDevice())
        DestroyViews(*device);
    m_LevelViews.clear();
    m_Texture = {};
    m_BindlessIndex = 0;
    m_Field = {};
}

void TerrainShadowFieldLit::DestroyViews(Rendering::IDevice& device)
{
    for (Rendering::TextureViewHandle view : m_LevelViews)
        if (view.IsValid())
            device.DestroyTextureView(view);
}

void TerrainShadowFieldLit::OnDeviceRebuilt()
{
    // The rebuild already freed the pipeline, the texture and its views; these handles only look valid.
    m_LoadAttempted = false;
    m_PipelineId = {};
    m_Meta.reset();
    m_Set0Layout = {};
    m_Texture = {};
    m_LevelViews.clear();
    m_BindlessIndex = 0;
    m_Side = 0;
    m_Levels = 0;
    m_Field = {};
}

bool TerrainShadowFieldLit::EnsurePipeline(Rendering::IDevice& device)
{
    if (m_LoadAttempted)
        return m_PipelineId.IsValid();
    m_LoadAttempted = true;

    Rendering::ShaderPackage pkg{};
    std::string loadErr;
    if (!Rendering::LoadShaderPkg("Shaders/terrain_shadow_field_lit.shaderpkg", device.PreferredShaderSource(), pkg,
                                  &loadErr))
    {
        Logger::Log::Error("TerrainShadowFieldLit: failed to load terrain_shadow_field_lit.shaderpkg ({}); far "
                           "terrain reads its shadow unfiltered until the shader package is staged",
                           loadErr);
        return false;
    }
    auto cs = pkg.stageBytes.find("cs");
    if (cs == pkg.stageBytes.end() || cs->second.empty())
    {
        Logger::Log::Error("TerrainShadowFieldLit: terrain_shadow_field_lit.shaderpkg has no compute stage");
        return false;
    }
    m_Meta = std::make_unique<Rendering::ShaderMeta>(std::move(pkg.meta));

    Rendering::ComputePipelineDesc cd{};
    cd.ComputeShader = std::make_shared<const std::vector<uint8_t>>(std::move(cs->second));
    cd.DebugName = "TerrainShadow.FieldLit";
    auto captureSet0 = [this](uint32_t setIndex, Rendering::DescriptorSetLayoutDesc& layout)
    {
        if (setIndex == 0)
            m_Set0Layout = layout;
    };
    std::string err;
    if (!Rendering::MaterialHelper::ApplyShaderMetaToComputeDesc(device, *m_Meta, cd,
                                                               Rendering::MaterialBuilder::MergeMode::Auto,
                                                               {true, 128}, captureSet0, &err))
    {
        Logger::Log::Error("TerrainShadowFieldLit: cannot build the compute layout ({})", err);
        return false;
    }
    m_PipelineId = device.InternComputePipeline(std::move(cd));
    return m_PipelineId.IsValid();
}

} // namespace GameEngine::CBTTerrainECS
