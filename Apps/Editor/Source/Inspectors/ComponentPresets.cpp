#include "Inspectors/ComponentPresets.h"
#include "UI/EditorIcons.h"

#include "Components/Animation/ValueCurve.h"
#include "Components/Audio/AudioEmitter.h"
#include "Components/Audio/AudioListener.h"
#include "Components/Rendering/Camera.h"
#include "Components/Rendering/Light.h"
#include "Components/Rendering/Particles.h"
#include "Components/Rendering/PostProcessVolume.h"
#include "Components/Rendering/SkyEnvironment.h"
#include "Components/Rendering/Skybox.h"
#include "Components/Rendering/WindVolume.h"
#include "Components/Spline/SplineComponent.h"
#include "Components/Terrain/TerrainModifierEffects.h"
#include "Components/Terrain/TerrainModifierVolume.h"
#include "Components/Video/VideoTextureComponent.h"
#include "ECS/ECS.h"
#include "ECS/ECSTemplates.h" // World::HasComponent / AddComponentImmediate definitions
#include "PhysicsECS/Components/BoxColliderShape.h"
#include "PhysicsECS/Components/CharacterController.h"
#include "PhysicsECS/Components/PhysicsBody.h"
#include "PhysicsECS/Components/PhysicsCollider.h"
#include "PhysicsECS/Components/PlaneColliderShape.h"
#include "PhysicsECS/Components/SphereColliderShape.h"
#include "TerrainECS/TerrainModifierComponents.h"

#include <algorithm>
#include <cassert>
#include <iterator>

