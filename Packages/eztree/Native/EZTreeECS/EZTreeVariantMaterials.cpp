#include "EZTreeECS/EZTreeVariantMaterials.h"

#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Assets/MaterialAsset.h"
#include "Core/Engine.h"
#include "EZTree/EZTreeOptions.h"
#include "EZTreeECS/EZTreeDefaultTextures.h"
#include "EZTreeECS/EZTreeRuntimeMaterials.h"
#include "Engine/Rendering/Impostors/ImpostorBaker.h"
#include "Engine/Rendering/Material.h"
#include "Engine/Rendering/MaterialRegistry.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/TextureService.h"
#include "Mathematics/Geometry.h"
#include "Rendering/Materials/MaterialDocument.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <string>
#include <vector>

namespace GameEngine::EZTreeECS
{
namespace
{

// The leaves sway more than the bark at the same wind.
constexpr float kBarkWindScale = 1.0f;
constexpr float kLeafWindScale = 1.35f;

GUID VariantMaterialGuid(uint32 slot, const char* part)
{
    return GUID::Derive(GUID::Null(), "engine/ez-tree/variant/" + std::to_string(slot) + "/" + part);
}

GUID GuidFromArray(const std::array<uint8, 16>& bytes)
{
    GUID::Data data{};
    std::copy(bytes.begin(), bytes.end(), data.begin());
    return GUID(data);
}

std::vector<float> ColorFromHex(uint32 rgb, float alpha = 1.0f)
{
    return {
        static_cast<float>((rgb >> 16) & 0xFFu) / 255.0f,
        static_cast<float>((rgb >> 8) & 0xFFu) / 255.0f,
        static_cast<float>(rgb & 0xFFu) / 255.0f,
        alpha};
}

uint32 DefaultBarkTint(EZTree::BarkType type)
{
    switch (type)
    {
    case EZTree::BarkType::Birch:  return 0xB8B2A3u;
    case EZTree::BarkType::Pine:   return 0x5A3A24u;
    case EZTree::BarkType::Willow: return 0x6B5A3Au;
    case EZTree::BarkType::Oak:
    default:                       return 0x6E4A2Fu;
    }
}

uint32 DefaultLeafTint(EZTree::LeafType type)
{
    switch (type)
    {
    case EZTree::LeafType::Ash:   return 0x6F8F3Au;
    case EZTree::LeafType::Aspen: return 0x89A84Bu;
    case EZTree::LeafType::Pine:  return 0x254D2Eu;
    case EZTree::LeafType::Oak:
    default:                      return 0x4C7A2Eu;
    }
}

// Default textures ship inside the eztree package mount. Their GUIDs are the
// pre-extraction editor-mount originals, carried by the package's
// stored-identity .assetmanifest.
GUID ResolvePackageTextureGuid(std::filesystem::path relativePath)
{
    auto& assets = EngineCore::GetInstance().GetAssetManager();
    GUID guid = assets.ResolveAssetGuid(relativePath, EZTree::kPackageAlias);
    if (!guid.IsNull())
        return guid;
    guid = assets.ResolveAssetGuid(relativePath, kAssetSourceAliasEditor);
    if (guid.IsNull())
        assets.ReportAssetNotInBuild(relativePath.generic_string());
    return guid;
}

GUID DefaultLeafTextureGuid(EZTree::LeafType type, const char* suffix = "color")
{
    return ResolvePackageTextureGuid(DefaultLeafTextureRelativePath(type, suffix));
}

GUID DefaultBarkTextureGuid(EZTree::BarkType type, const char* suffix)
{
    return ResolvePackageTextureGuid(DefaultBarkTextureRelativePath(type, suffix));
}

bool TryLoadMaterialDocument(const GUID& materialGuid, MaterialDocument& outDoc)
{
    if (materialGuid.IsNull())
        return false;

    auto& assets = EngineCore::GetInstance().GetAssetManager();
    SharedPtr<Asset> asset = assets.GetAsset(materialGuid);
    if (!asset)
        asset = assets.LoadAssetAsync(materialGuid).get();

    auto* materialAsset = dynamic_cast<MaterialAsset*>(asset.get());
    if (!materialAsset)
        return false;

    outDoc = materialAsset->GetDocument();
    return true;
}

// The variant's authored wind. ez_tree_wind.glsl resolves the wind volumes at each
// instance on the GPU. windStrength: xyz = authored wind in tree space times
// strengthScale, w = strengthScale, 0 when the tree has no wind. windParams: authored
// gust frequency, spatial scale, tree height and base Y.
void ApplyWindProperties(MaterialDocument& doc,
                         const EZTree::TreeOptions& options,
                         const Mathematics::BoundingBox& bounds,
                         float strengthScale)
{
    doc.vertexModifier = "VertexModifiers/ez_tree_wind.glsl";
    const float scale = options.wind.enabled ? strengthScale : 0.0f;
    doc.properties["windStrength"] = std::vector<float>{
        options.wind.strength.x * scale,
        options.wind.strength.y * scale,
        options.wind.strength.z * scale,
        scale};
    doc.properties["windParams"] = std::vector<float>{
        std::max(options.wind.frequency, 0.0f),
        std::max(0.001f, options.wind.scale),
        std::max(0.25f, bounds.halfExtents.y * 2.0f),
        bounds.center.y - bounds.halfExtents.y};
}

void PreloadMaterialDocumentTextures(Engine::Renderer::RenderServices& renderServices,
                                     const MaterialDocument& doc)
{
    for (const auto& [_, texGuidStr] : doc.textures)
    {
        if (texGuidStr.empty())
            continue;
        const GUID texGuid(texGuidStr);
        if (!texGuid.IsNull())
            (void)renderServices.Textures().GetOrUpload(texGuid);
    }
}

MaterialDocument BuildBarkMaterialDocument(const EZTree::TreeOptions& options,
                                           const Mathematics::BoundingBox& bounds)
{
    MaterialDocument doc{};
    const GUID authoredMaterial = GuidFromArray(options.barkMaterialGuid);
    const bool hasAuthoredMaterial = TryLoadMaterialDocument(authoredMaterial, doc);

    if (!hasAuthoredMaterial)
        doc = MakeBarkRuntimeMaterialShape();
    else
        doc.materialName = doc.materialName.empty() ? "Tree Generator Bark" : (doc.materialName + " (Tree Generator)");

    // The generated tree combines branch tubes and leaf billboards into one
    // renderable today. Keep it double-sided so leaf quads and any imported
    // EZ-Tree winding differences cannot disappear under backface culling.
    doc.doubleSided = true;

    const bool hasExplicitBarkTint = options.bark.tint != 0xFFFFFFu;
    if (!hasAuthoredMaterial || hasExplicitBarkTint)
        doc.properties["baseColor"] = ColorFromHex(hasExplicitBarkTint ? options.bark.tint : DefaultBarkTint(options.bark.type));
    ApplyWindProperties(doc, options, bounds, kBarkWindScale);

    // Per-slot override from the component, in kDefaultTextureSlots order.
    const std::array<GUID, 4> barkOverrides{
        GuidFromArray(options.barkColorTextureGuid),
        GuidFromArray(options.barkNormalTextureGuid),
        GuidFromArray(options.barkRoughnessTextureGuid),
        GuidFromArray(options.barkAoTextureGuid)};

    const std::array<float, 8> barkUv{
        std::max(0.0001f, options.bark.textureScale.x),
        0.0f,
        0.0f,
        0.0f,
        0.0f,
        std::max(0.0001f, options.bark.textureScale.y),
        0.0f,
        0.0f};

    for (size_t i = 0; i < std::size(kDefaultTextureSlots); ++i)
    {
        const DefaultTextureSlot& slot = kDefaultTextureSlots[i];
        GUID texture = barkOverrides[i];
        if (texture.IsNull() && options.bark.textured)
            texture = DefaultBarkTextureGuid(options.bark.type, slot.Suffix);

        if (!texture.IsNull())
            doc.textures[slot.MaterialSlot] = texture.ToString();
        else if (!hasAuthoredMaterial)
            doc.textures[slot.MaterialSlot] = std::string{};
        doc.textureTransforms[slot.MaterialSlot] = barkUv;
    }

    return doc;
}

MaterialDocument BuildLeafMaterialDocument(const EZTree::TreeOptions& options,
                                           const Mathematics::BoundingBox& bounds)
{
    MaterialDocument doc{};
    const GUID authoredMaterial = GuidFromArray(options.leafMaterialGuid);
    const bool hasAuthoredMaterial = TryLoadMaterialDocument(authoredMaterial, doc);

    if (!hasAuthoredMaterial)
        doc = MakeLeafRuntimeMaterialShape();
    else
        doc.materialName = doc.materialName.empty() ? "Tree Generator Leaves" : (doc.materialName + " (Tree Generator)");

    doc.doubleSided = true;
    doc.alphaMode = MaterialAlphaMode::Mask;
    doc.surfaceShader = "Surfaces/ez_tree_leaves.glsl";
    doc.properties["roughness"] = 0.9f;
    doc.properties["metallic"] = 0.0f;
    ApplyWindProperties(doc, options, bounds, kLeafWindScale);

    const bool hasExplicitLeafTint = options.leaves.tint != 0xFFFFFFu;
    if (!hasAuthoredMaterial || hasExplicitLeafTint)
        doc.properties["baseColor"] = ColorFromHex(hasExplicitLeafTint ? options.leaves.tint : DefaultLeafTint(options.leaves.type));
    doc.properties["alphaCutoff"] = std::clamp(options.leaves.alphaTest, 0.0f, 1.0f);

    // Per slot, in kDefaultTextureSlots order: the override the component
    // carries (only albedo has one for leaves), and whether a slot with no
    // texture is cleared or left holding the runtime shape's own value —
    // albedo and ambient occlusion clear, normal and roughness do not.
    const std::array<GUID, 4> leafOverrides{
        GuidFromArray(options.leafColorTextureGuid), GUID::Null(), GUID::Null(), GUID::Null()};
    const std::array<bool, 4> leafClearsWhenAbsent{true, false, false, true};

    const std::array<float, 8> leafUv{
        std::max(0.0001f, options.leaves.textureScale.x),
        0.0f,
        options.leaves.textureOffset.x,
        0.0f,
        0.0f,
        std::max(0.0001f, options.leaves.textureScale.y),
        options.leaves.textureOffset.y,
        0.0f};

    for (size_t i = 0; i < std::size(kDefaultTextureSlots); ++i)
    {
        const DefaultTextureSlot& slot = kDefaultTextureSlots[i];
        GUID texture = leafOverrides[i];
        if (texture.IsNull())
            texture = DefaultLeafTextureGuid(options.leaves.type, slot.Suffix);

        if (!texture.IsNull())
            doc.textures[slot.MaterialSlot] = texture.ToString();
        else if (!hasAuthoredMaterial && leafClearsWhenAbsent[i])
            doc.textures[slot.MaterialSlot] = std::string{};
        doc.textureTransforms[slot.MaterialSlot] = leafUv;
    }

    return doc;
}

MaterialDocument BuildTrellisMaterialDocument(const EZTree::TreeOptions& options)
{
    MaterialDocument doc{};
    const GUID authoredMaterial = GuidFromArray(options.trellisMaterialGuid);
    const bool hasAuthoredMaterial = TryLoadMaterialDocument(authoredMaterial, doc);

    if (!hasAuthoredMaterial)
        doc = MakeTrellisRuntimeMaterialShape();
    else
        doc.materialName = doc.materialName.empty() ? "Tree Generator Trellis" : (doc.materialName + " (Tree Generator)");

    doc.doubleSided = true;
    if (!hasAuthoredMaterial)
        doc.properties["baseColor"] = ColorFromHex(options.trellis.color);

    const GUID trellisTexture = GuidFromArray(options.trellisTextureGuid);
    if (!trellisTexture.IsNull())
        doc.textures["albedoMap"] = trellisTexture.ToString();
    else if (!hasAuthoredMaterial)
        doc.textures["albedoMap"] = std::string{};

    return doc;
}

bool HasValidMaterial(Engine::Renderer::RenderServices& renderServices, const GUID& guid)
{
    const auto* material = renderServices.Materials().Registry().Find(guid);
    return material && material->GetGraphicsPipelineId().IsValid();
}

// The static twin draws the same surface without wind: while the wind material cannot compile,
// so the tree never vanishes, and always for the impostor bake, which bakes the rest pose.
void RegisterWithStaticTwin(Engine::Renderer::RenderServices& renderServices,
                            const GUID& windGuid,
                            const GUID& staticGuid,
                            MaterialDocument doc)
{
    PreloadMaterialDocumentTextures(renderServices, doc);
    renderServices.Materials().RegisterMaterialFromDocument(windGuid, doc);
    doc.vertexModifier.clear();
    renderServices.Materials().RegisterMaterialFromDocument(staticGuid, doc);
}

} // namespace

TreeVariantMaterials VariantMaterialGuids(uint32 slot)
{
    return TreeVariantMaterials{VariantMaterialGuid(slot, "bark"),
                                VariantMaterialGuid(slot, "bark-static"),
                                VariantMaterialGuid(slot, "leaves"),
                                VariantMaterialGuid(slot, "leaves-static"),
                                VariantMaterialGuid(slot, "trellis"),
                                VariantMaterialGuid(slot, "impostor")};
}

void RegisterVariantMaterials(Engine::Renderer::RenderServices& renderServices,
                              const TreeVariantMaterials& materials,
                              const EZTree::TreeOptions& options,
                              const Mathematics::BoundingBox& bounds)
{
    RegisterWithStaticTwin(renderServices, materials.Bark, materials.BarkStatic,
                           BuildBarkMaterialDocument(options, bounds));
    RegisterWithStaticTwin(renderServices, materials.Leaves, materials.LeavesStatic,
                           BuildLeafMaterialDocument(options, bounds));
    MaterialDocument trellis = BuildTrellisMaterialDocument(options);
    PreloadMaterialDocumentTextures(renderServices, trellis);
    renderServices.Materials().RegisterMaterialFromDocument(materials.Trellis, trellis);
}

void RegisterImpostorMaterial(Engine::Renderer::RenderServices& renderServices,
                              const GUID& material,
                              const Engine::Renderer::ImpostorAtlas& atlas,
                              const Mathematics::BoundingBox& bounds,
                              uint32 framesPerSide,
                              Mathematics::OctahedralLayout layout)
{
    MaterialDocument doc{};
    doc.schemaVersion = 3;
    doc.materialName = "Tree Generator Impostor";
    doc.lightingModel = "StandardPBR";
    doc.surfaceShader = "Surfaces/impostor.glsl";
    doc.vertexModifier = "VertexModifiers/impostor_card.glsl";
    doc.alphaMode = MaterialAlphaMode::Mask;
    doc.doubleSided = true;
    doc.properties["alphaCutoff"] = 0.5f;
    doc.properties["impostorBounds"] =
        std::vector<float>{bounds.center.x, bounds.center.y, bounds.center.z, bounds.Radius()};
    doc.properties["impostorAtlas"] = std::vector<float>{
        static_cast<float>(framesPerSide), layout == Mathematics::OctahedralLayout::Hemisphere ? 0.0f : 1.0f, 0.0f, 0.0f};
    doc.textures["albedoMap"] = std::string{};
    doc.textures["normalMap"] = std::string{};
    Engine::Renderer::Material* registered = renderServices.Materials().RegisterMaterialFromDocument(material, doc);
    if (!registered)
        return;
    // The atlas is a runtime texture: bound by handle, as a video frame is.
    const StringId albedo = HashStringId("albedoMap");
    const StringId normal = HashStringId("normalMap");
    registered->SetTexture(albedo, atlas.Color);
    registered->SetBindlessTextureIndex(albedo, renderServices.Textures().GetBindlessIndex(atlas.Color));
    registered->SetTexture(normal, atlas.NormalDepth);
    registered->SetBindlessTextureIndex(normal, renderServices.Textures().GetBindlessIndex(atlas.NormalDepth));
}

bool VariantMaterialsReady(Engine::Renderer::RenderServices& renderServices, const TreeVariantMaterials& materials)
{
    // The static twins keep the tree visible while shader services start, but do
    // not count as ready: a transient startup miss must not leave the tree static.
    return HasValidMaterial(renderServices, materials.Bark)
        && HasValidMaterial(renderServices, materials.Leaves)
        && HasValidMaterial(renderServices, materials.Trellis);
}

GUID SelectBarkMaterial(Engine::Renderer::RenderServices& renderServices, const TreeVariantMaterials& materials)
{
    return HasValidMaterial(renderServices, materials.Bark) ? materials.Bark : materials.BarkStatic;
}

GUID SelectLeafMaterial(Engine::Renderer::RenderServices& renderServices, const TreeVariantMaterials& materials)
{
    return HasValidMaterial(renderServices, materials.Leaves) ? materials.Leaves : materials.LeavesStatic;
}

float WindScaleBound(const EZTree::TreeOptions& options)
{
    return options.wind.enabled ? std::max(kBarkWindScale, kLeafWindScale) : 0.0f;
}

} // namespace GameEngine::EZTreeECS
