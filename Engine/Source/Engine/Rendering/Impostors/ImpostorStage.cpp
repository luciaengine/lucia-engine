#include "Engine/Rendering/Impostors/ImpostorStage.h"

#include "Engine/Rendering/Impostors/ImpostorBaker.h"
#include "Engine/Rendering/Material.h"
#include "Engine/Rendering/MaterialRegistry.h"
#include "Engine/Rendering/MaterialSystem.h"
#include "Engine/Rendering/PrimitiveGenerator.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/TextureService.h"
#include "Logger/Logger.h"
#include "Platform/Environment.h"
#include "Rendering/Core/GPUInstanceDepthClass.h"
#include "Rendering/Core/GPUScene.h"
#include "Rendering/Materials/MaterialDocument.h"

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
    m_CardMeshIndex.store(entry ? entry->gpuMeshIndex : kNoCardMesh, std::memory_order_relaxed);
}

void ImpostorStage::ApplyToSlice(Rendering::GPUDrawStreamBuilder::ViewLODParams& lod) const
{
    static const bool enabled = Platform::EnvironmentSwitchEnabled("GE_IMPOSTORS", true);
    if (!enabled)
        return;
    lod.impostorCardMesh = m_CardMeshIndex.load(std::memory_order_relaxed);
    lod.impostorSwitchCoverage = kSwitchCoverage;
    lod.impostorSwitchHysteresis = kSwitchHysteresis;
}

const Material* ImpostorStage::RegisterCardMaterial(RenderServices& services, const GUID& material,
                                                    std::string_view name, const ImpostorAtlas& atlas,
                                                    const Mathematics::Vector3& center, float radius,
                                                    uint32 framesPerSide, Mathematics::OctahedralLayout layout)
{
    if (!atlas.Color.IsValid() || !atlas.NormalDepth.IsValid())
        return nullptr;
    Material* registered = services.Materials().Registry().Find(material);
    const StringId albedo = HashStringId("albedoMap");
    const StringId normal = HashStringId("normalMap");
    if (!registered || registered->GetTexture(albedo) != atlas.Color)
    {
        MaterialDocument doc{};
        doc.schemaVersion = 3;
        doc.materialName = std::string(name);
        doc.lightingModel = "StandardPBR";
        doc.surfaceShader = "Surfaces/impostor.glsl";
        doc.vertexModifier = "VertexModifiers/impostor_card.glsl";
        doc.alphaMode = MaterialAlphaMode::Mask;
        doc.doubleSided = true;
        doc.properties["alphaCutoff"] = 0.5f;
        doc.properties["impostorBounds"] = std::vector<float>{center.x, center.y, center.z, radius};
        doc.properties["impostorAtlas"] = std::vector<float>{
            static_cast<float>(framesPerSide), layout == Mathematics::OctahedralLayout::Hemisphere ? 0.0f : 1.0f,
            Platform::EnvironmentSwitchEnabled("GE_IMPOSTOR_DEBUG_TINT", false) ? 1.0f : 0.0f, 0.0f};
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
    return registered->GetGraphicsPipelineId().IsValid() ? registered : nullptr;
}

bool ImpostorStage::Link(RenderServices& services, std::span<const Rendering::MeshGPUHandle> parts,
                         const Material& card) const
{
    Rendering::GPUScene* scene = services.GetGPUScene();
    const uint32 cardMesh = m_CardMeshIndex.load(std::memory_order_relaxed);
    const uint32 cardMaterial = card.GetGpuSceneMaterialIndex();
    if (!scene || parts.empty() || cardMesh == kNoCardMesh || cardMaterial == 0xFFFFFFFFu)
        return false;
    // The scatter routes a card through the material-dependent shadow rows, as an alpha-masked
    // material with a vertex modifier always is.
    if (services.Materials().GetMaterialDepthClass(cardMaterial) != Rendering::MaterialDepthClass::MaterialDependent)
    {
        Logger::Log::Error("ImpostorStage: card material {} is not material-dependent in depth; it is not linked",
                           cardMaterial);
        return false;
    }
    const Rendering::MeshGPURegistry& registry = services.GetMeshGPURegistry();
    for (size_t i = 0; i < parts.size(); ++i)
    {
        const Rendering::MeshGPUEntry* entry = registry.Find(parts[i]);
        if (!entry || entry->gpuMeshIndex == 0xFFFFFFFFu)
            continue;
        scene->SetMeshImpostorCard(entry->gpuMeshIndex, cardMaterial, cardMesh, i == 0);
    }
    return true;
}

void ImpostorStage::Unlink(RenderServices& services, std::span<const Rendering::MeshGPUHandle> parts)
{
    Rendering::GPUScene* scene = services.GetGPUScene();
    if (!scene)
        return;
    const Rendering::MeshGPURegistry& registry = services.GetMeshGPURegistry();
    for (const Rendering::MeshGPUHandle part : parts)
    {
        if (const Rendering::MeshGPUEntry* entry = registry.Find(part); entry && entry->gpuMeshIndex != 0xFFFFFFFFu)
            scene->ClearMeshImpostorCard(entry->gpuMeshIndex);
    }
}

void ImpostorStage::OnDeviceRebuilt(Rendering::IDevice* /*device*/)
{
    // The rebuild freed the quad; the next EnsureCardMesh registers it again.
    m_CardMeshIndex.store(kNoCardMesh, std::memory_order_relaxed);
}

} // namespace GameEngine::Engine::Renderer
