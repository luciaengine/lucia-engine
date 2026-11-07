#include "Editor/Hierarchy/HierarchyEntityIcon.h"

#include "AssetCore/Asset.h"
#include "AssetCore/AssetTypes.h"
#include "Assets/PolyhavenService.h"
#include "Components/Animation/AnimatedSprite2D.h"
#include "Components/Rendering/Sprite2D.h"
#include "Components/Hierarchy.h"
#include "Components/Transform.h"
#include "Components/HierarchyQueries.h"
#include "Components/Measure/MeasureComponent.h"
#include "Components/PolyhavenPlaceholder.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Components/Rendering/LensFlareSource.h"
#include "Components/Rendering/Ocean.h"
#include "Components/Rendering/Particles.h"
#include "Components/Rendering/PostProcessVolume.h"
#include "Components/Rendering/ReflectionProbe.h"
#include "Components/Rendering/SkyEnvironment.h"
#include "Components/Rendering/Skybox.h"
#include "Components/Rendering/WindVolume.h"
#include "Components/Spline/SplineComponent.h"
#include "Components/Terrain/Terrain.h"
#include "Components/Terrain/TerrainModifierVolume.h"
#include "Components/Video/VideoTextureComponent.h"
#include "ECS/Components.h"
#include "ECS/ECSTemplates.h"
#include "EditorContext.h"
#include "Engine/Rendering/PrimitiveGenerator.h"
#include "Editor/Entities/EditorComponentTraits.h"
#include "PhysicsECS/Components/PhysicsBody.h"
#include "PhysicsECS/Components/PhysicsCollider.h"
#include "PhysicsECS/Components/PhysicsWorldSettingsComponent.h"
#include "Thumbnails/IThumbnailProvider.h"
#include "UI/Layout/ElementOverrideHelpers.h"
#include "UI/StyleProperties.h"
#include "UI/UIManager.h"

#include "AssetCore/Asset.h"
#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Assets/MaterialAsset.h"
#include "Components/Rendering/Camera.h"
#include "Components/Rendering/Light.h"

#include <algorithm>
#include <cmath>
#include <filesystem>

