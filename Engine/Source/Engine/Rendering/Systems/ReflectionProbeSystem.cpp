#include "ECSModules/Rendering/Systems/ReflectionProbeSystem.h"

#include "Components/Rendering/Light.h"
#include "Components/Rendering/LocalBounds.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Components/Rendering/ReflectionProbe.h"
#include "Components/Transform.h"
#include "ECS/Components.h"
#include "ECS/Query.h"
#include "Engine/Rendering/ImageBasedLightingFeature.h"
#include "Engine/Rendering/Material.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/SceneReflectionProbeEnvironmentSource.h"

#include "Mathematics/VectorOps.h"
#include "Types/Fnv1a.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace GameEngine { namespace Engine::Renderer {

namespace
{
using Mathematics::Dot3;

constexpr float kReflectionProbeDegToRad = 0.01745329251994329577f;
constexpr float kMinProbeExtent = 0.0001f;
constexpr float kMinProjectionScale = 0.001f;

SceneReflectionProbeUpdateMode ToSceneUpdateMode(Components::ReflectionProbeUpdateMode mode)
{
    switch (mode)
    {
    case Components::ReflectionProbeUpdateMode::Once:
        return SceneReflectionProbeUpdateMode::Once;
    case Components::ReflectionProbeUpdateMode::Realtime:
    default:
        return SceneReflectionProbeUpdateMode::Realtime;
    }
}

struct ProbeVolume
{
    float Center[3] = {0.0f, 0.0f, 0.0f};
    float AxisX[3] = {1.0f, 0.0f, 0.0f};
    float AxisY[3] = {0.0f, 1.0f, 0.0f};
    float AxisZ[3] = {0.0f, 0.0f, 1.0f};
    float HalfExtents[3] = {0.0f, 0.0f, 0.0f};
    bool Valid = false;
};

// A mesh renderer the digest pass reads, copied once per update and sorted by
// entity id so the digest does not depend on query order.
struct ProbeSceneObject
{
    ECS::EntityHandle Entity;
    Components::WorldTransform Transform;
    Components::MeshRenderer Mesh;
};

ProbeVolume ExtractProbeVolume(const Components::WorldTransform& xf)
{
    ProbeVolume volume{};
    const float* m = xf.matrix;
    volume.Center[0] = m[12];
    volume.Center[1] = m[13];
    volume.Center[2] = m[14];

    const float ax[3] = {m[0], m[1], m[2]};
    const float ay[3] = {m[4], m[5], m[6]};
    const float az[3] = {m[8], m[9], m[10]};
    const float sx = std::sqrt(Dot3(ax, ax));
    const float sy = std::sqrt(Dot3(ay, ay));
    const float sz = std::sqrt(Dot3(az, az));
    if (sx <= kMinProbeExtent || sy <= kMinProbeExtent || sz <= kMinProbeExtent)
        return volume;

    const float invSx = 1.0f / sx;
    const float invSy = 1.0f / sy;
    const float invSz = 1.0f / sz;
    for (int i = 0; i < 3; ++i)
    {
        volume.AxisX[i] = ax[i] * invSx;
        volume.AxisY[i] = ay[i] * invSy;
        volume.AxisZ[i] = az[i] * invSz;
    }
    volume.HalfExtents[0] = sx * 0.5f;
    volume.HalfExtents[1] = sy * 0.5f;
    volume.HalfExtents[2] = sz * 0.5f;
    volume.Valid = true;
    return volume;
}

void ProjectToVolumeLocal(const ProbeVolume& volume, const float worldPos[3], float outLocal[3])
{
    const float d[3] = {
        worldPos[0] - volume.Center[0],
        worldPos[1] - volume.Center[1],
        worldPos[2] - volume.Center[2]};
    outLocal[0] = Dot3(d, volume.AxisX);
    outLocal[1] = Dot3(d, volume.AxisY);
    outLocal[2] = Dot3(d, volume.AxisZ);
}

float ComputeBoxCaptureRadius(const ProbeVolume& volume, const float capturePosition[3])
{
    float captureLocal[3] = {0.0f, 0.0f, 0.0f};
    ProjectToVolumeLocal(volume, capturePosition, captureLocal);
    const float dx = volume.HalfExtents[0] + std::fabs(captureLocal[0]);
    const float dy = volume.HalfExtents[1] + std::fabs(captureLocal[1]);
    const float dz = volume.HalfExtents[2] + std::fabs(captureLocal[2]);
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

SceneReflectionProbeEnvironmentDesc BuildProbeDesc(ECS::World& world, ECS::EntityHandle entity,
                                                   const Components::ReflectionProbe& probe,
                                                   const ProbeVolume& volume)
{
    SceneReflectionProbeEnvironmentDesc desc{};
    const float projectionScale[3] = {probe.ProjectionScaleX, probe.ProjectionScaleY, probe.ProjectionScaleZ};
    for (int i = 0; i < 3; ++i)
    {
        desc.Position[i] = volume.Center[i] + volume.AxisX[i] * probe.OriginOffsetX +
                           volume.AxisY[i] * probe.OriginOffsetY + volume.AxisZ[i] * probe.OriginOffsetZ;
        desc.BoxCenter[i] = volume.Center[i];
        desc.BoxHalfExtents[i] = volume.HalfExtents[i];
        desc.BoxAxisX[i] = volume.AxisX[i];
        desc.BoxAxisY[i] = volume.AxisY[i];
        desc.BoxAxisZ[i] = volume.AxisZ[i];
        desc.ProjectionCenter[i] = volume.Center[i] + volume.AxisX[i] * probe.ProjectionOffsetX +
                                   volume.AxisY[i] * probe.ProjectionOffsetY +
                                   volume.AxisZ[i] * probe.ProjectionOffsetZ;
        desc.ProjectionHalfExtents[i] = volume.HalfExtents[i] * std::max(kMinProjectionScale, projectionScale[i]);
    }

    const bool isBox = probe.InfluenceShape == Components::ReflectionProbeInfluenceShape::Box;
    desc.InfluenceShape = static_cast<uint32_t>(probe.InfluenceShape);
    desc.InfluenceRadius = ComputeBoxCaptureRadius(volume, desc.Position);
    if (isBox)
    {
        ProbeVolume projection = volume;
        for (int i = 0; i < 3; ++i)
        {
            projection.Center[i] = desc.ProjectionCenter[i];
            projection.HalfExtents[i] = desc.ProjectionHalfExtents[i];
        }
        desc.InfluenceRadius = std::max(desc.InfluenceRadius, ComputeBoxCaptureRadius(projection, desc.Position));
    }
    desc.BoxProjection = isBox;
    desc.BlendDistance = std::max(0.0f, probe.BlendDistance);
    desc.Priority = probe.Priority;
    desc.MaxDistance = std::max(0.0f, probe.MaxDistance);
    desc.CullMask = probe.CullMask;
    desc.WorldId = world.GetWorldId();
    desc.SourceKey = (static_cast<uint64_t>(world.GetWorldId()) << 32u) ^ static_cast<uint64_t>(entity.id);
    if (desc.SourceKey == 0)
        desc.SourceKey = 1;
    desc.Intensity = std::max(0.0f, probe.Intensity);
    desc.ExposureEV = probe.ExposureEV;
    desc.RotationRadians = probe.RotationDegrees * kReflectionProbeDegToRad;
    desc.LowerHemisphereDarkness = std::clamp(probe.IblLowerHemisphereDarkness, 0.0f, 1.0f);
    desc.CaptureEnvironment = probe.CaptureEnvironment;
    desc.CaptureResolution = ImageBasedLightingFeature::NormalizeCaptureResolution(probe.CaptureResolution);
    desc.UpdateMode = ToSceneUpdateMode(probe.UpdateMode);
    desc.RealtimeUpdateInterval = std::max(0.0f, probe.RealtimeUpdateInterval);
    return desc;
}

// True when the object's world bounds reach within farDistance of the capture
// position; folds those bounds into the hash. An object without LocalBounds
// is included conservatively.
bool ObjectWithinCaptureReach(ECS::World& world, const ProbeSceneObject& object,
                              const float position[3], float farDistance, uint64_t& hash)
{
    const auto* bounds = world.GetComponent<Components::LocalBounds>(object.Entity);
    if (!bounds)
        return true;
    const auto aabb = bounds->Box.TransformToAABB(object.Transform.matrix);
    const float nearest[3] = {
        std::clamp(position[0], aabb.min.x, aabb.max.x),
        std::clamp(position[1], aabb.min.y, aabb.max.y),
        std::clamp(position[2], aabb.min.z, aabb.max.z)};
    float distanceSq = 0.0f;
    for (int i = 0; i < 3; ++i)
    {
        const float d = position[i] - nearest[i];
        distanceSq += d * d;
    }
    if (distanceSq > farDistance * farDistance)
        return false;
    hash = Hashing::Fnv1a64Value(hash, bounds->Box.center);
    hash = Hashing::Fnv1a64Value(hash, bounds->Box.halfExtents);
    return true;
}

// Membership, transforms and material versions inside the capture's far
// sphere, plus every directional light (unbounded reach, included even in a
// scene without a SkyEnvironment). A move across the boundary changes
// membership in both affected probes. Fills desc.MaterialKeys and
// desc.SceneEditDigest.
void ComputeProbeSceneDigest(ECS::World& world, const RenderServices& services,
                             const std::vector<ProbeSceneObject>& objects,
                             SceneReflectionProbeEnvironmentDesc& desc)
{
    uint64_t hash = Hashing::kFnv1a64OffsetBasis;
    const float farDistance = std::max(1.0f, desc.MaxDistance > 0.0f ? desc.MaxDistance : desc.InfluenceRadius);
    for (const ProbeSceneObject& object : objects)
    {
        if ((object.Mesh.renderLayerMask & desc.CullMask) == 0)
            continue;
        if (!ObjectWithinCaptureReach(world, object, desc.Position, farDistance, hash))
            continue;
        const GUID materialKey = object.Mesh.materialAssetGuid.ToGuid();
        if (!materialKey.IsNull() &&
            std::find(desc.MaterialKeys.begin(), desc.MaterialKeys.end(), materialKey) == desc.MaterialKeys.end())
            desc.MaterialKeys.push_back(materialKey);
        hash = Hashing::Fnv1a64Value(hash, object.Entity.id);
        hash = Hashing::Fnv1a64Value(hash, object.Transform.matrix);
        hash = Hashing::Fnv1a64Value(hash, object.Mesh.modelAssetGuid);
        hash = Hashing::Fnv1a64Value(hash, object.Mesh.meshId);
        hash = Hashing::Fnv1a64Value(hash, object.Mesh.MeshNameId);
        hash = Hashing::Fnv1a64Value(hash, object.Mesh.meshGpuHandleId);
        hash = Hashing::Fnv1a64Value(hash, object.Mesh.materialAssetGuid);
        if (const Material* material = services.Materials().Registry().Find(materialKey))
        {
            hash = Hashing::Fnv1a64Value(hash, material->GetVersion());
            hash = Hashing::Fnv1a64Value(hash, material->GetContentRevision());
        }
    }
    world.Query<ECS::Read<Components::Light>, ECS::Read<Components::WorldTransform>>()
        .Without<ECS::Disabled>()
        .Each([&](ECS::EntityHandle lightEntity, const Components::Light& light,
                  const Components::WorldTransform& transform) {
            if (!light.CastsLight || light.Type != Components::LightType::Directional)
                return;
            hash = Hashing::Fnv1a64Value(hash, lightEntity.id);
            hash = Hashing::Fnv1a64Value(hash, transform.matrix);
            hash = Hashing::Fnv1a64Value(hash, light.Color);
            hash = Hashing::Fnv1a64Value(hash, light.Intensity);
            hash = Hashing::Fnv1a64Value(hash, light.IntensityUnit);
            hash = Hashing::Fnv1a64Value(hash, light.UseColorTemperature);
            hash = Hashing::Fnv1a64Value(hash, light.ColorTemperature);
        });
    desc.SceneEditDigest = hash != 0 ? hash : 1;
}

bool EntityIdLess(const ProbeSceneObject& a, const ProbeSceneObject& b)
{
    return a.Entity.id < b.Entity.id;
}

// True when any C column was write-granted after the gate; advances the gate
// to the caller's entry sample.
template <typename C>
bool ColumnWrittenSince(ECS::World& world, ECS::ChangeGate& gate, uint64_t entryVersion)
{
    auto q = world.Query<ECS::Read<C>>();
    q.template Changed<C>(gate);
    bool written = false;
    q.BatchEach([&](const C*, std::size_t) { written = true; });
    gate.LastRunVersion = entryVersion;
    return written;
}

// Versions of every material a probe's capture reach holds. Material edits are
// not ECS writes, so the idle fast path compares this instead.
uint64_t ComputeProbeMaterialDigest(const RenderServices& services,
                                    const std::vector<SceneReflectionProbeEnvironmentDesc>& probes)
{
    uint64_t hash = Hashing::kFnv1a64OffsetBasis;
    for (const SceneReflectionProbeEnvironmentDesc& desc : probes)
        for (const GUID& key : desc.MaterialKeys)
        {
            hash = Hashing::Fnv1a64Value(hash, key);
            if (const Material* material = services.Materials().Registry().Find(key))
            {
                hash = Hashing::Fnv1a64Value(hash, material->GetVersion());
                hash = Hashing::Fnv1a64Value(hash, material->GetContentRevision());
            }
        }
    return hash;
}
} // namespace

bool ReflectionProbeSystem::SceneUnchangedSinceLastBuild(ECS::World& world)
{
    // Sample before probing, and write the sample back as every gate (ChangeGate contract).
    const uint64_t version = world.GetGlobalSystemVersion();
    bool anyChange = ColumnWrittenSince<Components::ReflectionProbe>(world, m_ProbeGate, version);
    anyChange |= ColumnWrittenSince<Components::MeshRenderer>(world, m_MeshGate, version);
    anyChange |= ColumnWrittenSince<Components::WorldTransform>(world, m_TransformGate, version);
    anyChange |= ColumnWrittenSince<Components::LocalBounds>(world, m_BoundsGate, version);
    anyChange |= ColumnWrittenSince<Components::Light>(world, m_LightGate, version);
    return m_HasBuild && !anyChange && ECS::ChangeFilter::Enabled() &&
           m_BuiltWorldId == world.GetWorldId() &&
           m_BuiltStructuralVersion == world.GetStructuralChangeVersion() &&
           m_BuiltMaterialDigest == ComputeProbeMaterialDigest(*m_RenderServices, m_Probes);
}

// Every valid probe in the world goes to the IBL feature, which keeps up to
// ImageBasedLightingFeature::kMaxLocalProbes captures per world, chosen by
// priority and entity key. Fragments weight their probes by position; the
// camera position plays no part. The sky stays the global source. A world
// without probes skips the scene walk; an idle frame re-sends the last
// descriptors without rescanning meshes or rehashing materials.
void ReflectionProbeSystem::Update(ECS::World& world, float32 /*deltaTime*/)
{
    RenderServices* rs = m_RenderServices;
    if (!rs)
        return;

    auto q = world.Query<
        ECS::Read<Components::ReflectionProbe>,
        ECS::Read<Components::WorldTransform>>();
    if (q.Count() == 0)
    {
        m_Probes.clear();
        m_HasBuild = false;
    }
    else if (!SceneUnchangedSinceLastBuild(world))
    {
        std::vector<ProbeSceneObject> objects;
        world.Query<ECS::Read<Components::MeshRenderer>, ECS::Read<Components::WorldTransform>>()
            .Each([&](ECS::EntityHandle e, const Components::MeshRenderer& mesh, const Components::WorldTransform& xf) {
                objects.push_back({e, xf, mesh});
            });
        std::sort(objects.begin(), objects.end(), EntityIdLess);

        m_Probes.clear();
        q.Each([&](ECS::EntityHandle e,
                   const Components::ReflectionProbe& probe,
                   const Components::WorldTransform& xf) {
            const ProbeVolume volume = ExtractProbeVolume(xf);
            if (!volume.Valid)
                return;
            SceneReflectionProbeEnvironmentDesc desc = BuildProbeDesc(world, e, probe, volume);
            ComputeProbeSceneDigest(world, *rs, objects, desc);
            m_Probes.push_back(std::move(desc));
        });
        m_BuiltWorldId = world.GetWorldId();
        m_BuiltStructuralVersion = world.GetStructuralChangeVersion();
        m_BuiltMaterialDigest = ComputeProbeMaterialDigest(*rs, m_Probes);
        m_HasBuild = true;
    }

    // Registered even before a device exists: capture allocation happens in the
    // IBL generation node, and an empty list clears only this world's probes.
    auto& feature = rs->EnsureFeature<ImageBasedLightingFeature>();
    feature.UpdateLocalProbes(*rs, world.GetWorldId(), m_Probes);
}

} } // namespace GameEngine::Engine::Renderer
