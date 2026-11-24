#include "Engine/Rendering/Impostors/ImpostorStage.h"

#include "Engine/Rendering/Impostors/ImpostorBaker.h"
#include "Engine/Rendering/Material.h"
#include "Engine/Rendering/MaterialRegistry.h"
#include "Engine/Rendering/MaterialSystem.h"
#include "Engine/Rendering/PrimitiveGenerator.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/TextureService.h"
#include "Engine/Rendering/ViewRegistry.h"
#include "Engine/Rendering/WorldDrawTypes.h"
#include "Logger/Logger.h"
#include "Mathematics/Vector4.h"
#include "Platform/Environment.h"
#include "Rendering/Core/GPUInstanceDepthClass.h"
#include "Rendering/Core/GPUScene.h"
#include "Rendering/Materials/MaterialDocument.h"
#include "Types/Fnv1a.h"

#include <iterator>
#include <string>
#include <vector>

namespace GameEngine::Engine::Renderer
{

void ImpostorStage::EnsureCardMesh(RenderServices& services)
{
    Rendering::MeshGPURegistry& registry = services.GetMeshGPURegistry();
    const Rendering::MeshGPUHandle card =
        registry.RegisterSubmesh(Rendering::MeshGPUKey{PrimitiveGenerator::PlaneSpriteUvGuid(), 0u},
                                 PrimitiveGenerator::GeneratePlaneSpriteUv());
    const Rendering::MeshGPUEntry* entry = registry.Find(card);
    m_CardMesh = card;
    m_CardMeshIndex.store(entry ? entry->gpuMeshIndex : kNoCardMesh, std::memory_order_relaxed);
}

bool ImpostorStage::IsEnabled()
{
    static const bool enabled = Platform::EnvironmentSwitchEnabled("GE_IMPOSTORS", true);
    return enabled;
}

float ImpostorStage::DefaultSwitchCoverage(const RenderServices& services, Rendering::ViewId viewId)
{
    const uint32 height = services.Views().GetViewWorldViewportHeight(viewId);
    const uint32 renderHeight = height > 0u ? height : Rendering::kLodSseFallbackViewportH;
    return static_cast<float>(ImpostorAtlasKey{}.FrameSize) / static_cast<float>(renderHeight);
}

void ImpostorStage::ApplyToSlice(Rendering::GPUDrawStreamBuilder::ViewLODParams& lod, const RenderServices& services,
                                 Rendering::ViewId viewId) const
{
    if (!IsEnabled() || services.GetMeshGPURegistry().GetLodSelectionMode() == Rendering::LodSelectionMode::Off)
        return;
    lod.impostorCardMesh = m_CardMeshIndex.load(std::memory_order_relaxed);
    lod.impostorSwitchCoverage = DefaultSwitchCoverage(services, viewId);
    lod.impostorSwitchHysteresis = kSwitchHysteresis;
}

const Material* ImpostorStage::RegisterCardMaterial(RenderServices& services, const GUID& material,
                                                    std::string_view name, const ImpostorAtlas& atlas,
                                                    const Mathematics::Vector3& center, float radius,
                                                    uint32 framesPerSide, Mathematics::OctahedralLayout layout,
                                                    ImpostorCardSource source)
{
    if (!atlas.Color.IsValid() || !atlas.NormalDepth.IsValid())
        return nullptr;
    Material* registered = services.Materials().Registry().Find(material);
    const StringId albedo = HashStringId("albedoMap");
    const StringId normal = HashStringId("normalMap");
    // impostorAtlas: frames per side, layout, debug tint, highest ready mip.
    const Mathematics::Vector4 atlasParams(
        static_cast<float>(framesPerSide), layout == Mathematics::OctahedralLayout::Hemisphere ? 0.0f : 1.0f,
        Platform::EnvironmentSwitchEnabled("GE_IMPOSTOR_DEBUG_TINT", false) ? 1.0f : 0.0f,
        static_cast<float>(atlas.HighestReadyMip));
    const StringId atlasProperty = HashStringId("impostorAtlas");
    if (!registered || registered->GetTexture(albedo) != atlas.Color)
    {
        MaterialDocument doc{};
        doc.schemaVersion = 3;
        doc.materialName = std::string(name);
        doc.lightingModel = "StandardPBR";
        doc.surfaceShader = "Surfaces/impostor.glsl";
        const bool packed = source == ImpostorCardSource::PackedForest;
        doc.vertexModifier = packed ? "VertexModifiers/forest_impostor.glsl" : "VertexModifiers/impostor_card.glsl";
        doc.customVertexShader = packed;
        doc.alphaMode = MaterialAlphaMode::Mask;
        doc.doubleSided = true;
        doc.properties["alphaCutoff"] = 0.5f;
        doc.properties["impostorBounds"] = std::vector<float>{center.x, center.y, center.z, radius};
        doc.properties["impostorAtlas"] = std::vector<float>(std::begin(atlasParams.v), std::end(atlasParams.v));
        doc.textures["albedoMap"] = std::string{};
        doc.textures["normalMap"] = std::string{};
        registered = services.Materials().RegisterMaterialFromDocument(material, doc);
        if (!registered)
            return nullptr;
        // The atlas is a runtime texture: bound by handle, as a video frame is.
        registered->SetTexture(albedo, atlas.Color);
        registered->SetBindlessTextureIndex(albedo, services.Textures().GetBindlessIndex(atlas.Color));
        registered->SetTexture(normal, atlas.NormalDepth);
        registered->SetBindlessTextureIndex(normal, services.Textures().GetBindlessIndex(atlas.NormalDepth));
    }
    else
    {
        // The same atlas gains its mips over several frames: only the ready mip changes.
        Mathematics::Vector4 current{};
        if (!registered->GetVector(atlasProperty, current.v, 4) || current.w != atlasParams.w)
            registered->SetVector(atlasProperty, atlasParams.v, 4);
    }
    return registered->GetGraphicsPipelineId().IsValid() ? registered : nullptr;
}

std::vector<uint32> ImpostorStage::PartMeshIndices(const RenderServices& services,
                                                   std::span<const Rendering::MeshGPUHandle> parts)
{
    std::vector<uint32> indices;
    indices.reserve(parts.size());
    const Rendering::MeshGPURegistry& registry = services.GetMeshGPURegistry();
    for (const Rendering::MeshGPUHandle part : parts)
    {
        const Rendering::MeshGPUEntry* entry = registry.Find(part);
        if (!entry || entry->gpuMeshIndex == 0xFFFFFFFFu)
            return {};
        indices.push_back(entry->gpuMeshIndex);
    }
    return indices;
}

bool ImpostorStage::Link(RenderServices& services, std::span<const uint32> partMeshes, const Material& card,
                         float switchCoverage) const
{
    Rendering::GPUScene* scene = services.GetGPUScene();
    const uint32 cardMesh = m_CardMeshIndex.load(std::memory_order_relaxed);
    const uint32 cardMaterial = card.GetGpuSceneMaterialIndex();
    // The CPU draw path of the compatibility profile has no scatter to select the card.
    if (!scene || partMeshes.empty() || cardMesh == kNoCardMesh || cardMaterial == 0xFFFFFFFFu
        || !services.GetProfile().UseGpuDrivenDraws)
        return false;
    // The scatter routes a card through the material-dependent shadow rows, as an alpha-masked
    // material with a vertex modifier always is.
    if (services.Materials().GetMaterialDepthClass(cardMaterial) != Rendering::MaterialDepthClass::MaterialDependent)
    {
        Logger::Log::Error("ImpostorStage: card material {} is not material-dependent in depth; it is not linked",
                           cardMaterial);
        return false;
    }
    // The scatter measures the switch on the sphere the atlas was baked on (impostorBounds.w).
    float bounds[4]{};
    if (!card.GetVector(HashStringId("impostorBounds"), bounds, 4) || !(bounds[3] > 0.0f))
    {
        Logger::Log::Error("ImpostorStage: card material {} has no impostorBounds radius; it is not linked",
                           cardMaterial);
        return false;
    }
    return scene->SetMeshImpostorGroup(partMeshes, cardMaterial, cardMesh, switchCoverage, bounds[3]);
}

void ImpostorStage::Unlink(RenderServices& services, std::span<const uint32> partMeshes)
{
    Rendering::GPUScene* scene = services.GetGPUScene();
    if (!scene)
        return;
    if (!partMeshes.empty())
        scene->ClearMeshImpostorGroup(partMeshes.front());
}

void ImpostorStage::SubmitCards(RenderServices& services, uint64 worldId, std::span<const Material* const> cards) const
{
    if (cards.empty() || !m_CardMesh.IsValid())
        return;
    // A card is alpha-masked and casts a shadow; the scatter, not this record, decides which
    // instances draw it, so the record names no instance of its own and every layer.
    const uint32 flags = kSubmissionFlagCastShadows | 2u
                         | Rendering::DepthClassInstanceFlagBits(Rendering::MaterialDepthClass::MaterialDependent);
    std::vector<WorldSubmissionRecord> records;
    uint64 revision = Hashing::kFnv1a64OffsetBasis;
    for (const auto& view : services.Views().GetViews())
    {
        if (view.worldId != 0 && view.worldId != worldId)
            continue;
        for (const Material* card : cards)
        {
            WorldSubmissionRecord record{};
            record.viewId = view.id;
            record.meshHandle = m_CardMesh;
            record.material = card;
            record.renderLayerMask = Rendering::RenderLayerMask::All();
            record.flags = flags;
            records.push_back(record);
            revision = Hashing::HashCombine(revision, Hashing::HashPointer(card));
            revision = Hashing::HashCombine(revision, static_cast<uint64>(view.id));
        }
    }
    // A nonzero revision names an unchanged record set, so equal frames keep their batch keys.
    if (!records.empty())
        services.SubmitWorldSubmissions(records, revision | 1u);
}

void ImpostorStage::OnDeviceRebuilt(Rendering::IDevice* /*device*/)
{
    // The rebuild freed the quad; the next EnsureCardMesh registers it again.
    m_CardMeshIndex.store(kNoCardMesh, std::memory_order_relaxed);
}

} // namespace GameEngine::Engine::Renderer
