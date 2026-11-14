#include "PhysicsECS/ContactSurfaceTable.h"

#include "AssetCore/AssetReloadInvalidator.h"
#include "Core/Engine.h"
#include "Logger/Logger.h"
#include "PhysicsECS/PhysicsMaterialAsset.h"
#include "Physics/PhysicsWorld.h"

#include <filesystem>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

namespace GameEngine
{
namespace PhysicsECS
{
namespace
{

// A one-way surface counts as "top" while its outward normal is within ~45
// degrees of world up. Wider than that and a body sliding down a steep wall of
// one-way tiles would be caught by it.
constexpr float32 kTopFaceMinNormalY = 0.7f;

std::vector<Physics::PhysicsMaterial>& Materials()
{
    // Slot 0 is kNoMaterial and is never handed out by Resolve/Add.
    static std::vector<Physics::PhysicsMaterial> materials(1);
    return materials;
}

// Where a resolved material lives and where it came from. The path is kept so a
// reload can re-read the file without the asset registry, which a headless host
// (tests, tools) does not have.
struct MaterialEntry
{
    uint32 Index = ContactSurfaceTable::kNoMaterial;
    std::filesystem::path Path;
};

std::unordered_map<GUID, MaterialEntry>& EntryByGuid()
{
    static std::unordered_map<GUID, MaterialEntry> map;
    return map;
}

std::mutex& PendingReloadsMutex()
{
    static std::mutex mutex;
    return mutex;
}

// Written from the file-watcher thread, drained on the main thread.
std::vector<GUID>& PendingReloads()
{
    static std::vector<GUID> pending;
    return pending;
}

AssetReloadInvalidator& ReloadInvalidator()
{
    static AssetReloadInvalidator invalidator;
    return invalidator;
}

// The .physmat at `path`, or nothing if it cannot be read or decoded.
std::optional<Physics::PhysicsMaterial> LoadMaterialFile(const GUID& guid,
                                                         const std::filesystem::path& path)
{
    PhysicsMaterialAsset asset(guid, path);
    if (!asset.Load())
    {
        Logger::Log::Warning("ContactSurfaceTable: failed to load physics material {}",
                             guid.ToString());
        return std::nullopt;
    }
    return asset.Material();
}

} // namespace

uint32 ContactSurfaceTable::AddMaterial(const Physics::PhysicsMaterial& material)
{
    auto& materials = Materials();
    materials.push_back(material);
    return static_cast<uint32>(materials.size() - 1);
}

uint32 ContactSurfaceTable::ResolveMaterial(const GUID& guid)
{
    if (guid.IsNull())
        return kNoMaterial;

    auto& byGuid = EntryByGuid();
    if (auto it = byGuid.find(guid); it != byGuid.end())
        return it->second.Index;

    // Same retry policy as the tilemap store: registry resolution is transient,
    // so a miss is cached as nothing and simply re-tried on the next bake.
    auto& engine = EngineCore::GetInstance();
    AssetMetadata meta{};
    if (!engine.IsInitialized() ||
        !engine.GetAssetManager().GetRegistry().TryGetAssetMetadata(guid, meta) ||
        meta.Path.empty())
    {
        return kNoMaterial;
    }

    return ResolveMaterialFromFile(guid, meta.Path);
}

uint32 ContactSurfaceTable::ResolveMaterialFromFile(const GUID& guid,
                                                    const std::filesystem::path& path)
{
    if (guid.IsNull())
        return kNoMaterial;

    auto& byGuid = EntryByGuid();
    if (auto it = byGuid.find(guid); it != byGuid.end())
        return it->second.Index;

    const std::optional<Physics::PhysicsMaterial> material = LoadMaterialFile(guid, path);
    if (!material)
        return kNoMaterial;

    const uint32 index = AddMaterial(*material);
    byGuid.emplace(guid, MaterialEntry{index, path});
    return index;
}

void ContactSurfaceTable::SubscribeToAssetChanges(AssetEventDispatcher& dispatcher)
{
    // ContentEvents: the table decodes .physmat files straight from disk, so a
    // plain edit raises AssetModified and never AssetReloaded. These arrive on
    // the watcher thread, so the handler only enqueues.
    ReloadInvalidator() = AssetReloadInvalidator(
        dispatcher, AssetType::PhysicsMaterial,
        [](const GUID& guid)
        {
            std::lock_guard lock(PendingReloadsMutex());
            PendingReloads().push_back(guid);
        },
        AssetReloadInvalidator::EventSet::ContentEvents);
}

void ContactSurfaceTable::UnsubscribeFromAssetChanges()
{
    ReloadInvalidator().Reset();
}

void ContactSurfaceTable::DrainPendingReloads()
{
    std::vector<GUID> pending;
    {
        std::lock_guard lock(PendingReloadsMutex());
        pending.swap(PendingReloads());
    }
    if (pending.empty())
        return;

    auto& byGuid = EntryByGuid();
    auto& materials = Materials();
    for (const GUID& guid : pending)
    {
        const auto it = byGuid.find(guid);
        if (it == byGuid.end())
            continue;

        // A move keeps the GUID but not the path, so the registry — when there
        // is one — is the authority on where the file is now.
        auto& engine = EngineCore::GetInstance();
        AssetMetadata meta{};
        if (engine.IsInitialized() &&
            engine.GetAssetManager().GetRegistry().TryGetAssetMetadata(guid, meta) &&
            !meta.Path.empty())
        {
            it->second.Path = meta.Path;
        }

        const std::optional<Physics::PhysicsMaterial> material =
            LoadMaterialFile(guid, it->second.Path);
        // A failed re-read keeps the values the bodies were baked with; a
        // half-written file mid-save must not zero a live surface.
        if (material && it->second.Index < materials.size())
            materials[it->second.Index] = *material;
    }
}

const Physics::PhysicsMaterial& ContactSurfaceTable::MaterialAt(uint32 index)
{
    const auto& materials = Materials();
    return index < materials.size() ? materials[index] : materials[kNoMaterial];
}

void ContactSurfaceTable::Reset()
{
    Materials().assign(1, Physics::PhysicsMaterial{});
    EntryByGuid().clear();
    std::lock_guard lock(PendingReloadsMutex());
    PendingReloads().clear();
}

void ContactSurfaceTable::Install(Physics::PhysicsWorld& world)
{
    // The table is file-static, so the callback needs no context of its own.
    world.SetContactModifyCallback(&ContactSurfaceTable::Decide, nullptr);
}

Physics::ContactModification ContactSurfaceTable::Decide(void* /*context*/,
                                                         const Physics::ContactBody& a,
                                                         const Physics::ContactBody& b,
                                                         const Physics::Vector3& normal,
                                                         const Physics::Vector3& relativeVelocity)
{
    Physics::ContactModification mod{};

    const bool aIsOneWay = (a.contactFlags & kContactFlagOneWay) != 0;
    const bool bIsOneWay = (b.contactFlags & kContactFlagOneWay) != 0;
    if (aIsOneWay != bIsOneWay)
    {
        // Outward normal of the one-way surface, and the other body's velocity
        // relative to it. `normal` points from a toward b, and
        // `relativeVelocity` is b's at the contact minus a's, so both flip
        // together when the one-way body is b.
        const float32 sign = aIsOneWay ? 1.0f : -1.0f;
        const Physics::Vector3 outward(normal.x * sign, normal.y * sign, normal.z * sign);
        const Physics::Vector3 otherVel(relativeVelocity.x * sign, relativeVelocity.y * sign,
                                        relativeVelocity.z * sign);
        const Physics::ContactBody& other = aIsOneWay ? b : a;

        if ((other.contactFlags & kContactFlagIgnoreOneWay) != 0)
        {
            mod.accept = false;
            return mod;
        }

        const float32 approach = otherVel.x * outward.x + otherVel.y * outward.y +
                                 otherVel.z * outward.z;
        mod.accept = outward.y > kTopFaceMinNormalY && approach <= 0.0f;
        if (!mod.accept)
            return mod;
    }

    // Two one-way surfaces meeting is not a case the flag describes, so it
    // falls through to plain solid — as does every unflagged contact.

    if (a.materialIndex == kNoMaterial && b.materialIndex == kNoMaterial)
    {
        mod.overrideSurface = false;
        return mod;
    }

    Physics::PhysicsMaterial materialA = MaterialAt(a.materialIndex);
    Physics::PhysicsMaterial materialB = MaterialAt(b.materialIndex);
    if (a.materialIndex == kNoMaterial)
    {
        materialA.friction = a.friction;
        materialA.restitution = a.restitution;
    }
    if (b.materialIndex == kNoMaterial)
    {
        materialB.friction = b.friction;
        materialB.restitution = b.restitution;
    }
    const Physics::ResolvedContactSurface surface = Physics::CombineMaterials(materialA, materialB);
    mod.friction = surface.friction;
    mod.restitution = surface.restitution;
    return mod;
}

} // namespace PhysicsECS
} // namespace GameEngine
