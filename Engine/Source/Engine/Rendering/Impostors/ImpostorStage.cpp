#include "Engine/Rendering/Impostors/ImpostorStage.h"

#include "Engine/Rendering/Impostors/ImpostorBaker.h"
#include "Engine/Rendering/DepthDrawRecorder.h"
#include "Engine/Rendering/Material.h"
#include "Engine/Rendering/MaterialRegistry.h"
#include "Engine/Rendering/MaterialSystem.h"
#include "Engine/Rendering/MeshLODThresholds.h"
#include "Engine/Rendering/ParallaxReliefDepth.h"
#include "Engine/Rendering/PipelineVariantCache.h"
#include "Engine/Rendering/PrimitiveGenerator.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/TextureService.h"
#include "Engine/Rendering/ViewRegistry.h"
#include "Engine/Rendering/WorldDrawTypes.h"
#include "Logger/Logger.h"
#include "Platform/Environment.h"
#include "Rendering/Core/GPUInstanceDepthClass.h"
#include "Rendering/Core/GPUScene.h"
#include "Rendering/Materials/MaterialDocument.h"
#include "Types/Fnv1a.h"

#include <string>
#include <vector>

namespace GameEngine::Engine::Renderer
{

bool ImpostorStage::IsEnabled(const RenderServices& services)
{
    static const bool enabled = Platform::EnvironmentSwitchEnabled("GE_IMPOSTORS", true);
    if (!enabled)
        return false;
    if (services.GetProfile().UseGpuDrivenDraws)
        return true;
    const auto* device = services.GetDevice();
    const uint32_t usage = static_cast<uint32_t>(Rendering::TextureUsage::RenderTarget) |
                           static_cast<uint32_t>(Rendering::TextureUsage::ShaderResource);
    return services.GetProfile().IsCompat() && device &&
           device->IsTextureFormatSupported(Rendering::TextureFormat::RGBA8_UNORM, usage) &&
           device->IsTextureFormatSupported(Rendering::TextureFormat::R16G16B16A16_FLOAT, usage) &&
           device->IsTextureFormatSupported(Rendering::TextureFormat::R8G8_UNORM, usage);
}

float ImpostorStage::DefaultSwitchCoverage(const RenderServices& services, Rendering::ViewId viewId)
{
    const uint32 height = services.Views().GetViewWorldViewportHeight(viewId);
    const uint32 renderHeight = height > 0u ? height : Rendering::kLodSseFallbackViewportH;
    return static_cast<float>(services.GetProfile().ImpostorSwitchPixels) / static_cast<float>(renderHeight);
}

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

void ImpostorStage::ApplyToSlice(Rendering::GPUDrawStreamBuilder::ViewLODParams& lod, const RenderServices& services,
                                 Rendering::ViewId viewId) const
{
    if (!IsEnabled(services) || services.GetMeshGPURegistry().GetLodSelectionMode() == Rendering::LodSelectionMode::Off)
        return;
    lod.impostorCardMesh = m_CardMeshIndex.load(std::memory_order_relaxed);
    lod.renderedLevelHistoryRequested |= lod.impostorCardMesh != kNoCardMesh;
    lod.impostorSwitchCoverage = DefaultSwitchCoverage(services, viewId);
    lod.impostorSwitchHysteresis = kSwitchHysteresis;
}

bool ImpostorStage::AreShadowCardsReady(RenderServices& services, Rendering::ViewId viewId,
                                        bool directional) const
{
    const auto& registry = services.GetMeshGPURegistry();
    if (!m_CardMesh.IsValid() || !IsEnabled(services) ||
        registry.GetLodSelectionMode() == Rendering::LodSelectionMode::Off)
        return false;
    const auto* mesh = registry.Find(m_CardMesh);
    Rendering::MeshGPUEntryBindings bindings{};
    if (!mesh || !registry.TryGetDrawableBindings(*mesh, bindings))
        return false;

    const auto worldKeywords = services.Views().GetViewDeclaredWorldPassKeywords(viewId);
    const auto shadowKeywords = ChooseDepthHeadPipeline(
        DepthPassKeywords(worldKeywords), directional ? DepthPassType::ShadowCascade : DepthPassType::SpotShadow,
        /*alphaTest=*/true, /*vertexModified=*/true,
        /*sharedDepthOffered=*/false, /*writesReliefDepth=*/false).Keywords;

    bool hasCards = false;
    bool ready = true;
    auto& variants = services.Materials().Variants();
    const auto frontFace = services.GetFrontFace();
    for (const auto& key : services.GetEntityBatchKeys(viewId))
    {
        if (key.mesh != m_CardMesh || !key.material ||
            key.material->GetCompileSpec().surfaceShaderPath != "Surfaces/impostor.glsl")
            continue;
        hasCards = true;
        const auto flags = Rendering::BoundStreamVertexFlags(*mesh, bindings, key.material->IgnoresVertexColor());
        for (const auto winding : {frontFace, Rendering::FlipWinding(frontFace)})
            ready &= variants.WarmShadowDepthVariant(*key.material, flags, mesh->topology,
                                                      shadowKeywords, winding, /*depthClampEnable=*/directional);
    }
    return hasCards && ready;
}

const Material* ImpostorStage::RegisterCardMaterial(RenderServices& services, const GUID& material,
                                                    std::string_view name, const ImpostorAtlas& atlas,
                                                    const Mathematics::Vector3& center, float radius,
                                                    uint32 framesPerSide, Mathematics::OctahedralLayout layout,
                                                    const ImpostorFoliageSway& sway, CardMaterialStyle style)
{
    const bool coverageOnly = style == CardMaterialStyle::ShadowCoverage;
    if (!atlas.Color.IsValid() || (!coverageOnly && (!atlas.NormalDepth.IsValid() || !atlas.SeasonMask.IsValid())))
        return nullptr;
    Material* registered = services.Materials().Registry().Find(material);
    const StringId albedo = HashStringId("albedoMap");
    const StringId normal = HashStringId("normalMap");
    const StringId seasonMask = HashStringId("impostorSeasonMaskMap");
    const StringId diffuseTransmission = HashStringId("impostorDiffuseTransmissionMap");
    if (!registered || registered->GetTexture(albedo) != atlas.Color ||
        (!coverageOnly && registered->GetTexture(diffuseTransmission) != atlas.DiffuseTransmission))
    {
        MaterialDocument doc{};
        doc.schemaVersion = 3;
        doc.materialName = std::string(name);
        doc.lightingModel = "StandardPBR";
        doc.surfaceShader = coverageOnly ? "Surfaces/impostor_shadow.glsl" : "Surfaces/impostor.glsl";
        doc.customVertexShader = style == CardMaterialStyle::PackedColor;
        doc.vertexModifier = doc.customVertexShader ? "VertexModifiers/forest_impostor.glsl" :
                                                    "VertexModifiers/impostor_card.glsl";
        doc.alphaMode = MaterialAlphaMode::Mask;
        doc.doubleSided = true;
        doc.properties["alphaCutoff"] = 0.5f;
        doc.properties["impostorBounds"] = std::vector<float>{center.x, center.y, center.z, radius};
        doc.properties["foliageWindHeight"] = std::vector<float>{sway.BaseY, sway.Height};
        doc.properties["foliageWindSway"] = sway.MaxDisplacement;
        doc.properties["impostorAtlas"] = std::vector<float>{
            static_cast<float>(framesPerSide), layout == Mathematics::OctahedralLayout::Hemisphere ? 0.0f : 1.0f,
            Platform::EnvironmentSwitchEnabled("GE_IMPOSTOR_DEBUG_TINT", false) ? 1.0f : 0.0f,
            static_cast<float>(atlas.HighestReadyMip)};
        doc.textures["albedoMap"] = std::string{};
        auto keywords = Rendering::MaterialKeyword::None;
        if (!coverageOnly)
        {
            doc.textures["normalMap"] = std::string{};
            doc.textures["impostorSeasonMaskMap"] = std::string{};
            if (atlas.DiffuseTransmission.IsValid())
            {
                doc.textures["impostorDiffuseTransmissionMap"] = std::string{};
                keywords |= Rendering::MaterialKeyword::DiffuseTransmission;
            }
        }
        // Source meshes or the forest stand remain drawable until the card pipeline publishes.
        registered = services.Materials().RegisterMaterialFromDocument(
            material, doc, keywords, MaterialSystem::BaseCompileMode::Async);
        if (!registered)
            return nullptr;
        // The atlas is a runtime texture: bound by handle, as a video frame is.
        registered->SetTexture(albedo, atlas.Color);
        registered->SetBindlessTextureIndex(albedo, services.Textures().GetBindlessIndex(atlas.Color));
        if (!coverageOnly)
        {
            registered->SetTexture(normal, atlas.NormalDepth);
            registered->SetBindlessTextureIndex(normal, services.Textures().GetBindlessIndex(atlas.NormalDepth));
            registered->SetTexture(seasonMask, atlas.SeasonMask);
            registered->SetBindlessTextureIndex(seasonMask, services.Textures().GetBindlessIndex(atlas.SeasonMask));
            if (atlas.DiffuseTransmission.IsValid())
            {
                registered->SetTexture(diffuseTransmission, atlas.DiffuseTransmission);
                registered->SetBindlessTextureIndex(diffuseTransmission,
                                                    services.Textures().GetBindlessIndex(atlas.DiffuseTransmission));
            }
        }
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
        if (!entry || entry->gpuMeshIndex == 0xFFFFFFFFu || !registry.IsEntryDrawable(*entry))
            return {};
        indices.push_back(entry->gpuMeshIndex);
    }
    return indices;
}

bool ImpostorStage::Link(RenderServices& services, std::span<const uint32> partMeshes, const Material& card,
                         float switchCoverage, bool requiresPlacement) const
{
    Rendering::GPUScene* scene = services.GetGPUScene();
    const uint32 cardMesh = m_CardMeshIndex.load(std::memory_order_relaxed);
    const uint32 cardMaterial = card.GetGpuSceneMaterialIndex();
    if (!scene || partMeshes.empty() || cardMesh == kNoCardMesh || cardMaterial == 0xFFFFFFFFu)
        return false;
    // The scatter routes a card through the material-dependent shadow rows, as an alpha-masked
    // material with a vertex modifier always is.
    if (services.Materials().GetMaterialDepthClass(cardMaterial) != Rendering::MaterialDepthClass::MaterialDependent)
    {
        Logger::Log::Error("ImpostorStage: card material {} is not material-dependent in depth; it is not linked",
                           cardMaterial);
        return false;
    }
    return scene->SetMeshImpostorGroup(partMeshes, cardMaterial, cardMesh, switchCoverage, requiresPlacement);
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
    const auto& registry = services.GetMeshGPURegistry();
    const auto* entry = registry.Find(m_CardMesh);
    Rendering::MeshGPUEntryBindings bindings{};
    if (!entry || !registry.TryGetDrawableBindings(*entry, bindings))
        return;

    auto& variants = services.Materials().Variants();
    const auto& views = services.Views();
    const auto frontFace = services.GetFrontFace();
    const bool requiresMirrored = !services.GetProfile().IsCompat();

    // A card is alpha-masked and casts a shadow; the scatter, not this record, decides which
    // instances draw it, so the record names no instance of its own and every layer.
    const uint32 shadowFlag = services.GetProfile().UseGpuDrivenDraws ? kSubmissionFlagCastShadows : 0u;
    const uint32 flags = shadowFlag | 2u
                         | Rendering::DepthClassInstanceFlagBits(Rendering::MaterialDepthClass::MaterialDependent);
    std::vector<WorldSubmissionRecord> records;
    uint64 revision = Hashing::kFnv1a64OffsetBasis;
    for (const auto& view : views.GetViews())
    {
        if ((view.worldId != 0u && view.worldId != worldId) || !view.RenderedByPipeline() ||
            services.ResolveViewForceLOD(view.id) != 0xFFFFFFFFu ||
            view.ActiveRenderLayerMask() == Rendering::RenderLayerMask::None())
            continue;
        const auto* camera = views.FindCameraData(view.cameraId);
        if (!camera || camera->cameraPos.w == 1.0f || camera->proj[1][1] == 0.0f)
            continue;
        const auto pass = views.GetViewDeclaredWorldPassKeywords(view.id);
        const auto format = views.GetViewWorldColorFormatKey(view.id);
        if (!pass || format.ColorCount == 0u)
            continue;

        const auto relief = views.GetViewWorldReliefDepthSource(view.id);
        const bool needsPrepass = relief != ReliefDepthSource::NoPrepass;
        const auto depth = views.GetViewPrepassFormatKey(view.id);
        const auto prepassKeywords = ChooseDepthHeadPipeline(
            DepthPassKeywords(pass), DepthPassType::Prepass, /*alphaTest=*/true,
            /*vertexModified=*/true, /*sharedDepthOffered=*/false, /*writesReliefDepth=*/false).Keywords;

        for (const Material* card : cards)
        {
            // The registered card is masked and vertex-modified, so its camera depth head
            // consumes the same vertex streams as its color pass.
            const auto flagsForMesh = Rendering::BoundStreamVertexFlags(*entry, bindings, card->IgnoresVertexColor());
            const auto keywords = WorldEntityColorKeywords(*pass, card->GetVariantKey().materialKeywords,
                                                            card->GetAlphaMode(), relief);
            const auto warmWinding = [&](Rendering::FrontFace winding)
            {
                const bool colorReady =
                    variants.WarmColorVariant(*card, flagsForMesh, entry->topology, keywords, winding, format);
                if (!needsPrepass)
                    return colorReady;
                const auto* variant =
                    variants.FindOrRequestPrepassVariant(*card, flagsForMesh, entry->topology, prepassKeywords, winding);
                const bool depthReady = variant && variants.EnsureConcreteWarm(*card, variant->PipelineId, depth);
                return colorReady && depthReady;
            };

            bool ready = warmWinding(frontFace);
            // Both full-profile scatter segments have their own winding-specific pipelines.
            if (requiresMirrored)
                ready &= warmWinding(Rendering::FlipWinding(frontFace));
            if (!ready)
                continue; // A view without the card batch keeps its mesh draws.
            WorldSubmissionRecord record{};
            record.viewId = view.id;
            record.meshHandle = m_CardMesh;
            record.material = card;
            record.instanceIndex = 0xFFFFFFFFu;
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