namespace GameEngine::Editor
{
namespace
{

using Preset = ComponentPreset;

// One row per canonical terrain effect: the Add Component preset that creates a
// volume carrying it. The three asserts below keep it honest against TerrainECS's
// canonical list — the count static_assert catches an effect added there with no
// preset here, the contiguity static_assert catches a Terrain*Area enumerator
// added to ComponentPreset with no row, and the runtime identity check catches
// an effect replaced or reordered (which neither count nor order can see).
constexpr TerrainAreaPreset kTerrainAreaPresets[] = {
    {Preset::TerrainFlattenArea, "Terrain: Flatten Area",
     ECS::GetComponentTypeId<Components::TerrainFlattenEffect>()},
    {Preset::TerrainHeightOffsetArea, "Terrain: Height Offset Area",
     ECS::GetComponentTypeId<Components::TerrainHeightOffsetEffect>()},
    {Preset::TerrainNoiseArea, "Terrain: Noise Area",
     ECS::GetComponentTypeId<Components::TerrainNoiseEffect>()},
    {Preset::TerrainStampArea, "Terrain: Stamp Area",
     ECS::GetComponentTypeId<Components::TerrainStampEffect>()},
    {Preset::TerrainPaintArea, "Terrain: Paint Area",
     ECS::GetComponentTypeId<Components::TerrainPaintLayerEffect>()},
    {Preset::TerrainRulesArea, "Terrain: Surface Rules Area",
     ECS::GetComponentTypeId<Components::TerrainSurfaceRulesEffect>()},
    {Preset::TerrainGroundClaimArea, "Terrain: Ground Claim",
     ECS::GetComponentTypeId<Components::TerrainGroundClaimEffect>()},
    {Preset::TerrainGrassArea, "Terrain: Grass Area",
     ECS::GetComponentTypeId<Components::TerrainGrassEffect>()},
};

static_assert(std::size(kTerrainAreaPresets) == TerrainECS::kTerrainEffectTypeCount,
              "Every terrain effect needs an Add Component preset: add a row to "
              "kTerrainAreaPresets for the type just added to ModifierEffectComponents.");

// The rows occupy one ascending run of ComponentPreset enumerators, so a
// Terrain*Area enumerator inserted into that run without a row here leaves a
// hole. With the count assert above, the preset set and the canonical effect
// set can only change together.
constexpr bool TerrainAreaPresetsAreContiguous()
{
    for (std::size_t i = 1; i < std::size(kTerrainAreaPresets); ++i)
    {
        const auto previous = static_cast<uint8_t>(kTerrainAreaPresets[i - 1].PresetId);
        if (static_cast<uint8_t>(kTerrainAreaPresets[i].PresetId) != previous + 1)
            return false;
    }
    return true;
}

static_assert(TerrainAreaPresetsAreContiguous(),
              "Terrain area presets must be one ascending run of ComponentPreset enumerators: a "
              "Terrain*Area enumerator was added without its kTerrainAreaPresets row.");

} // namespace

std::span<const TerrainAreaPreset> TerrainAreaPresets()
{
    return kTerrainAreaPresets;
}

const TerrainAreaPreset* FindTerrainAreaPreset(ComponentPreset p)
{
    for (const TerrainAreaPreset& entry : kTerrainAreaPresets)
        if (entry.PresetId == p)
            return &entry;
    return nullptr;
}

bool TerrainAreaPresetsCoverCanonicalEffects()
{
    const std::span<const TerrainECS::TerrainEffectTypeInfo> effects = TerrainECS::TerrainEffectTypes();
    if (effects.size() != std::size(kTerrainAreaPresets))
        return false;

    for (const TerrainECS::TerrainEffectTypeInfo& effect : effects)
    {
        const bool covered = std::any_of(std::begin(kTerrainAreaPresets), std::end(kTerrainAreaPresets),
                                         [&effect](const TerrainAreaPreset& area)
                                         { return area.EffectTypeId == effect.TypeId; });
        if (!covered)
            return false;
    }
    return true;
}

TerrainAreaPresetAdditions ApplyTerrainAreaPreset(ECS::World& world, ECS::EntityHandle entity,
                                                  const TerrainAreaPreset& area)
{
    TerrainAreaPresetAdditions added;

    // The region belongs to the volume, which is why no terrain preset asks the
    // user for a shape — the table row carries the one case where the EFFECT
    // constrains it.
    if (area.RequiresSplineRoute && !world.HasComponent<Components::SplineComponent>(entity))
    {
        world.AddComponentImmediate<Components::SplineComponent>(entity,
                                                                 Components::SplineComponent{});
        added.AddedSpline = true;
    }

    if (!world.HasComponent<Components::TerrainModifierVolume>(entity))
    {
        Components::TerrainModifierVolume volume{};
        if (area.RequiresSplineRoute)
            volume.Shape = Components::TerrainVolumeShape::SplinePath;
        world.AddComponentImmediate<Components::TerrainModifierVolume>(entity, volume);
        added.AddedVolume = true;
    }

    const int32 stackOrder = TerrainECS::NextEffectStackOrder(world, entity);
    if (TerrainECS::AddTerrainEffectDefault(world, entity, area.EffectTypeId, stackOrder))
        added.AddedEffectTypeId = area.EffectTypeId;

    return added;
}

const char* PresetUndoDisplayName(ComponentPreset p)
{
    if (const TerrainAreaPreset* area = FindTerrainAreaPreset(p))
        return area->Label;

    switch (p)
    {
    case Preset::Camera:
        return "Camera";
    case Preset::Light:
        return "Light";
    case Preset::SkyEnvironment:
        return "Sky Environment";
    case Preset::Skybox:
        return "Skybox";
    case Preset::PhysicsDynamicBox:
        return "Physics: Dynamic Box";
    case Preset::PhysicsDynamicSphere:
        return "Physics: Dynamic Sphere";
    case Preset::PhysicsStaticPlane:
        return "Physics: Static Plane";
    case Preset::PhysicsStaticBox:
        return "Physics: Static Box";
    case Preset::PhysicsCharacterController:
        return "Physics: Character Controller";
    case Preset::AudioEmitter:
        return "Audio: Emitter";
    case Preset::AudioListener:
        return "Audio: Listener";
    case Preset::PostProcessVolume:
        return "Post Process Volume";
    case Preset::WindVolume:
        return "Wind Volume";
    case Preset::TerrainSplineFlatten:
        return "Terrain: Spline Flatten";
    case Preset::SplineComponent:
        return "Spline";
    case Preset::ValueCurve:
        return "Value Curve";
    case Preset::VideoTexture:
        return "Video Texture";
    case Preset::ParticleEmitter2D:
        return "Particle Emitter 2D";
    case Preset::ParticleEmitter3D:
        return "Particle Emitter 3D";
    case Preset::ParticleCollisionEvents:
        return "Particle Collision Events";
    }
    return "Component";
}

std::vector<ECS::ComponentTypeId> ComponentTypeIdsForPreset(ComponentPreset p)
{
    if (const TerrainAreaPreset* area = FindTerrainAreaPreset(p))
    {
        std::vector<ECS::ComponentTypeId> ids;
        if (area->RequiresSplineRoute)
            ids.push_back(ECS::GetComponentTypeId<Components::SplineComponent>());
        ids.push_back(ECS::GetComponentTypeId<Components::TerrainModifierVolume>());
        ids.push_back(area->EffectTypeId);
        return ids;
    }

    switch (p)
    {
    case Preset::Camera:
        return {ECS::GetComponentTypeId<Components::Camera>()};
    case Preset::Light:
        return {ECS::GetComponentTypeId<Components::Light>()};
    case Preset::SkyEnvironment:
        return {ECS::GetComponentTypeId<Components::SkyEnvironment>()};
    case Preset::Skybox:
        return {ECS::GetComponentTypeId<Components::Skybox>()};
    case Preset::PhysicsDynamicBox:
    case Preset::PhysicsStaticBox:
        return {ECS::GetComponentTypeId<Components::PhysicsBody>(),
                ECS::GetComponentTypeId<Components::PhysicsCollider>(),
                ECS::GetComponentTypeId<Components::BoxColliderShape>()};
    case Preset::PhysicsDynamicSphere:
        return {ECS::GetComponentTypeId<Components::PhysicsBody>(),
                ECS::GetComponentTypeId<Components::PhysicsCollider>(),
                ECS::GetComponentTypeId<Components::SphereColliderShape>()};
    case Preset::PhysicsStaticPlane:
        return {ECS::GetComponentTypeId<Components::PhysicsBody>(),
                ECS::GetComponentTypeId<Components::PhysicsCollider>(),
                ECS::GetComponentTypeId<Components::PlaneColliderShape>()};
    case Preset::PhysicsCharacterController:
        return {ECS::GetComponentTypeId<Components::CharacterController>()};
    case Preset::AudioEmitter:
        return {ECS::GetComponentTypeId<Components::AudioEmitter>()};
    case Preset::AudioListener:
        return {ECS::GetComponentTypeId<Components::AudioListener>()};
    case Preset::PostProcessVolume:
        return {ECS::GetComponentTypeId<Components::PostProcessVolume>()};
    case Preset::WindVolume:
        return {ECS::GetComponentTypeId<Components::WindVolume>()};
    case Preset::ParticleEmitter2D:
    case Preset::ParticleEmitter3D:
        return {ECS::GetComponentTypeId<Components::ParticleEmitter3D>(),
                ECS::GetComponentTypeId<Components::ParticleCollisionEventsBuffer>()};
    case Preset::ParticleCollisionEvents:
        return {ECS::GetComponentTypeId<Components::ParticleCollisionEventsBuffer>()};
    case Preset::TerrainSplineFlatten:
        return {ECS::GetComponentTypeId<Components::SplineComponent>(),
                ECS::GetComponentTypeId<Components::TerrainModifierVolume>(),
                ECS::GetComponentTypeId<Components::TerrainFlattenEffect>()};
    case Preset::SplineComponent:
        return {ECS::GetComponentTypeId<Components::SplineComponent>()};
    case Preset::ValueCurve:
        return {ECS::GetComponentTypeId<Components::ValueCurve>()};
    case Preset::VideoTexture:
        return {ECS::GetComponentTypeId<Components::VideoTextureComponent>()};
    }
    return {};
}

namespace
{

// Half extent of the Static Plane preset's ground collider, in metres.
constexpr float kStaticPlaneHalfExtent = 50.0f;

// Adds `value` unless the entity already carries a T, recording the add.
template <typename T>
void AddIfMissing(ECS::World& world, ECS::EntityHandle entity, const T& value,
                  std::vector<ECS::ComponentTypeId>& added)
{
    if (world.HasComponent<T>(entity))
        return;
    world.AddComponentImmediate<T>(entity, value);
    added.push_back(ECS::GetComponentTypeId<T>());
}

// A body, its collider and `shape`: the physics presets' common set.
template <typename TShape>
void AddPhysicsBodyWithShape(ECS::World& world, ECS::EntityHandle entity, Physics::MotionType motionType,
                             const TShape& shape, std::vector<ECS::ComponentTypeId>& added)
{
    Components::PhysicsBody body{};
    body.motionType = motionType;
    AddIfMissing(world, entity, body, added);
    AddIfMissing(world, entity, Components::PhysicsCollider{}, added);
    AddIfMissing(world, entity, shape, added);
}

Components::PlaneColliderShape GroundPlaneShape()
{
    Components::PlaneColliderShape plane{};
    plane.normalX = 0.0f;
    plane.normalY = 1.0f;
    plane.normalZ = 0.0f;
    plane.d = 0.0f;
    plane.halfExtent = kStaticPlaneHalfExtent;
    return plane;
}

// A directional light at clear-noon sun illuminance, in physical units.
Components::Light SunLight()
{
    Components::Light light{};
    light.Type = Components::LightType::Directional;
    light.Intensity = Components::kClearNoonSunIlluminanceLux;
    light.IntensityUnit = Components::LightUnit::Lux;
    return light;
}

// The spline supplies the region, so the volume takes its shape from it.
// Adding the SplineComponent is the point: a spline flatten without one would
// do nothing.
void ApplySplineFlatten(ECS::World& world, ECS::EntityHandle entity, std::vector<ECS::ComponentTypeId>& added)
{
    AddIfMissing(world, entity, Components::SplineComponent{}, added);
    Components::TerrainModifierVolume volume{};
    volume.Shape = Components::TerrainVolumeShape::SplineArea;
    AddIfMissing(world, entity, volume, added);

    const ECS::ComponentTypeId flattenTypeId = ECS::GetComponentTypeId<Components::TerrainFlattenEffect>();
    const int32 stackOrder = TerrainECS::NextEffectStackOrder(world, entity);
    if (TerrainECS::AddTerrainEffectDefault(world, entity, flattenTypeId, stackOrder))
        added.push_back(flattenTypeId);
}

} // namespace

std::vector<ECS::ComponentTypeId> ApplyComponentPreset(ECS::World& world, ECS::EntityHandle entity,
                                                       ComponentPreset p)
{
    std::vector<ECS::ComponentTypeId> added;

    // Terrain area presets are answered by the table that pairs a preset with
    // its effect, never by a case label.
    if (const TerrainAreaPreset* area = FindTerrainAreaPreset(p))
    {
        const TerrainAreaPresetAdditions additions = ApplyTerrainAreaPreset(world, entity, *area);
        if (additions.AddedSpline)
            added.push_back(ECS::GetComponentTypeId<Components::SplineComponent>());
        if (additions.AddedVolume)
            added.push_back(ECS::GetComponentTypeId<Components::TerrainModifierVolume>());
        if (additions.AddedEffectTypeId != 0)
            added.push_back(additions.AddedEffectTypeId);
        return added;
    }

    switch (p)
    {
    case Preset::Camera:
        AddIfMissing(world, entity, Components::Camera{}, added);
        break;
    case Preset::Light:
        AddIfMissing(world, entity, SunLight(), added);
        break;
    case Preset::SkyEnvironment:
        AddIfMissing(world, entity, Components::SkyEnvironment{}, added);
        break;
    case Preset::Skybox:
        AddIfMissing(world, entity, Components::Skybox{}, added);
        break;
    case Preset::PhysicsDynamicBox:
        AddPhysicsBodyWithShape(world, entity, Physics::MotionType::Dynamic, Components::BoxColliderShape{}, added);
        break;
    case Preset::PhysicsDynamicSphere:
        AddPhysicsBodyWithShape(world, entity, Physics::MotionType::Dynamic, Components::SphereColliderShape{},
                                added);
        break;
    case Preset::PhysicsStaticPlane:
        AddPhysicsBodyWithShape(world, entity, Physics::MotionType::Static, GroundPlaneShape(), added);
        break;
    case Preset::PhysicsStaticBox:
        AddPhysicsBodyWithShape(world, entity, Physics::MotionType::Static, Components::BoxColliderShape{}, added);
        break;
    case Preset::PhysicsCharacterController:
        AddIfMissing(world, entity, Components::CharacterController{}, added);
        break;
    case Preset::AudioEmitter:
        AddIfMissing(world, entity, Components::AudioEmitter{}, added);
        break;
    case Preset::AudioListener:
        AddIfMissing(world, entity, Components::AudioListener{}, added);
        break;
    case Preset::PostProcessVolume:
        AddIfMissing(world, entity, Components::PostProcessVolume{}, added);
        break;
    case Preset::WindVolume:
        AddIfMissing(world, entity, Components::WindVolume{}, added);
        break;
    case Preset::ParticleEmitter2D:
        AddIfMissing(world, entity, Components::MakeParticleEmitter2DDefaults(), added);
        AddIfMissing(world, entity, Components::ParticleCollisionEventsBuffer{}, added);
        break;
    case Preset::ParticleEmitter3D:
        AddIfMissing(world, entity, Components::ParticleEmitter3D{}, added);
        AddIfMissing(world, entity, Components::ParticleCollisionEventsBuffer{}, added);
        break;
    case Preset::ParticleCollisionEvents:
        AddIfMissing(world, entity, Components::ParticleCollisionEventsBuffer{}, added);
        break;
    case Preset::TerrainSplineFlatten:
        ApplySplineFlatten(world, entity, added);
        break;
    case Preset::SplineComponent:
        AddIfMissing(world, entity, Components::SplineComponent{}, added);
        break;
    case Preset::ValueCurve:
        AddIfMissing(world, entity, Components::ValueCurve{}, added);
        break;
    case Preset::VideoTexture:
        AddIfMissing(world, entity, Components::VideoTextureComponent{}, added);
        break;
    }
    return added;
}

std::vector<ECS::ComponentTypeId> ComponentTypeIdsOfferedOnlyByPresets()
{
    std::vector<ECS::ComponentTypeId> covered;
    for (const ComponentPresetEntry& entry : ComponentPresetCatalog())
    {
        for (ECS::ComponentTypeId typeId : ComponentTypeIdsForPreset(entry.Preset))
        {
            if (std::find(covered.begin(), covered.end(), typeId) == covered.end())
                covered.push_back(typeId);
        }
    }

    // The modifier volume is the exception: every terrain preset adds one, but
    // none of them IS one. An empty volume authored on its own — region first,
    // effects from its Add Effect picker after — is a real flow, so it keeps
    // its standalone entry.
    const ECS::ComponentTypeId volumeTypeId = ECS::GetComponentTypeId<Components::TerrainModifierVolume>();
    covered.erase(std::remove(covered.begin(), covered.end(), volumeTypeId), covered.end());
    return covered;
}

const std::vector<ComponentPresetEntry>& ComponentPresetCatalog()
{
    static const std::vector<ComponentPresetEntry> kCatalog = []
    {
        using P = ComponentPreset;
        std::vector<ComponentPresetEntry> entries;
        auto Add = [&](const char* label, const char* category, P preset, const char* icon)
        {
            entries.push_back({std::string(label), std::string(category), preset, std::string(icon)});
        };
        Add("Camera", "Rendering", P::Camera, EditorIcons::kVideoCam);
        Add("Light", "Rendering", P::Light, EditorIcons::kLight);
        Add("Sky Environment", "Rendering", P::SkyEnvironment, EditorIcons::kSkyEnvironment);
        Add("Skybox", "Rendering", P::Skybox, EditorIcons::kSkyEnvironment);
        Add("Dynamic Box", "Physics", P::PhysicsDynamicBox, EditorIcons::kCube);
        Add("Dynamic Sphere", "Physics", P::PhysicsDynamicSphere, EditorIcons::kSphere);
        Add("Static Plane", "Physics", P::PhysicsStaticPlane, EditorIcons::kPlane);
        Add("Static Box", "Physics", P::PhysicsStaticBox, EditorIcons::kCube);
        Add("Character Controller", "Physics", P::PhysicsCharacterController, EditorIcons::kCube);
        Add("Emitter", "Audio", P::AudioEmitter, EditorIcons::kMusicNote);
        Add("Listener", "Audio", P::AudioListener, EditorIcons::kMusicNote);
        Add("Post Process Volume", "Rendering", P::PostProcessVolume, EditorIcons::kPostFx);
        Add("Wind Volume", "Rendering", P::WindVolume, EditorIcons::kWind);
        // Terrain area presets come from the one table that pairs a preset with
        // its effect, so the picker cannot list a different set than the command
        // creates. Each adds a volume + that effect: the region belongs to the
        // volume, which is why no terrain preset asks for a shape.
        for (const TerrainAreaPreset& area : kTerrainAreaPresets)
            Add(area.Label, "Terrain", area.PresetId, EditorIcons::kTerrain);
        Add("Terrain: Spline Flatten", "Terrain", P::TerrainSplineFlatten, EditorIcons::kTerrain);

        // Reordering or replacing an entry in ModifierEffectComponents without
        // updating kTerrainAreaPresets leaves an effect with no way to be
        // created; the count static_assert cannot see that, so check identity.
        assert(TerrainAreaPresetsCoverCanonicalEffects()
               && "terrain effect has no Add Component preset");

        Add("Spline", "Spline", P::SplineComponent, EditorIcons::kSplineCurve);
        Add("Value Curve", "Animation", P::ValueCurve, EditorIcons::kDrawCurve);
        Add("Video Texture", "Rendering", P::VideoTexture, EditorIcons::kFilm);
        // Post-process effects are absent by design: they add only through the
        // Volume inspector's Add Post FX picker (doc §4 — Volume-owned effects).
        Add("Particle Emitter 2D", "Rendering", P::ParticleEmitter2D, EditorIcons::kSparkles);
        Add("Particle Emitter 3D", "Rendering", P::ParticleEmitter3D, EditorIcons::kSparkles);
        Add("Particle Collision Events", "Rendering", P::ParticleCollisionEvents, EditorIcons::kSparkles);

        std::sort(entries.begin(), entries.end(),
                  [](const ComponentPresetEntry& a, const ComponentPresetEntry& b)
                  {
                      if (a.Category != b.Category)
                          return a.Category < b.Category;
                      return a.Label < b.Label;
                  });
        return entries;
    }();
    return kCatalog;
}

} // namespace GameEngine::Editor