namespace GameEngine::Editor
{
namespace
{
using Components::MeshRenderer;

inline GUID ModelGuidFromMeshRenderer(const MeshRenderer& mr)
{
    return mr.modelAssetGuid.ToGuid();
}

inline GUID MaterialGuidFromMeshRenderer(const MeshRenderer& mr)
{
    return mr.materialAssetGuid.ToGuid();
}

inline bool MeshRendererReferencesModelAsset(const MeshRenderer& mr)
{
    return !mr.modelAssetGuid.IsNull();
}

// Textured planes and legacy thin XY cubes can both carry sprite atlas regions.
// Keep ordinary 3D cubes on their primitive icon path.
inline bool IsSpriteMeshRenderer(const MeshRenderer& mr, ECS::World* world, ECS::EntityHandle handle)
{
    using PG = Engine::Renderer::PrimitiveGenerator;
    const GUID meshGuid = ModelGuidFromMeshRenderer(mr);
    if (meshGuid != PG::PlaneGuid() && meshGuid != PG::PlaneSpriteUvGuid())
    {
        const auto* transform = world->GetComponent<Components::Transform>(handle);
        if (meshGuid != PG::CubeGuid() || !transform) return false;
        const auto scale = transform->GetScale();
        if (scale.z > 0.1f * std::min(scale.x, scale.y)) return false;
    }
    const GUID matGuid = MaterialGuidFromMeshRenderer(mr);
    if (matGuid.IsNull() || matGuid == PG::DefaultMaterialGuid())
        return false;
    return true;
}

// Resolves the active model-thumb coloring mode from the owning UIManager root.
// Returns true if model thumbnails should be shown in natural color (saturation=1 / tint=white),
// i.e. when either the main "Colored Hierarchy Icons" flag or the "3D Model Thumbnails Always
// Colored" flag is on.
static bool ShouldColorModelThumb(UIElement& titleEl)
{
    UIManager* mgr = titleEl.GetOwnerManager();
    if (!mgr)
        return false;
    UIElement* root = mgr->GetRootElement();
    if (!root)
        return false;
    return root->HasClass("hierarchy-model-thumbs-colored") ||
           root->HasClass("hierarchy-icons-colored");
}

static void ApplyModelThumbSaturationOverride(UIElement& titleEl)
{
    if (ShouldColorModelThumb(titleEl))
        titleEl.Overrides().Set(Style::BackgroundImageSaturation, 1.0f);
}

// The Inspector header icon sits in a fixed 32x32 slot whose CSS centers every
// other glyph in it; a letterboxed sprite follows suit.
constexpr float kInspectorIconAlignPercent = 50.0f;

static bool IsMeasureEndpointEntity(ECS::World& world, ECS::EntityHandle handle)
{
    bool found = false;
    world.Query<ECS::Read<Components::MeasureComponent>>()
        .IncludeDisabled()
        .Each([&](ECS::EntityHandle, const Components::MeasureComponent& measure)
              {
            if (measure.StartEntity == handle || measure.EndEntity == handle)
                found = true; });
    return found;
}

} // namespace

bool EntityIconIsCssOnly(const UIElement& element)
{
    static constexpr const char* kCssOnly[] = {
        "hierarchy-entity-camera",
        "hierarchy-entity-sky",
        "hierarchy-entity-reflection-probe",
        "hierarchy-entity-light-ambient",
        "hierarchy-entity-light-directional",
        "hierarchy-entity-light-spot",
        "hierarchy-entity-light-point",
        "hierarchy-entity-light-area",
        "hierarchy-entity-light-volume",
        "hierarchy-entity-shape-cube",
        "hierarchy-entity-shape-sphere",
        "hierarchy-entity-shape-capsule",
        "hierarchy-entity-shape-plane",
        "hierarchy-entity-terrain",
        "hierarchy-entity-planet",
        "hierarchy-entity-tree-generator",
        "hierarchy-entity-ocean",
        "hierarchy-entity-spline",
        "hierarchy-entity-measure",
        "hierarchy-entity-measure-point",
        "hierarchy-entity-physics",
        "hierarchy-entity-physics-settings",
        "hierarchy-entity-particles",
        "hierarchy-entity-video-texture",
        "hierarchy-entity-lens-flare",
        "hierarchy-entity-post-process-volume",
        "hierarchy-entity-wind-volume",
        "hierarchy-entity-terrain-surface-rules",
        "hierarchy-entity-empty",
    };
    for (const char* c : kCssOnly)
    {
        if (element.HasClass(c))
            return true;
    }
    return false;
}

std::filesystem::path TryResolveSpriteAlbedoTexturePath(ECS::World* world,
                                                        ECS::EntityHandle handle,
                                                        AssetManager* assets)
{
    if (!world || !assets || !handle.IsValid() || !world->IsValid(handle))
        return {};

    const auto* sprite = world->GetComponent<Components::Sprite2D>(handle);
    if(sprite && !sprite->Texture.IsNull()) {
        AssetMetadata meta;
        if(assets->GetRegistry().TryGetAssetMetadata(sprite->Texture.ToGuid(),meta)) return assets->ResolveAssetPath(meta.Path);
    }
    const auto* mr = world->GetComponent<MeshRenderer>(handle);
    if (!sprite && (!mr || !IsSpriteMeshRenderer(*mr, world, handle)))
        return {};

    const GUID matGuid = sprite ? sprite->Material.ToGuid() : MaterialGuidFromMeshRenderer(*mr);
    if (matGuid.IsNull())
        return {};

    SharedPtr<Asset> asset = assets->GetAsset(matGuid);
    if (!asset)
    {
        // Fire-and-forget async load — the icon falls back to the default
        // sprite class for now; the next hierarchy panel repaint (after
        // the load lands) will resolve the texture path on retry.
        assets->LoadAsset(matGuid, AssetLoadResultCallback{},
                          AssetLoadPriority::High);
        return {};
    }
    auto* matAsset = dynamic_cast<MaterialAsset*>(asset.get());
    if (!matAsset)
        return {};

    const MaterialDocument& doc = matAsset->GetDocument();
    auto it = doc.textures.find("albedoMap");
    if (it == doc.textures.end() || it->second.empty())
        return {};

    const GUID texGuid(it->second);
    if (texGuid.IsNull())
        return {};

    AssetMetadata meta;
    if (!assets->GetRegistry().TryGetAssetMetadata(texGuid, meta))
        return {};

    return assets->ResolveAssetPath(meta.Path);
}

std::array<float, 2> ResolveSpritePreviewKey(ECS::World* world, ECS::EntityHandle handle, AssetManager* assets)
{
    const auto* mr = world->GetComponent<MeshRenderer>(handle);
    if (!mr) return {-1,0};
    auto asset = assets->GetAsset(MaterialGuidFromMeshRenderer(*mr));
    const auto* material = dynamic_cast<MaterialAsset*>(asset.get());
    if (!material) return {-1,0};
    const auto& properties = material->GetDocument().properties;
    const auto it = properties.find("previewMagentaKey");
    if (it != properties.end())
        if (const auto* values = std::get_if<std::vector<float>>(&it->second); values && values->size()==2)
            return {(*values)[0],(*values)[1]};
    return {-1,0};
}

std::array<float, 4> ResolveSpritePreviewUV(ECS::World* world, ECS::EntityHandle handle, AssetManager* assets)
{
    std::array<float, 4> uv{0,0,1,1};
    const auto* sprite=world->GetComponent<Components::Sprite2D>(handle);
    if(sprite)uv={sprite->U0,sprite->V0,sprite->U1,sprite->V1};
    const auto* mr = world->GetComponent<MeshRenderer>(handle);
    if (!mr && !sprite) return uv;
    auto asset = assets->GetAsset(sprite?sprite->Material.ToGuid():MaterialGuidFromMeshRenderer(*mr));
    const auto* material = dynamic_cast<MaterialAsset*>(asset.get());
    if (!material) { if(sprite && sprite->FlipX)std::swap(uv[0],uv[2]);if(sprite && sprite->FlipY)std::swap(uv[1],uv[3]);return uv; }
    const auto& transforms = material->GetDocument().textureTransforms;
    const auto it = transforms.find("albedoMap");
    if (it != transforms.end())
    {
        const auto& t = it->second;
        uv = {t[2]+t[0]*uv[0],t[6]+t[5]*uv[1],t[2]+t[0]*uv[2],t[6]+t[5]*uv[3]};
    }
    if(sprite && sprite->FlipX)std::swap(uv[0],uv[2]);
    if(sprite && sprite->FlipY)std::swap(uv[1],uv[3]);
    return uv;
}

SpriteFramesPreviewRequest TryResolveSpriteFramesPreview(ECS::World* world,
                                                         ECS::EntityHandle handle,
                                                         AssetManager* assets)
{
    SpriteFramesPreviewRequest request;
    if (!world || !assets || !handle.IsValid() || !world->IsValid(handle))
        return request;

    const auto* sprite = world->GetComponent<Components::AnimatedSprite2D>(handle);
    if (!sprite || sprite->SpriteFrames.IsNull())
        return request;

    AssetMetadata meta;
    if (!assets->GetRegistry().TryGetAssetMetadata(sprite->SpriteFrames.ToGuid(), meta))
        return request;

    request.AssetPath = assets->ResolveAssetPath(meta.Path);
    request.AnimationName = std::string(sprite->GetAnimationName());
    request.FlipX = sprite->FlipX;
    return request;
}

std::filesystem::path TryResolveInspectorIconAssetPath(ECS::World* world,
                                                       ECS::EntityHandle handle,
                                                       AssetManager* assets)
{
    if (!world || !assets || !handle.IsValid() || !world->IsValid(handle))
        return {};

    if (const auto* sprite = world->GetComponent<Components::AnimatedSprite2D>(handle))
    {
        AssetMetadata meta;
        if (!sprite->SpriteFrames.IsNull() &&
            assets->GetRegistry().TryGetAssetMetadata(sprite->SpriteFrames.ToGuid(), meta))
        {
            return meta.Path;
        }
    }

    if (const auto* mr = world->GetComponent<MeshRenderer>(handle))
    {
        if (IsSpriteMeshRenderer(*mr, world, handle))
        {
            const GUID matGuid = MaterialGuidFromMeshRenderer(*mr);
            if (!matGuid.IsNull())
            {
                SharedPtr<Asset> asset = assets->GetAsset(matGuid);
                if (!asset)
                {
                    assets->LoadAsset(matGuid, AssetLoadResultCallback{},
                                      AssetLoadPriority::High);
                    return {};
                }
                if (auto* matAsset = dynamic_cast<MaterialAsset*>(asset.get()))
                {
                    const MaterialDocument& doc = matAsset->GetDocument();
                    auto it = doc.textures.find("albedoMap");
                    if (it != doc.textures.end() && !it->second.empty())
                    {
                        const GUID texGuid(it->second);
                        AssetMetadata meta;
                        if (!texGuid.IsNull() && assets->GetRegistry().TryGetAssetMetadata(texGuid, meta))
                            return meta.Path;
                    }
                }
            }
        }
    }

    const GUID modelGuid = FindModelGuidForEntityOrChildren(world, handle);
    if (!modelGuid.IsNull())
    {
        AssetMetadata meta;
        if (assets->GetRegistry().TryGetAssetMetadata(modelGuid, meta))
            return meta.Path;
    }

    return {};
}

GUID FindModelGuidForEntityOrChildren(ECS::World* world, ECS::EntityHandle handle)
{
    if (!world || !handle.IsValid() || !world->IsValid(handle))
        return {};

    if (const auto* mr = world->GetComponent<MeshRenderer>(handle))
    {
        if (MeshRendererReferencesModelAsset(*mr))
        {
            const GUID g = ModelGuidFromMeshRenderer(*mr);
            if (!g.IsNull() && !Engine::Renderer::PrimitiveGenerator::IsPrimitive(g))
                return g;
        }
    }

    // Imported models routinely place the MeshRenderer several levels deep
    // (under an Armature, or on a submesh under a transform pivot), so a
    // direct-child scan misses them. BFS the full subtree and return the
    // first non-primitive model GUID we find.
    std::vector<ECS::EntityHandle> descendants;
    Components::DescendantsOf(*world, handle, descendants);
    for (const auto& descendant : descendants)
    {
        const auto* mr = world->GetComponent<MeshRenderer>(descendant);
        if (!mr || !MeshRendererReferencesModelAsset(*mr))
            continue;
        const GUID g = ModelGuidFromMeshRenderer(*mr);
        if (!g.IsNull() && !Engine::Renderer::PrimitiveGenerator::IsPrimitive(g))
            return g;
    }
    return {};
}

namespace {
/* The shared marker every entity-icon rule keys on. It goes on the element that
   actually draws the glyph — the tree's .hierarchy-preview-icon, the inspector
   header icon, the bookmark icon — so one selector reaches all three. Never on
   a row: a row carrying it would paint the glyph as its own background. */
constexpr const char* kEntityIconMarker = "entity-icon";
} // namespace

void ClearHierarchyEntityIconClasses(UIElement* row)
{
    if (!row)
        return;
    row->RemoveClass(kEntityIconMarker);
    static constexpr const char* kClasses[] = {
        "hierarchy-entity-camera",
        "hierarchy-entity-sky",
        "hierarchy-entity-reflection-probe",
        "hierarchy-entity-light-ambient",
        "hierarchy-entity-light-directional",
        "hierarchy-entity-light-spot",
        "hierarchy-entity-light-point",
        "hierarchy-entity-light-area",
        "hierarchy-entity-light-volume",
        "hierarchy-entity-shape-cube",
        "hierarchy-entity-shape-sphere",
        "hierarchy-entity-shape-capsule",
        "hierarchy-entity-shape-plane",
        "hierarchy-entity-model",
        "hierarchy-entity-sprite",
        "hierarchy-entity-animated-sprite",
        "hierarchy-entity-physics",
        "hierarchy-entity-physics-settings",
        "hierarchy-entity-terrain",
        "hierarchy-entity-planet",
        "hierarchy-entity-tree-generator",
        "hierarchy-entity-ocean",
        "hierarchy-entity-spline",
        "hierarchy-entity-measure",
        "hierarchy-entity-measure-point",
        "hierarchy-entity-particles",
        "hierarchy-entity-video-texture",
        "hierarchy-entity-lens-flare",
        "hierarchy-entity-post-process-volume",
        "hierarchy-entity-wind-volume",
        "hierarchy-entity-terrain-surface-rules",
        "hierarchy-entity-empty",
    };
    for (const char* c : kClasses)
        row->RemoveClass(c);

    // Classes contributed by registered component traits are not in the list
    // above and cannot be — packages register them at runtime. Clearing them
    // from the same registry Apply reads means a row recycled from an entity
    // whose type came from a package does not keep that package's icon.
    for (const auto& [typeId, rowClass] : Editor::EditorComponentTraitsRegistry::Get().HierarchyRowClasses())
    {
        (void)typeId;
        row->RemoveClass(rowClass);
    }
}

void ApplyHierarchyRowDecorations(ECS::World& world, ECS::EntityHandle entity, HierarchyRowParts& parts)
{
    for (const auto& decorate : EditorComponentTraitsRegistry::Get().HierarchyRowDecorators())
        decorate(world, entity, parts);
}

void ApplyHierarchyEntityIconClasses(ECS::World* world, UIElement* row, ECS::EntityHandle handle)
{
    ClearHierarchyEntityIconClasses(row);
    if (!world || !row || !handle.IsValid() || !world->IsValid(handle))
        return;

    // Plugin-owned components (editor packages) contribute row classes via
    // registered traits.
    for (const auto& [typeId, rowClass] : Editor::EditorComponentTraitsRegistry::Get().HierarchyRowClasses())
    {
        if (world->HasComponent(handle, typeId))
        {
            row->AddClass(rowClass);
            return;
        }
    }

    if (world->GetComponent<Components::Camera>(handle) != nullptr)
    {
        row->AddClass("hierarchy-entity-camera");
        return;
    }
    if (world->GetComponent<Components::Skybox>(handle) != nullptr ||
        world->GetComponent<Components::SkyEnvironment>(handle) != nullptr)
    {
        row->AddClass("hierarchy-entity-sky");
        return;
    }
    if (world->GetComponent<Components::ReflectionProbe>(handle) != nullptr)
    {
        row->AddClass("hierarchy-entity-reflection-probe");
        return;
    }
    if (const auto* light = world->GetComponent<Components::Light>(handle))
    {
        using LT = Components::LightType;
        switch (light->Type)
        {
        case LT::Ambient:
            row->AddClass("hierarchy-entity-light-ambient");
            return;
        case LT::Directional:
            row->AddClass("hierarchy-entity-light-directional");
            return;
        case LT::Spot:
            row->AddClass("hierarchy-entity-light-spot");
            return;
        case LT::Point:
            row->AddClass("hierarchy-entity-light-point");
            return;
        case LT::Area:
            row->AddClass("hierarchy-entity-light-area");
            return;
        case LT::Volume:
            row->AddClass("hierarchy-entity-light-volume");
            return;
        default:
            return;
        }
    }

    // Before the mesh and physics checks: a 2D sprite is what the entity looks
    // like, and a platformer character carries a PhysicsBody that would
    // otherwise claim the row.
    if (world->GetComponent<Components::Sprite2D>(handle) != nullptr) {
        row->AddClass("hierarchy-entity-sprite");return;
    }
    if (world->GetComponent<Components::AnimatedSprite2D>(handle) != nullptr)
    {
        row->AddClass("hierarchy-entity-animated-sprite");
        return;
    }

    if (world->GetComponent<Components::ParticleEmitter3D>(handle) != nullptr)
    {
        row->AddClass("hierarchy-entity-particles");
        return;
    }

    if (world->GetComponent<Components::VideoTextureComponent>(handle) != nullptr)
    {
        row->AddClass("hierarchy-entity-video-texture");
        return;
    }

    if (const auto* meshRenderer = world->GetComponent<MeshRenderer>(handle))
    {
        const GUID g = ModelGuidFromMeshRenderer(*meshRenderer);
        using PG = Engine::Renderer::PrimitiveGenerator;
        if (PG::IsPrimitive(g))
        {
            if (g == PG::CubeGuid())
            {
                row->AddClass(IsSpriteMeshRenderer(*meshRenderer, world, handle) ? "hierarchy-entity-sprite" : "hierarchy-entity-shape-cube");
                return;
            }
            if (g == PG::SphereGuid())
            {
                row->AddClass("hierarchy-entity-shape-sphere");
                return;
            }
            if (g == PG::CapsuleGuid())
            {
                row->AddClass("hierarchy-entity-shape-capsule");
                return;
            }
            if (g == PG::PlaneSpriteUvGuid())
            {
                if (IsSpriteMeshRenderer(*meshRenderer, world, handle))
                    row->AddClass("hierarchy-entity-sprite");
                else
                    row->AddClass("hierarchy-entity-shape-plane");
                return;
            }
            if (g == PG::PlaneGuid())
            {
                if (!world->GetComponent<Components::PolyhavenPlaceholder>(handle))
                {
                    if (IsSpriteMeshRenderer(*meshRenderer, world, handle))
                        row->AddClass("hierarchy-entity-sprite");
                    else
                        row->AddClass("hierarchy-entity-shape-plane");
                    return;
                }
            }
            return;
        }
        if (MeshRendererReferencesModelAsset(*meshRenderer))
        {
            row->AddClass("hierarchy-entity-model");
            return;
        }
        return;
    }

    if (world->GetComponent<Components::OceanSurface>(handle) != nullptr)
    {
        row->AddClass("hierarchy-entity-ocean");
        return;
    }

    if (world->GetComponent<Components::Terrain>(handle) != nullptr)
    {
        const auto* terrain = world->GetComponent<Components::Terrain>(handle);
        if (terrain && terrain->Domain == Components::TerrainDomain::Spherical)
            row->AddClass("hierarchy-entity-planet");
        else
            row->AddClass("hierarchy-entity-terrain");
        return;
    }

    if (world->GetComponent<Components::TerrainModifierVolume>(handle) != nullptr)
    {
        row->AddClass("hierarchy-entity-terrain");
        return;
    }

    if (world->GetComponent<Components::SplineComponent>(handle) != nullptr)
    {
        row->AddClass("hierarchy-entity-spline");
        return;
    }

    if (world->GetComponent<Components::MeasureComponent>(handle) != nullptr)
    {
        row->AddClass("hierarchy-entity-measure");
        return;
    }

    if (IsMeasureEndpointEntity(*world, handle))
    {
        row->AddClass("hierarchy-entity-measure-point");
        return;
    }

    if (world->GetComponent<Components::PostProcessVolume>(handle) != nullptr)
    {
        row->AddClass("hierarchy-entity-post-process-volume");
        return;
    }

    if (world->GetComponent<Components::WindVolume>(handle) != nullptr)
    {
        row->AddClass("hierarchy-entity-wind-volume");
        return;
    }

    if (world->GetComponent<Components::LensFlareSource>(handle) != nullptr)
    {
        row->AddClass("hierarchy-entity-lens-flare");
        return;
    }

    if (world->GetComponent<Components::PhysicsWorldSettingsComponent>(handle) != nullptr)
    {
        row->AddClass("hierarchy-entity-physics-settings");
        return;
    }

    if (world->GetComponent<Components::PhysicsBody>(handle) != nullptr ||
        world->GetComponent<Components::PhysicsCollider>(handle) != nullptr)
    {
        row->AddClass("hierarchy-entity-physics");
        return;
    }

    const GUID childModelGuid = FindModelGuidForEntityOrChildren(world, handle);
    if (!childModelGuid.IsNull())
    {
        row->AddClass("hierarchy-entity-model");
        return;
    }

    row->AddClass("hierarchy-entity-empty");
}

void ApplyHierarchyEntityIconClassesToIcon(ECS::World* world, UIElement* icon,
                                           ECS::EntityHandle handle)
{
    ApplyHierarchyEntityIconClasses(world, icon, handle);
    if (!icon)
        return;
    /* Only the glyph-drawing element gets the marker, so `.entity-icon` plus the
       kind class is one selector for the tree, the inspector header and the
       bookmark row alike. */
    icon->AddClass(kEntityIconMarker);
}

void ApplyTitleModelThumbnail(UIElement& titleEl, const std::string& relOrEngine, float iconSizePx)
{
    if (relOrEngine.empty())
    {
        titleEl.RemoveClass("hierarchy-entity-model-thumb");
        UI::Layout::DisableBackgroundOverride(titleEl);
        return;
    }
    constexpr uint32_t kBackgroundTintNoDim = 0xFFFFFFFFu;
    const float icon = std::max(1.0f, iconSizePx);
    BackgroundImageSource source{};
    constexpr const char* kEnginePrefix = "engine:";
    constexpr size_t kEnginePrefixLen = 7;
    if (relOrEngine.rfind(kEnginePrefix, 0) == 0)
    {
        source.Kind = BackgroundImageSource::SourceKind::ResourceName;
        source.Value = relOrEngine.substr(kEnginePrefixLen);
    }
    else
    {
        source.Kind = BackgroundImageSource::SourceKind::Path;
        source.Value = relOrEngine;
    }
    BackgroundSizeValue size{};
    size.Mode = BackgroundSizeMode::Explicit;
    size.SizeX = icon;
    size.SizeXIsPercent = false;
    size.SizeY = icon;
    size.SizeYIsPercent = false;
    BackgroundPositionValue pos{0.0f, true, 50.0f, true};

    titleEl.AddClass("hierarchy-entity-model-thumb");
    titleEl.Overrides()
        .Set(Style::BackgroundImage, source)
        .Set(Style::BackgroundRepeatProp, BackgroundRepeat::NoRepeat)
        .Set(Style::BackgroundSize, size)
        .Set(Style::BackgroundPosition, pos)
        .Set(Style::BackgroundTint, kBackgroundTintNoDim);
    ApplyModelThumbSaturationOverride(titleEl);
    titleEl.MarkDirty(UIElement::VisualDirty);
}

void ApplySpriteFrameThumbnail(UIElement& iconEl, const std::string& thumbPath,
                               float horizontalAlignPercent)
{
    if (thumbPath.empty())
        return;

    constexpr uint32_t kBackgroundTintNoDim = 0xFFFFFFFFu;
    BackgroundImageSource source{};
    source.Kind = BackgroundImageSource::SourceKind::Path;
    source.Value = thumbPath;
    // Contain, not an explicit square: a sprite frame is rarely square, and
    // stretching it to the icon box is the one thing a preview must not do.
    BackgroundSizeValue size{};
    size.Mode = BackgroundSizeMode::Contain;
    BackgroundPositionValue pos{horizontalAlignPercent, true, 50.0f, true};

    iconEl.AddClass("hierarchy-entity-model-thumb");
    iconEl.Overrides()
        .Set(Style::BackgroundImage, source)
        .Set(Style::BackgroundRepeatProp, BackgroundRepeat::NoRepeat)
        .Set(Style::BackgroundSize, size)
        .Set(Style::BackgroundPosition, pos)
        .Set(Style::BackgroundTint, kBackgroundTintNoDim);
    ApplyModelThumbSaturationOverride(iconEl);
    iconEl.MarkDirty(UIElement::VisualDirty);
}

void ApplyInspectorEntityIcon(ECS::World* world,
                              const EditorContext* ctx,
                              UIElement& iconEl,
                              ECS::EntityHandle handle,
                              float iconSizePx,
                              UIManager* uiManager,
                              bool uiReplayActive,
                              UIElement* postActionHost)
{
    iconEl.RemoveClass("hierarchy-entity-model-thumb");
    UI::Layout::DisableBackgroundOverride(iconEl);

    ApplyHierarchyEntityIconClassesToIcon(world, &iconEl, handle);

    if (EntityIconIsCssOnly(iconEl))
        return;

    if (!world || !handle.IsValid() || !world->IsValid(handle))
        return;

    if (!ctx || uiReplayActive)
        return;

    const float modelThumbPx = std::max(1.0f, iconSizePx * 2.0f);

    const auto* phPlaceholder = world->GetComponent<Components::PolyhavenPlaceholder>(handle);
    if (phPlaceholder && phPlaceholder->slug[0] != '\0')
    {
        std::filesystem::path thumbPath = PolyhavenService::GetCacheDir() / (std::string(phPlaceholder->Slug()) + ".png");
        std::error_code ec;
        if (std::filesystem::exists(thumbPath, ec) && uiManager && ctx->Assets)
        {
            std::string pathStr = thumbPath.string();
            GUID thumbGuid = uiManager->ResolveBackgroundImagePath(pathStr);
            if (!thumbGuid.IsNull())
            {
                // Uploads at once when the texture is loaded, otherwise requests it; never
                // waits (a first open can still be cooking it).
                uiManager->EnsureBackgroundTextureUploaded(thumbGuid);

                const float icon = modelThumbPx;
                BackgroundImageSource source{};
                source.Kind = BackgroundImageSource::SourceKind::Path;
                source.Value = pathStr;
                BackgroundSizeValue size{};
                size.Mode = BackgroundSizeMode::Explicit;
                size.SizeX = icon;
                size.SizeXIsPercent = false;
                size.SizeY = icon;
                size.SizeYIsPercent = false;
                BackgroundPositionValue pos{0.0f, true, 50.0f, true};
                iconEl.AddClass("hierarchy-entity-model-thumb");
                iconEl.Overrides()
                    .Set(Style::BackgroundImage, source)
                    .Set(Style::BackgroundRepeatProp, BackgroundRepeat::NoRepeat)
                    .Set(Style::BackgroundSize, size)
                    .Set(Style::BackgroundPosition, pos)
                    .Set(Style::BackgroundTint, uint32_t(0xFFFFFFFFu));
                ApplyModelThumbSaturationOverride(iconEl);
                iconEl.MarkDirty(UIElement::VisualDirty);
            }
        }
        return;
    }

    // Sprite sheets are useless as previews, so the thumbnail service crops the
    // entity's current frame out of its sheet — the same request, cache and crop
    // the Hierarchy rows use, so header and row always agree.
    if (ctx->Assets && ctx->Thumbnails && postActionHost && uiManager)
    {
        const SpriteFramesPreviewRequest request =
            TryResolveSpriteFramesPreview(world, handle, ctx->Assets);
        if (!request.AssetPath.empty())
        {
            const uint64_t iconInstanceId = iconEl.GetInstanceId();
            const std::string immediate = ctx->Thumbnails->GetOrRequestSpriteFrame(
                request.AssetPath, request.AnimationName, request.FlipX,
                [postActionHost, iconInstanceId, uiManager](const std::string& rel)
                {
                    if (rel.empty())
                        return;
                    postActionHost->PostAction(
                        [iconInstanceId, uiManager, rel]()
                        {
                            if (!uiManager)
                                return;
                            UIElement* icon = uiManager->FindElementByInstanceId(iconInstanceId);
                            if (!icon)
                                return;
                            ApplySpriteFrameThumbnail(*icon, rel, kInspectorIconAlignPercent);
                        });
                });
            if (!immediate.empty())
                ApplySpriteFrameThumbnail(iconEl, immediate, kInspectorIconAlignPercent);
            return;
        }
    }



    // Sprite sheets are useless as previews, so the thumbnail service crops the
    // entity's current frame out of its sheet — the same request, cache and crop
    // the Hierarchy rows use, so header and row always agree.
    if (ctx->Assets && ctx->Thumbnails && postActionHost && uiManager)
    {
        const auto path = TryResolveSpriteAlbedoTexturePath(world, handle, ctx->Assets);
        if (!path.empty())
        {
            const uint64_t iconInstanceId = iconEl.GetInstanceId();
            const std::string immediate = ctx->Thumbnails->GetOrRequestAtlasRegion(
                path, ResolveSpritePreviewUV(world, handle, ctx->Assets),
                [postActionHost, iconInstanceId, uiManager](const std::string& rel)
                {
                    if (rel.empty())
                        return;
                    postActionHost->PostAction(
                        [iconInstanceId, uiManager, rel]()
                        {
                            if (!uiManager)
                                return;
                            UIElement* icon = uiManager->FindElementByInstanceId(iconInstanceId);
                            if (!icon)
                                return;
                            ApplySpriteFrameThumbnail(*icon, rel, kInspectorIconAlignPercent);
                        });
                }, ResolveSpritePreviewKey(world, handle, ctx->Assets));
            if (!immediate.empty())
                ApplySpriteFrameThumbnail(iconEl, immediate, kInspectorIconAlignPercent);
            return;
        }
    }



    if (ctx->Assets && uiManager)
    {
        const std::filesystem::path spritePath =
            TryResolveSpriteAlbedoTexturePath(world, handle, ctx->Assets);
        if (!spritePath.empty())
        {
            std::error_code ec;
            if (std::filesystem::exists(spritePath, ec))
            {
                const std::string pathStr = spritePath.string();
                const GUID bgGuid = uiManager->ResolveBackgroundImagePath(pathStr);
                if (!bgGuid.IsNull())
                {
                    auto texAsset = ctx->Assets->GetAsset(bgGuid);
                    if (!texAsset)
                    {
                        // Texture not yet cached — kick async; fall through
                        // and let the next repaint pick up the loaded asset.
                        ctx->Assets->LoadAsset(bgGuid,
                                               AssetLoadResultCallback{},
                                               AssetLoadPriority::High);
                    }
                    uiManager->EnsureBackgroundTextureUploaded(bgGuid);

                    (void)modelThumbPx;
                    BackgroundImageSource source{};
                    source.Kind = BackgroundImageSource::SourceKind::Path;
                    source.Value = pathStr;
                    BackgroundSizeValue size{};
                    size.Mode = BackgroundSizeMode::Contain;
                    BackgroundPositionValue pos{50.0f, true, 50.0f, true};
                    iconEl.AddClass("hierarchy-entity-model-thumb");
                    iconEl.Overrides()
                        .Set(Style::BackgroundImage, source)
                        .Set(Style::BackgroundRepeatProp, BackgroundRepeat::NoRepeat)
                        .Set(Style::BackgroundSize, size)
                        .Set(Style::BackgroundPosition, pos)
                        .Set(Style::BackgroundTint, uint32_t(0xFFFFFFFFu));
                    ApplyModelThumbSaturationOverride(iconEl);
                    iconEl.MarkDirty(UIElement::VisualDirty);
                    return;
                }
            }
        }
    }

    if (ctx->Assets && ctx->Thumbnails)
    {
        const GUID modelGuid = FindModelGuidForEntityOrChildren(world, handle);
        if (!modelGuid.IsNull())
        {
            AssetMetadata meta;
            if (ctx->Assets->GetRegistry().TryGetAssetMetadata(modelGuid, meta))
            {
                const std::filesystem::path absPath = ctx->Assets->ResolveAssetPath(meta.Path);
                if (!absPath.empty() && postActionHost && uiManager)
                {
                    const int desiredSize =
                        static_cast<int>(std::clamp(modelThumbPx * 3.0f, 48.0f, 256.0f));
                    const auto weakIcon = UIElement::MakeWeakRef(&iconEl);
                    const UI::UiPostHandle post = postActionHost->GetPostHandle();

                    std::string immediate = ctx->Thumbnails->GetOrRequest(
                        absPath,
                        desiredSize,
                        [post, weakIcon, modelThumbPx](const std::string& rel)
                        {
                            if (rel.empty())
                                return;
                            post.Post([weakIcon, rel, modelThumbPx]()
                                                       {
                                                           UIElement* te = weakIcon.Get();
                                                           if (!te)
                                                               return;
                                                           ApplyTitleModelThumbnail(*te, rel, modelThumbPx); });
                        },
                        true);
                    if (!immediate.empty())
                        ApplyTitleModelThumbnail(iconEl, immediate, modelThumbPx);
                }
            }
        }
    }
}

} // namespace GameEngine::Editor
