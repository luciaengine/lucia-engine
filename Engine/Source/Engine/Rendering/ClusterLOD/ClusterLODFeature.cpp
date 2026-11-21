#include "Engine/Rendering/ClusterLOD/ClusterLODFeature.h"
#include "Engine/Rendering/ClusterLOD/ClusterLODCut.h"

#include "ClusterLODSplits.h"

#include "Assets/AssetManager.h"
#include "Assets/AssetDecodeCancellation.h"
#include "Assets/ClusterLOD/MeshClusterLODAsset.h"
#include "Assets/ModelAsset.h"
#include "Components/Rendering/DDGIVolume.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Components/Rendering/LODGroup.h"
#include "Components/Rendering/MeshGPUData.h"
#include "Components/Rendering/MorphTargetWeights.h"
#include "Components/Rendering/SkinnedMeshRenderer.h"
#include "Components/Rendering/PlanarReflection.h"
#include "Components/Rendering/ReflectionProbe.h"
#include "Components/Transform.h"
#include "Core/DebugMetrics.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "Engine/Rendering/MeshGPURegistry.h"
#include "Engine/Rendering/Material.h"
#include "Engine/Rendering/MeshLODThresholds.h"
#include "Engine/Rendering/ModelRenderSetup.h"
#include "Engine/Rendering/RenderServices.h"
#include "Logger/Logger.h"
#include "Mathematics/Matrix4x4.h"
#include "Rendering/Core/GPUScene.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Engine/Rendering/RenderOrigin.h"
#include "Types/Types.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <limits>
#include <mutex>
#include <utility>

namespace GameEngine::Engine::Renderer
{
namespace
{
using namespace GameEngine::Rendering;
using Mathematics::Matrix4x4;
using Mathematics::Vector3;

constexpr uint64 kMaximumPageWorkBytes = 64ull << 20;
// One record per streamed unit; pages of one model unit are shared by every instance.
constexpr uint32 kMaximumPageRecords = 1024;
constexpr uint32 kMaximumPendingReads = 8;
constexpr uint32 kMaximumAdmissionsPerFrame = 8;
constexpr uint64 kPageWorkMultiplier = 8;
constexpr uint64 kUnusedFrameGrace = 30;
// A split registers the resident units of its first cut before it activates.
constexpr uint32 kMaximumUnitRegistrationsPerFrame = 512;
// A frame can read fine pages on every queue. Graphics is first: its token is required.
constexpr std::array<IDevice::QueueType, 3> kReaderQueues{
    IDevice::QueueType::Graphics, IDevice::QueueType::Compute, IDevice::QueueType::Transfer};

Mesh PageDescriptor(const Mesh& coarse)
{
    Mesh descriptor;
    descriptor.Name = coarse.Name;
    descriptor.MaterialIndex = coarse.MaterialIndex;
    descriptor.SourceNodeIndex = coarse.SourceNodeIndex;
    descriptor.SourceNodeParentIndex = coarse.SourceNodeParentIndex;
    std::copy_n(coarse.SourceNodeTransform, 16, descriptor.SourceNodeTransform);
    std::copy_n(coarse.SourceNodeLocalTransform, 16, descriptor.SourceNodeLocalTransform);
    std::copy_n(coarse.MinBounds, 3, descriptor.MinBounds);
    std::copy_n(coarse.MaxBounds, 3, descriptor.MaxBounds);
    return descriptor;
}

bool HasUnsupportedConsumers(ECS::World& world)
{
    return world.Query<ECS::Read<Components::DDGIVolume>, ECS::Read<Components::WorldTransform>>().Count() != 0 ||
        world.Query<ECS::Read<Components::ReflectionProbe>>().Count() != 0 ||
        world.Query<ECS::Read<Components::PlanarReflection>>().Count() != 0;
}

bool SupportsFineInstance(RenderServices& services, ECS::World& world, ECS::EntityHandle entity,
                          const Components::MeshRenderer& renderer)
{
    if (world.HasComponent<Components::SkinnedMeshRenderer>(entity) ||
        world.HasComponent<Components::MorphTargetWeights>(entity))
        return false;
    const auto* material = services.Materials().Registry().Find(renderer.materialAssetGuid.ToGuid());
    if (!material || material->GetAlphaMode() != MaterialAlphaMode::Opaque)
        return false;
    const auto& spec = material->GetCompileSpec();
    return (spec.surfaceShaderPath == "Surfaces/standard_pbr.glsl" ||
            spec.surfaceShaderPath == "Surfaces/standard_pbr_extended.glsl") &&
        spec.vertexModifierPath.empty() && !spec.customVertexShader && spec.userKeywords.empty() &&
        !spec.parallax && !HasKeyword(material->GetVariantKey().materialKeywords, MaterialKeyword::Transmission);
}

// Frobenius norm bounds the largest scale even for a sheared affine transform.
double ScaleBound(const Components::WorldTransform& transform)
{
    double squared = 0.0;
    for (uint32 column = 0; column < 3; ++column)
        for (uint32 row = 0; row < 3; ++row)
            squared += static_cast<double>(transform.Basis.Data()[column * 3 + row]) *
                       static_cast<double>(transform.Basis.Data()[column * 3 + row]);
    return std::sqrt(squared);
}

// The values of one view that every group's demand reads, collected once per preparation.
struct DemandView
{
    RenderLayerMask Layers;
    uint64 WorldId = 0;
    // Forced LOD 0, or selection off: every group refines.
    bool MustRefine = false;
    // Automatic selection with a viewport: the projected error decides.
    bool Measured = false;
    // No usable projection or budget: a group with an error refines.
    bool Unusable = false;
    double EyeX = 0.0;
    double EyeY = 0.0;
    double EyeZ = 0.0;
    double ProjectionScaleY = 0.0;
    uint32 Height = 0;
    bool Orthographic = false;
    double Budget = 0.0;
};

void CollectDemandViews(const RenderServices& services, std::vector<DemandView>& views)
{
    views.clear();
    const bool selectionOff = services.GetMeshGPURegistry().GetLodSelectionMode() == LodSelectionMode::Off;
    for (const auto& view : services.Views().GetViews())
    {
        DemandView sample;
        sample.Layers = view.ActiveRenderLayerMask();
        sample.WorldId = view.worldId;
        const uint32 forced = services.ResolveViewForceLOD(view.id);
        sample.MustRefine = forced == 0 || (forced == ~0u && selectionOff);
        sample.Height = services.Views().GetViewWorldViewportHeight(view.id);
        sample.Measured = !sample.MustRefine && forced == ~0u && sample.Height != 0;
        if (sample.Measured)
        {
            const auto camera = services.Views().ResolveCameraData(view.id);
            const auto eye = CameraEye(camera);
            sample.EyeX = eye.X;
            sample.EyeY = eye.Y;
            sample.EyeZ = eye.Z;
            sample.ProjectionScaleY = camera.proj[1][1];
            sample.Orthographic = std::abs(camera.proj[3][3]) > 0.5f;
            sample.Budget = ResolveLodBudgetPx(services.GetLODErrorBudgetPx(),
                                               services.GetLODViewBudgetOverride(view.purpose));
            sample.Unusable = !std::isfinite(sample.Budget) || sample.Budget <= 0.0 || sample.ProjectionScaleY == 0.0;
        }
        views.push_back(sample);
    }
}

// The largest projected error of one sphere over the views, divided by the switch threshold (the
// keep band for a group that is refined). Above 1 wants refinement. Infinity: a view must draw
// the finest units (forced LOD 0, selection off or an unusable projection).
double Demand(std::span<const DemandView> views, const Components::MeshRenderer& renderer,
              const Components::WorldTransform& transform, const Vector3& center, float radius, float error,
              uint64 worldId, bool wasRefined, float lodBias)
{
    constexpr double kMustRefine = std::numeric_limits<double>::infinity();
    double offset[3]{};
    const double localCenter[3]{center.x, center.y, center.z};
    for (uint32 row = 0; row < 3; ++row)
        for (uint32 column = 0; column < 3; ++column)
            offset[row] += static_cast<double>(transform.Basis.Data()[column * 3 + row]) * localCenter[column];
    const double scale = ScaleBound(transform);
    const double worldRadius = static_cast<double>(radius) * scale;
    if (!std::isfinite(scale) || !std::isfinite(worldRadius) ||
        !std::isfinite(offset[0]) || !std::isfinite(offset[1]) || !std::isfinite(offset[2]))
        return 0.0;
    double demand = 0.0;
    for (const DemandView& view : views)
    {
        if (!view.Layers.Intersects(renderer.RenderLayers) || (view.WorldId != 0 && view.WorldId != worldId))
            continue;
        if (view.MustRefine)
            return kMustRefine;
        if (!view.Measured || error <= 0.0f)
            continue;
        if (view.Unusable)
            return kMustRefine;
        const double centerDistance = std::hypot((transform.Position.X - view.EyeX) + offset[0],
                                                 (transform.Position.Y - view.EyeY) + offset[1],
                                                 (transform.Position.Z - view.EyeZ) + offset[2]);
        const double errorPixels = ClusterGroupErrorPx(error, scale, centerDistance, worldRadius, view.ProjectionScaleY,
                                                       view.Height, lodBias, view.Orthographic);
        if (!std::isfinite(errorPixels))
            return kMustRefine;
        const double threshold = view.Budget * (wasRefined ? kClusterKeepFineBudgetScale : kClusterRefineBudgetScale);
        demand = std::max(demand, errorPixels / threshold);
    }
    return demand;
}

// The groups whose refinement draws streamed units: the non-terminal groups one level under the
// resident cut. A conservative stand-in for all of them: the mesh bounds and their largest error.
struct StreamedEntry
{
    Vector3 Center;
    float Radius = 0.0f;
    float Error = 0.0f;
};

StreamedEntry WholeMeshEntry(const MeshClusterLODManifest& manifest, uint32 mesh, const Mesh& coarse)
{
    const auto& range = manifest.Meshes[mesh];
    StreamedEntry entry;
    const Vector3 low{coarse.MinBounds[0], coarse.MinBounds[1], coarse.MinBounds[2]};
    const Vector3 high{coarse.MaxBounds[0], coarse.MaxBounds[1], coarse.MaxBounds[2]};
    entry.Center = (low + high) * 0.5f;
    entry.Radius = (high - low).Length() * 0.5f;
    for (uint32 group = range.FirstGroup; group < range.FirstGroup + range.GroupCount; ++group)
    {
        const auto& record = manifest.Groups[group];
        if (!record.Terminal && record.Depth + 1 == range.ResidentDepth)
            entry.Error = std::max(entry.Error, record.Error);
    }
    return entry;
}

// Parts copy the owner's renderer; the owner's handle is the only field they do not share.
bool SameRendererState(const Components::MeshRenderer& owner, const Components::MeshRenderer& copied)
{
    return owner.meshId == copied.meshId && owner.MeshNameId == copied.MeshNameId &&
        owner.materialAssetGuid == copied.materialAssetGuid && owner.modelAssetGuid == copied.modelAssetGuid &&
        owner.RenderLayers == copied.RenderLayers && owner.shadowCasting == copied.shadowCasting &&
        owner.receiveShadows == copied.receiveShadows && owner.motionVectors == copied.motionVectors;
}

// The registry key of a unit's geometry: after the model's own submesh keys. A resident unit
// registers its coarse range under it; a streamed unit's page uses it as its source key.
MeshGPUKey UnitKey(const GUID& model, const ModelAsset& asset, uint32 unit)
{
    return {model, static_cast<uint32>(asset.GetMeshes().size()) + unit};
}

struct PendingSplit
{
    ECS::EntityHandle Owner;
    Components::MeshRenderer Renderer;
    GUID Model;
    uint32 Mesh;
};

// The cut of a new split: the resident cut, LOD0 of the coarse mesh.
std::vector<ClusterLODCutDecision> ResidentCut(const MeshClusterLODManifest& manifest, uint32 mesh)
{
    const auto& range = manifest.Meshes[mesh];
    std::vector<ClusterLODCutDecision> decisions(range.GroupCount);
    for (uint32 local = 0; local < range.GroupCount; ++local)
    {
        const auto& group = manifest.Groups[range.FirstGroup + local];
        decisions[local] = group.Terminal || group.Depth >= range.ResidentDepth ? ClusterLODCutDecision::Refined
                                                                                : ClusterLODCutDecision::Coarse;
    }
    return decisions;
}

// HLOD replaced the member with its cluster proxy: the member and its parts draw nothing.
bool IsHlodEvicted(const ECS::World& world, ECS::EntityHandle entity)
{
    const auto* gpu = world.GetComponent<Components::MeshGPUData>(entity);
    return gpu && gpu->hlodEvicted;
}

} // namespace

struct ClusterLODFeature::State
{
    struct DecodeAccounting
    {
        std::atomic<uint64> ElementBytes{0};
    };

    struct Read
    {
        std::mutex Mutex;
        JobSystem::TaskHandle Task;
        std::weak_ptr<const void> PayloadOwner;
        std::shared_ptr<const Mesh> MeshData;
        bool Submitted = false;
        bool Failed = false;
    };

    struct DecodedMeshDeleter
    {
        std::shared_ptr<DecodeAccounting> Accounting;
        uint64 ElementBytes;

        void operator()(const Mesh* mesh) const noexcept
        {
            delete mesh;
            Accounting->ElementBytes.fetch_sub(ElementBytes, std::memory_order_relaxed);
        }
    };

    static SharedPtr<Asset> DecodePage(const std::shared_ptr<Read>& read,
                                      const MeshClusterLODManifest::Unit& directory, const Mesh& descriptor,
                                      const std::shared_ptr<std::atomic<bool>>& cancel,
                                      const std::shared_ptr<DecodeAccounting>& accounting,
                                      std::span<const uint8> bytes)
    {
        ScopedAssetDecodeCancellation cancellation(cancel);
        auto mesh = std::make_unique<Mesh>();
        String error;
        if (!DecodeMeshClusterLODFinePage(bytes, directory, descriptor, *mesh, error))
            return nullptr;
        const uint64 elements = mesh->Vertices.size() * sizeof(Vertex) + mesh->Indices.size() * sizeof(uint32);
        accounting->ElementBytes.fetch_add(elements, std::memory_order_relaxed);
        std::shared_ptr<const Mesh> owned(mesh.release(), DecodedMeshDeleter{accounting, elements});
        std::lock_guard lock(read->Mutex);
        read->MeshData = std::move(owned);
        return nullptr;
    }

    struct Page
    {
        MeshGPUKey Source;
        MeshGPUKey Fine{GUID::Generate(), 0};
        uint64 Incarnation = 0;
        MeshClusterLODManifest::Unit Directory;
        Mesh Descriptor;
        std::filesystem::path Store;
        MeshGPUHandle FineHandle;
        std::shared_ptr<const Mesh> Decoded;
        std::shared_ptr<Read> Request;
        std::shared_ptr<std::atomic<bool>> Cancel;
        std::atomic<uint32> Reservations{0};
        std::atomic<bool> MayHaveGpuReaders{false};
        uint64 Charge = 0;
        uint64 LastWantedEpoch = 0;
        uint64 RetryEpoch = 0;
        bool Wanted = false;
        bool Pending = false;
        bool Failed = false;
        bool Retiring = false;
        // One token for each of kReaderQueues, taken when retirement first waits for readers.
        std::array<IDevice::GpuSyncToken, kReaderQueues.size()> RetirementTokens{};
    };

    struct Lease
    {
        std::weak_ptr<const void> WorldOwner;
        std::weak_ptr<const void> CutOwner;
        std::vector<std::shared_ptr<Page>> Pages;
        uint64 FrameIndex = 0;
        std::atomic<bool> Released{false};

        ~Lease() { Release(); }
        void Release() noexcept
        {
            if (Released.exchange(true, std::memory_order_acq_rel))
                return;
            for (const auto& page : Pages)
                page->Reservations.fetch_sub(1, std::memory_order_acq_rel);
            Pages.clear();
        }
        void MarkReaders() const noexcept
        {
            for (const auto& page : Pages)
                page->MayHaveGpuReaders.store(true, std::memory_order_release);
        }
    };

    struct Binding
    {
        ECS::EntityHandle Entity;
        MeshGPUKey Source;
    };


    RenderServices* Services = nullptr;
    AssetManager* Assets = nullptr;
    FastHashMap<MeshGPUKey, std::shared_ptr<Page>> Pages;
    FastHashMap<RenderGraph::RGFrame*, std::weak_ptr<Lease>> Leases;
    ClusterLODSplits Splits;
    // Scratch of UpdateSplits: the cut inputs and decisions of every active split, in order.
    std::vector<ClusterLODCutGroup> CutGroups;
    std::vector<ClusterLODCutDecision> CutDecisions;
    std::vector<ClusterLODCutInstance> CutInstances;
    // The drawable geometry of every unit of one model (UnitGeometries).
    struct ModelGeometries
    {
        const ModelAsset* Model = nullptr;
        uint64 Incarnation = 0;
        std::vector<MeshGPUHandle> Handles;
        // The units whose storage waits for its upload: checked again once per preparation.
        std::vector<uint32> Waiting;
        uint64 CheckedEpoch = 0;
    };
    // Valid while the registry content revision holds: every change of a unit's geometry, except
    // an upload that lands, changes the revision.
    FastHashMap<GUID, ModelGeometries> Geometries;
    const MeshGPURegistry* GeometriesRegistry = nullptr;
    uint64 GeometriesRevision = 0;
    uint64 GeometriesEpoch = 0;
    // The epoch of the next fine page admission after fine storage refused one.
    uint64 AdmissionRetryEpoch = 0;
    // Resident unit registrations a full pool refused, with the epoch of their next try.
    FastHashMap<MeshGPUKey, uint64> RegistrationRetry;
    // The views of this preparation, as Demand reads them.
    std::vector<DemandView> DemandViews;
    // Scratch of MeasureGroups: 1 per unit of the measured instance that can draw.
    std::vector<uint8> UnitReady;
    // The owner world's entity count at the last stray-renderer check.
    size_t StrayCheckEntities = 0;
    std::shared_ptr<DecodeAccounting> DecodedAccounting = std::make_shared<DecodeAccounting>();
    std::shared_ptr<const void> CutOwner;
    uint64 PreparedEpoch = 0;
    uint64 OwnerSelectionEpoch = 0;
    uint64 WorldId = 0;
    uint64 WorldGeneration = 0;
    uint64 DeviceGeneration = 0;
    uint64 ChargedBytes = 0;
    uint64 TriangleBudget = 0;
    // The coarse triangles of the unsplit cluster instances in this preparation.
    uint64 UnsplitCoarseTriangles = 0;
    uint32 PendingReads = 0;
    // The parts that started or stopped drawing in the last preparation: a stable cut changes none.
    uint32 PartChanges = 0;
    bool Initialized = false;
    bool PolicyBlocked = false;
    // True while UpdateSplits iterates the splits: a split cannot end there.
    bool InSplitPass = false;

    ~State()
    {
        CutOwner.reset();
        for (const auto& [key, page] : Pages)
        {
            (void)key;
            if (page->Cancel)
                page->Cancel->store(true, std::memory_order_release);
        }
    }

    void CollectLeases()
    {
        for (auto it = Leases.begin(); it != Leases.end();)
        {
            auto lease = it->second.lock();
            if (!lease)
            {
                it = Leases.erase(it);
                continue;
            }
            if (lease->WorldOwner.expired() || lease->CutOwner.expired())
                lease->Release();
            ++it;
        }
    }

    void DrainReads()
    {
        for (auto& [key, pointer] : Pages)
        {
            (void)key;
            auto& page = *pointer;
            if (!page.Pending || !page.Request)
                continue;
            {
                std::lock_guard lock(page.Request->Mutex);
                if (!page.Request->PayloadOwner.expired() ||
                    (!page.Request->Failed && (!page.Request->Submitted || !page.Request->Task.IsDone())))
                    continue;
                page.Decoded = std::move(page.Request->MeshData);
            }
            page.Pending = false;
            --PendingReads;
            page.Request.reset();
            page.Failed = !page.Decoded;
            if (page.Failed)
            {
                ChargedBytes -= page.Charge;
                page.Charge = 0;
                page.RetryEpoch = Services->Spine().WorldFrameEpoch() + 120;
            }
        }
    }

    void Request(Page& page)
    {
        if (page.Failed && Services->Spine().WorldFrameEpoch() >= page.RetryEpoch)
            page.Failed = false;
        if (page.Pending || page.Decoded || page.Failed || PendingReads >= kMaximumPendingReads)
            return;
        // The charge scales the decoded page size. The encoded page in the store is smaller.
        const uint64 decodedBytes = static_cast<uint64>(page.Directory.VertexCount) * sizeof(Vertex) +
                                    static_cast<uint64>(page.Directory.IndexCount) * sizeof(uint32);
        if (decodedBytes > kMaximumPageWorkBytes / kPageWorkMultiplier)
        {
            page.Failed = true;
            return;
        }
        const uint64 charge = decodedBytes * kPageWorkMultiplier;
        if (charge > kMaximumPageWorkBytes - ChargedBytes)
            return;
        page.Charge = charge;
        ChargedBytes += charge;
        page.Pending = true;
        ++PendingReads;
        page.Cancel = std::make_shared<std::atomic<bool>>(false);
        const auto read = std::make_shared<Read>();
        page.Request = read;
        const auto directory = page.Directory;
        const auto descriptor = std::make_shared<const Mesh>(page.Descriptor);
        AssetIOService::ReadRequest request;
        request.AssetGuid = page.Fine.assetGuid;
        request.Metadata = AssetMetadata(page.Fine.assetGuid, AssetType::Model, page.Store);
        request.Priority = AssetLoadPriority::High;
        request.RangeOffset = directory.Offset;
        request.RangeBytes = directory.Bytes;
        request.MaximumBytes = directory.Bytes;
        request.CancelRequested = page.Cancel;
        const auto cancel = page.Cancel;
        const auto accounting = DecodedAccounting;
        const auto payloadOwner = std::make_shared<const uint8>(0);
        read->PayloadOwner = payloadOwner;
        request.ProcessData = [read, directory, descriptor, cancel, payloadOwner, accounting](ByteBuffer bytes) -> SharedPtr<Asset>
        {
            (void)payloadOwner;
            return DecodePage(read, directory, *descriptor, cancel, accounting, bytes);
        };
        request.OnSubmitted = [read](const JobSystem::TaskHandle& task)
        {
            std::lock_guard lock(read->Mutex);
            read->Task = task;
            read->Submitted = true;
        };
        request.OnFailure = [read](const String&)
        {
            std::lock_guard lock(read->Mutex);
            read->Failed = true;
        };
        Assets->SubmitRead(std::move(request));
    }

    // Unbinds a renderer from a fine page. A renderer with a model binds its own mesh again at
    // extraction; a split part draws nothing until its split ends and destroys it.
    bool RestoreBinding(ECS::World& world, const Binding& binding)
    {
        const auto page = Pages.find(binding.Source);
        auto* writable = world.GetComponentForWrite<Components::MeshRenderer>(binding.Entity);
        if (!writable || page == Pages.end() || writable->meshGpuHandleId != page->second->FineHandle.id)
            return false;
        writable->meshGpuHandleId = MeshGPUHandle{}.id;
        return true;
    }

    void RestoreBindings(ECS::World& world, const std::vector<Binding>& bindings)
    {
        bool changed = false;
        for (const auto& binding : bindings)
            changed |= RestoreBinding(world, binding);
        if (changed)
        {
            Services->NotifyRenderContentChanged(world.GetWorldId());
            Services->NotifyShadowCasterContentChanged(world.GetWorldId(), {}, ShadowCasterChangeScope::Unbounded);
        }
    }

    FastHashMap<uint64, MeshGPUKey> OwnedFineSources() const
    {
        FastHashMap<uint64, MeshGPUKey> fineSources;
        for (const auto& [source, page] : Pages)
        {
            if (page->FineHandle.IsValid())
                fineSources.emplace(page->FineHandle.id, source);
        }
        return fineSources;
    }

    bool HasClusterConsumers(ECS::World& world) const
    {
        const auto fineSources = OwnedFineSources();
        bool found = false;
        world.Query<ECS::Read<Components::MeshRenderer>>().IncludeDisabled().Each(
            [this, &fineSources, &found](const Components::MeshRenderer& renderer)
            {
                if (found)
                    return;
                const auto model = std::dynamic_pointer_cast<ModelAsset>(Assets->GetAsset(renderer.modelAssetGuid.ToGuid()));
                found = fineSources.contains(renderer.meshGpuHandleId) || (model && model->GetClusterLODManifest());
            });
        return found;
    }

    std::vector<Binding> CollectFineBindings(ECS::World& world) const
    {
        const auto fineSources = OwnedFineSources();
        std::vector<Binding> bindings;
        if (fineSources.empty())
            return bindings;
        world.Query<ECS::Read<Components::MeshRenderer>>().IncludeDisabled().Each(
            [&fineSources, &bindings](ECS::EntityHandle entity, const Components::MeshRenderer& renderer)
            {
                const auto source = fineSources.find(renderer.meshGpuHandleId);
                if (source != fineSources.end())
                    bindings.push_back({entity, source->second});
            });
        return bindings;
    }

    // Unbinds every renderer of `world` that draws a fine page.
    void RestoreWorld(ECS::World& world) { RestoreBindings(world, CollectFineBindings(world)); }

    // Unbinds the renderers of the owner world that draw a fine page but are not split parts:
    // copies of a part keep its fine handle, and only a part's split keeps the page alive. A copy
    // changes the entity count; other changes are found within one grace period.
    void UnbindStrayFineRenderers(ECS::World& world, uint64 epoch)
    {
        if (world.GetEntityCount() == StrayCheckEntities && epoch % kUnusedFrameGrace != 0)
            return;
        StrayCheckEntities = world.GetEntityCount();
        auto bindings = CollectFineBindings(world);
        if (bindings.empty())
            return;
        FastHashMap<ECS::EntityHandle, bool, ECS::EntityHandleHash> parts;
        for (const auto& instance : Splits.Instances())
            for (const ECS::EntityHandle part : instance.Parts)
                if (part.IsValid())
                    parts.emplace(part, true);
        std::erase_if(bindings, [&parts](const Binding& binding) { return parts.contains(binding.Entity); });
        RestoreBindings(world, bindings);
    }

    // Ends every split of the owner world now and restores the owners.
    void EndAllSplits()
    {
        auto* world = ECS::World::FindLiveWorld(WorldId);
        if (!world)
        {
            Splits.Clear();
            return;
        }
        std::vector<ECS::EntityHandle> owners;
        for (const auto& instance : Splits.Instances())
            owners.push_back(instance.Owner);
        for (const ECS::EntityHandle owner : owners)
            Splits.End(*world, Services->GetGPUScene(), owner);
        if (!owners.empty())
            Services->RequestResidencyExtraction();
    }

    bool HasFineInstanceReferences() const
    {
        const auto* scene = Services->GetGPUScene();
        if (!scene)
            return false;
        FastHashMap<uint32, bool> rows;
        for (const auto& [key, page] : Pages)
        {
            (void)key;
            const auto* entry = Services->GetMeshGPURegistry().Find(page->FineHandle);
            if (entry && entry->gpuMeshIndex != ~0u)
                rows.emplace(entry->gpuMeshIndex, true);
        }
        return std::any_of(scene->GetInstances().begin(), scene->GetInstances().end(),
            [&rows](const GPUInstance& instance) { return rows.contains(instance.meshIndex); });
    }

    void Block()
    {
        if (PolicyBlocked)
            return;
        PolicyBlocked = true;
        CutOwner.reset();
        // Inside the split pass, EndIdleSplits ends every split in the same preparation.
        if (!InSplitPass)
            EndAllSplits();
        for (auto& [key, page] : Pages)
        {
            (void)key;
            page->Wanted = false;
        }
        Logger::Log::Warning("Cluster LOD fine pages disabled: this prototype requires one cluster world without GI or reflection consumers");
    }

    // Takes the reader queue tokens once, then reports whether every one has completed. The
    // graphics token is required. An invalid compute or transfer token means no submit on that
    // queue yet, so no reader there.
    static bool ReadersComplete(Page& page, const IDevice& device)
    {
        auto& tokens = page.RetirementTokens;
        if (!tokens[0].IsValid())
            for (size_t queue = 0; queue < kReaderQueues.size(); ++queue)
                tokens[queue] = device.LastSubmissionToken(kReaderQueues[queue]);
        if (!tokens[0].IsValid() || device.QueryGpuSyncToken(tokens[0]) != IDevice::GpuSyncStatus::Complete)
            return false;
        return std::all_of(tokens.begin() + 1, tokens.end(), [&device](const IDevice::GpuSyncToken& token)
            { return !token.IsValid() || device.QueryGpuSyncToken(token) == IDevice::GpuSyncStatus::Complete; });
    }

    void RetireUnused(uint64 epoch)
    {
        auto& registry = Services->GetMeshGPURegistry();
        auto* device = Services->GetDevice();
        const auto* scene = Services->GetGPUScene();
        for (auto it = Pages.begin(); it != Pages.end();)
        {
            auto& page = *it->second;
            if ((!page.Retiring && (page.Wanted || epoch - page.LastWantedEpoch < kUnusedFrameGrace)) ||
                page.Pending || page.Reservations.load(std::memory_order_acquire) != 0)
            {
                ++it;
                continue;
            }
            if (const auto* entry = registry.Find(page.FineHandle))
            {
                const uint32 row = entry->gpuMeshIndex;
                if (row != ~0u && scene && std::any_of(scene->GetInstances().begin(), scene->GetInstances().end(),
                    [row](const GPUInstance& instance) { return instance.meshIndex == row; }))
                {
                    ++it;
                    continue;
                }
                if (!registry.CloseFineDraws(page.FineHandle))
                {
                    ++it;
                    continue;
                }
                page.Retiring = true;
                if (page.MayHaveGpuReaders.load(std::memory_order_acquire))
                {
                    if (DeviceGeneration != device->GetDeviceRebuildGeneration() || !ReadersComplete(page, *device))
                    {
                        ++it;
                        continue;
                    }
                }
                registry.UnregisterSubmesh(page.Fine);
            }
            ChargedBytes -= page.Charge;
            page.Decoded.reset();
            page.Descriptor = {};
            it = Pages.erase(it);
        }
    }

    // The page record of a streamed unit, made on first use. Null when the policy blocks pages or
    // the record limit is reached.
    std::shared_ptr<Page> TrackPage(const GUID& guid, const ModelAsset& model, uint32 unit)
    {
        const auto manifest = model.GetClusterLODManifest();
        const MeshGPUKey key = UnitKey(guid, model, unit);
        auto found = Pages.find(key);
        if (found == Pages.end())
        {
            if (PolicyBlocked || Pages.size() >= kMaximumPageRecords)
                return nullptr;
            const auto& record = manifest->Units[unit];
            auto page = std::make_shared<Page>();
            page->Source = key;
            page->Incarnation = manifest->Incarnation;
            page->Directory = record;
            page->Descriptor = PageDescriptor(model.GetMeshes()[record.Mesh]);
            page->Store = ResolveMeshClusterLODStore(model.GetPath(), Assets->GetAssetRoot(), *manifest);
            found = Pages.emplace(key, std::move(page)).first;
        }
        if (found->second->Incarnation != manifest->Incarnation)
        {
            Block();
            return nullptr;
        }
        return found->second;
    }

    // The drawable geometry of a unit, or an invalid handle. `waiting` is set when the unit has
    // storage whose upload has not landed. No side effects.
    MeshGPUHandle LookUpUnitGeometry(const GUID& guid, const ModelAsset& model, const MeshClusterLODManifest& manifest,
                                     uint32 unit, bool& waiting) const
    {
        const auto& registry = Services->GetMeshGPURegistry();
        const MeshGPUKey key = UnitKey(guid, model, unit);
        MeshGPUHandle handle;
        if (!manifest.Units[unit].IsStreamed())
            handle = registry.FindHandle(key);
        else if (const auto found = Pages.find(key); found != Pages.end() && !found->second->Retiring)
            handle = found->second->FineHandle;
        waiting = false;
        const auto* entry = registry.Find(handle);
        if (!entry)
            return {};
        if (registry.IsEntryDrawable(*entry))
            return handle;
        waiting = !entry->DrawClosed && !registry.IsFlushResident(*entry);
        return {};
    }

    MeshGPUHandle FindUnitGeometry(const GUID& guid, const ModelAsset& model, const MeshClusterLODManifest& manifest,
                                   uint32 unit) const
    {
        bool waiting = false;
        return LookUpUnitGeometry(guid, model, manifest, unit, waiting);
    }

    // Registers a resident unit or requests a streamed unit's page; returns the geometry once it
    // can draw.
    MeshGPUHandle PrepareUnitGeometry(const GUID& guid, const ModelAsset& model, uint32 unit, uint64 epoch,
                                      uint32& registrations)
    {
        const auto& record = model.GetClusterLODManifest()->Units[unit];
        if (record.IsStreamed())
        {
            if (const auto page = TrackPage(guid, model, unit))
            {
                page->Wanted = true;
                page->LastWantedEpoch = epoch;
            }
            return FindUnitGeometry(guid, model, *model.GetClusterLODManifest(), unit);
        }
        const MeshGPUHandle handle = FindUnitGeometry(guid, model, *model.GetClusterLODManifest(), unit);
        if (handle.IsValid() || registrations >= kMaximumUnitRegistrationsPerFrame)
            return handle;
        // A refused registration (a full pool) waits a grace period before it copies the unit again.
        const MeshGPUKey key = UnitKey(guid, model, unit);
        if (const auto retry = RegistrationRetry.find(key); retry != RegistrationRetry.end() && epoch < retry->second)
            return handle;
        ++registrations;
        Services->GetMeshGPURegistry().RegisterSubmesh(
            key, ExtractMeshClusterLODResidentUnit(model.GetMeshes()[record.Mesh], record));
        const MeshGPUHandle registered = FindUnitGeometry(guid, model, *model.GetClusterLODManifest(), unit);
        if (registered.IsValid())
            RegistrationRetry.erase(key);
        else
            RegistrationRetry[key] = epoch + kUnusedFrameGrace;
        return registered;
    }

    // Makes the part of one unit draw-ready: geometry first, then the part entity.
    void PrepareUnit(ECS::World& world, ClusterLODSplits::Instance& instance, const ModelAsset& model, uint32 unit,
                     uint64 epoch, uint32& registrations)
    {
        const auto& manifest = *model.GetClusterLODManifest();
        const MeshGPUHandle geometry = PrepareUnitGeometry(instance.Model, model, unit, epoch, registrations);
        UnitGeometries(instance.Model, model)[unit] = geometry;
        if (!geometry.IsValid())
            return;
        const auto& record = manifest.Units[unit];
        Splits.SetPart(world, instance, unit - manifest.Meshes[instance.Mesh].FirstUnit, geometry,
                       Mathematics::BoundingBox::FromMinMax(record.MinBounds, record.MaxBounds), epoch);
    }

    // Forgets the unit geometries when the registry changed since they were found.
    void ValidateGeometries(uint64 epoch)
    {
        const auto& registry = Services->GetMeshGPURegistry();
        const uint64 revision = registry.GetContentRevision();
        if (&registry != GeometriesRegistry || revision != GeometriesRevision)
            Geometries.clear();
        GeometriesRegistry = &registry;
        GeometriesRevision = revision;
        GeometriesEpoch = epoch;
    }

    // The drawable geometry of every unit of `model`, shared by its instances. A unit is looked up
    // again only after a registry change, or once per preparation while its upload waits.
    // PrepareUnit refreshes an entry it registers.
    std::vector<MeshGPUHandle>& UnitGeometries(const GUID& guid, const ModelAsset& model)
    {
        const auto& manifest = *model.GetClusterLODManifest();
        auto& geometries = Geometries[guid];
        if (geometries.Model != &model || geometries.Incarnation != manifest.Incarnation ||
            geometries.Handles.size() != manifest.Units.size())
        {
            geometries.Model = &model;
            geometries.Incarnation = manifest.Incarnation;
            geometries.Handles.assign(manifest.Units.size(), MeshGPUHandle{});
            geometries.Waiting.clear();
            for (uint32 unit = 0; unit < manifest.Units.size(); ++unit)
            {
                bool waiting = false;
                geometries.Handles[unit] = LookUpUnitGeometry(guid, model, manifest, unit, waiting);
                if (waiting)
                    geometries.Waiting.push_back(unit);
            }
            geometries.CheckedEpoch = GeometriesEpoch;
        }
        else if (geometries.CheckedEpoch != GeometriesEpoch)
        {
            geometries.CheckedEpoch = GeometriesEpoch;
            std::erase_if(geometries.Waiting, [this, &guid, &model, &manifest, &geometries](uint32 unit)
            {
                bool waiting = false;
                geometries.Handles[unit] = LookUpUnitGeometry(guid, model, manifest, unit, waiting);
                return !waiting;
            });
        }
        return geometries.Handles;
    }

    // `firstUnit` is the first unit of the instance's mesh: `unit` minus it is the part index.
    bool IsUnitReady(const ECS::World& world, ClusterLODSplits::Instance& instance,
                     std::span<const MeshGPUHandle> geometries, uint32 unit, uint32 firstUnit)
    {
        const MeshGPUHandle geometry = geometries[unit];
        return geometry.IsValid() && Splits.IsPartReady(world, instance, unit - firstUnit, geometry);
    }

    // The cut inputs of every group of one active split instance.
    void MeasureGroups(ECS::World& world, ClusterLODSplits::Instance& instance, const ModelAsset& model,
                       const Components::MeshRenderer& renderer, const Components::WorldTransform& transform)
    {
        const auto& manifest = *model.GetClusterLODManifest();
        const auto& range = manifest.Meshes[instance.Mesh];
        const float bias = LodBias(world, instance.Owner);
        // Every unit is a group's unit once and a producer's unit at most once: check it once.
        UnitReady.resize(range.UnitCount);
        const auto& geometries = UnitGeometries(instance.Model, model);
        for (uint32 local = 0; local < range.UnitCount; ++local)
            UnitReady[local] = IsUnitReady(world, instance, geometries, range.FirstUnit + local, range.FirstUnit) ? 1 : 0;
        for (uint32 local = 0; local < range.GroupCount; ++local)
        {
            const auto& group = manifest.Groups[range.FirstGroup + local];
            ClusterLODCutGroup state;
            state.WasRefined = IsClusterLODRefined(instance.Decisions[local]);
            if (!group.Terminal)
                state.Demand = Demand(DemandViews, renderer, transform, group.Center, group.Radius, group.Error,
                                      world.GetWorldId(), state.WasRefined, bias);
            state.UnitsReady = true;
            for (uint32 unit = group.FirstUnit; unit < group.FirstUnit + group.UnitCount && state.UnitsReady; ++unit)
                state.UnitsReady = UnitReady[unit - range.FirstUnit] != 0;
            state.ProducedReady = true;
            for (uint32 index = group.FirstProduced; index < group.FirstProduced + group.ProducedCount && state.ProducedReady;
                 ++index)
                state.ProducedReady = UnitReady[manifest.ProducedUnits[index] - range.FirstUnit] != 0;
            CutGroups.push_back(state);
        }
    }

    // Prepares the units of every group that wants to refine and is not refined, and the produced
    // units a group needs to coarsen. Then draws the units of the new cut and evicts the others.
    // The groups that want to refine are closed toward the root (the error and sphere of a group
    // cover the groups under it), so a group under an unrefined one fetches early, not in vain.
    // Returns true when a part changed.
    bool ApplyCut(ECS::World& world, GPUScene& scene, ClusterLODSplits::Instance& instance, const ModelAsset& model,
                  std::span<const ClusterLODCutGroup> groups, std::span<const ClusterLODCutDecision> decisions,
                  uint64 epoch, uint32& registrations)
    {
        const auto& manifest = *model.GetClusterLODManifest();
        const auto& range = manifest.Meshes[instance.Mesh];
        for (uint32 local = 0; local < range.GroupCount; ++local)
        {
            const auto& group = manifest.Groups[range.FirstGroup + local];
            if (decisions[local] == ClusterLODCutDecision::RefineBlocked)
                for (uint32 unit = group.FirstUnit; unit < group.FirstUnit + group.UnitCount; ++unit)
                    PrepareUnit(world, instance, model, unit, epoch, registrations);
            else if (decisions[local] == ClusterLODCutDecision::CoarsenBlocked)
                for (uint32 index = group.FirstProduced; index < group.FirstProduced + group.ProducedCount; ++index)
                    PrepareUnit(world, instance, model, manifest.ProducedUnits[index], epoch, registrations);
            // A group under the resident cut that refines, or wants to, draws streamed units.
            if (!group.Terminal && group.Depth < range.ResidentDepth &&
                (decisions[local] != ClusterLODCutDecision::Coarse || groups[local].Demand > 1.0))
                instance.LastWantedEpoch = epoch;
        }
        bool changed = false;
        for (uint32 local = 0; local < range.UnitCount; ++local)
        {
            const uint32 unit = range.FirstUnit + local;
            const bool drawn = IsClusterLODUnitDrawn(manifest, unit, decisions);
            // A unit of a refined group draws, or draws as soon as the group that made it
            // coarsens: its part and page stay, so a coarser cut is ready at once.
            const bool kept = IsClusterLODRefined(decisions[manifest.Units[unit].Group - range.FirstGroup]);
            if (kept)
                Splits.KeepPart(instance, local, epoch);
            if (kept && manifest.Units[unit].IsStreamed())
                if (const auto found = Pages.find(UnitKey(instance.Model, model, unit)); found != Pages.end())
                {
                    found->second->Wanted = true;
                    found->second->LastWantedEpoch = epoch;
                }
            const bool partChanged = Splits.ShowPart(world, scene, instance, local, drawn, epoch);
            PartChanges += partChanged ? 1u : 0u;
            changed |= partChanged;
        }
        instance.Decisions.assign(decisions.begin(), decisions.end());
        // Idle parts wait a grace period anyway: a pass every grace period is enough.
        if (epoch % kUnusedFrameGrace == 0)
            Splits.DestroyIdleParts(world, scene, instance, epoch, kUnusedFrameGrace);
        return changed;
    }

    // An inactive split prepares the parts of its first cut, the resident cut, and swaps the owner
    // for them once every one can draw. Returns true when the swap happened.
    bool ActivateSplit(ECS::World& world, GPUScene& scene, ClusterLODSplits::Instance& instance, const ModelAsset& model,
                       uint64 epoch, uint32& registrations)
    {
        const auto& manifest = *model.GetClusterLODManifest();
        const auto& range = manifest.Meshes[instance.Mesh];
        // A split stays while it prepares its first cut.
        instance.LastWantedEpoch = epoch;
        bool ready = true;
        for (uint32 unit = range.FirstUnit; unit < range.FirstUnit + range.UnitCount; ++unit)
        {
            if (!IsClusterLODUnitDrawn(manifest, unit, instance.Decisions))
                continue;
            PrepareUnit(world, instance, model, unit, epoch, registrations);
            ready = ready && IsUnitReady(world, instance, UnitGeometries(instance.Model, model), unit, range.FirstUnit);
        }
        if (!ready)
            return false;
        Splits.Activate(world, scene, instance);
        for (uint32 local = 0; local < range.UnitCount; ++local)
            Splits.ShowPart(world, scene, instance, local,
                            IsClusterLODUnitDrawn(manifest, range.FirstUnit + local, instance.Decisions), epoch);
        return true;
    }

    float LodBias(ECS::World& world, ECS::EntityHandle entity) const
    {
        const auto* lod = world.GetComponent<Components::LODGroup>(entity);
        return Services->GetLODGlobalBias() + (lod ? lod->Bias : 0.0f);
    }

    // An unsplit instance of a cluster mesh: it splits when a view wants a group under its
    // resident cut to refine.
    void ConsiderInstance(ECS::World& world, ECS::EntityHandle entity, const Components::MeshRenderer& renderer,
                          const Components::WorldTransform& transform, std::vector<PendingSplit>& splits)
    {
        const GUID guid = renderer.modelAssetGuid.ToGuid();
        if (guid.IsNull())
            return;
        const auto model = std::dynamic_pointer_cast<ModelAsset>(Assets->GetAsset(guid));
        if (!model || !model->GetClusterLODManifest())
            return;
        if (WorldId == 0)
        {
            WorldId = world.GetWorldId();
            WorldGeneration = world.GetEntityHandleEpoch();
        }
        // The parts of a split instance count their own triangles. An evicted member draws nothing.
        if (Splits.Find(entity) || IsHlodEvicted(world, entity))
            return;
        const auto& manifest = *model->GetClusterLODManifest();
        const uint32 mesh = ResolveSubmeshIndex(guid, *model, renderer);
        if (mesh >= model->GetMeshes().size())
            return;
        UnsplitCoarseTriangles += model->GetMeshes()[mesh].Indices.size() / 3;
        if (PolicyBlocked || !SupportsFineInstance(*Services, world, entity, renderer))
            return;
        // An ended split restores the owner, so the owner's coarse mesh must draw before a split starts.
        auto& registry = Services->GetMeshGPURegistry();
        const auto* coverage = registry.Find(MeshGPUHandle(renderer.meshGpuHandleId));
        if (!coverage || !registry.IsEntryDrawable(*coverage))
            return;
        const auto& range = manifest.Meshes[mesh];
        if (range.ResidentDepth == 0)
            return;
        const float bias = LodBias(world, entity);
        const uint64 worldId = world.GetWorldId();
        const StreamedEntry whole = WholeMeshEntry(manifest, mesh, model->GetMeshes()[mesh]);
        if (Demand(DemandViews, renderer, transform, whole.Center, whole.Radius, whole.Error, worldId, false, bias) <= 1.0)
            return;
        for (uint32 group = range.FirstGroup; group < range.FirstGroup + range.GroupCount; ++group)
        {
            const auto& record = manifest.Groups[group];
            if (record.Terminal || record.Depth + 1 != range.ResidentDepth)
                continue;
            if (Demand(DemandViews, renderer, transform, record.Center, record.Radius, record.Error, worldId, false, bias) > 1.0)
            {
                splits.push_back({entity, renderer, guid, mesh});
                return;
            }
        }
    }

    void BeginSplits(const std::vector<PendingSplit>& splits, uint64 epoch)
    {
        for (const PendingSplit& split : splits)
        {
            const auto model = std::dynamic_pointer_cast<ModelAsset>(Assets->GetAsset(split.Model));
            if (!model || !model->GetClusterLODManifest())
                continue;
            const auto& manifest = *model->GetClusterLODManifest();
            Splits.Begin(split.Owner, split.Renderer, split.Model, split.Mesh, ResidentCut(manifest, split.Mesh),
                         manifest.Meshes[split.Mesh].UnitCount, epoch);
        }
    }

    // True when the split must end: its owner, model, renderer or support changed.
    bool IsSplitStale(ECS::World& world, const ClusterLODSplits::Instance& instance, const ModelAsset* model) const
    {
        const auto* owner = world.GetComponent<Components::MeshRenderer>(instance.Owner);
        const auto* transform = world.GetComponent<Components::WorldTransform>(instance.Owner);
        return !owner || !transform || !model || !model->GetClusterLODManifest() ||
            owner->modelAssetGuid.ToGuid() != instance.Model ||
            ResolveSubmeshIndex(instance.Model, *model, *owner) != instance.Mesh ||
            !SameRendererState(*owner, instance.Renderer) || !SupportsFineInstance(*Services, world, instance.Owner, *owner) ||
            !ECS::Entity(&world, instance.Owner).IsEnabledInHierarchy() || IsHlodEvicted(world, instance.Owner);
    }

    // Selects the cut of every active split under the triangle budget, prepares what blocked
    // groups need, and swaps parts. Inactive splits prepare their first cut.
    void UpdateSplits(ECS::World& world, uint64 epoch)
    {
        InSplitPass = true;
        ValidateGeometries(epoch);
        auto* scene = Services->GetGPUScene();
        auto& instances = Splits.Instances();
        std::vector<ECS::EntityHandle> ended;
        std::vector<std::shared_ptr<ModelAsset>> models(instances.size());
        std::vector<uint32> active;
        std::vector<size_t> offsets;
        CutGroups.clear();
        for (uint32 index = 0; index < instances.size(); ++index)
        {
            auto& instance = instances[index];
            models[index] = std::dynamic_pointer_cast<ModelAsset>(Assets->GetAsset(instance.Model));
            if (!scene || IsSplitStale(world, instance, models[index].get()))
            {
                ended.push_back(instance.Owner);
                continue;
            }
            if (!instance.Active)
                continue;
            active.push_back(index);
            offsets.push_back(CutGroups.size());
            MeasureGroups(world, instance, *models[index], *world.GetComponent<Components::MeshRenderer>(instance.Owner),
                          *world.GetComponent<Components::WorldTransform>(instance.Owner));
        }
        CutDecisions.assign(CutGroups.size(), ClusterLODCutDecision::Coarse);
        CutInstances.clear();
        for (size_t slot = 0; slot < active.size(); ++slot)
        {
            const auto& instance = instances[active[slot]];
            const auto manifest = models[active[slot]]->GetClusterLODManifest();
            const size_t count = manifest->Meshes[instance.Mesh].GroupCount;
            CutInstances.push_back({manifest.get(), instance.Mesh,
                                    std::span<const ClusterLODCutGroup>(CutGroups.data() + offsets[slot], count),
                                    std::span<ClusterLODCutDecision>(CutDecisions.data() + offsets[slot], count)});
        }
        SelectClusterLODCuts(CutInstances, TriangleBudget, UnsplitCoarseTriangles);
        bool flipped = false;
        uint32 registrations = 0;
        PartChanges = 0;
        for (size_t slot = 0; slot < active.size(); ++slot)
            flipped |= ApplyCut(world, *scene, instances[active[slot]], *models[active[slot]], CutInstances[slot].Groups,
                                CutInstances[slot].Decisions, epoch, registrations);
        for (uint32 index = 0; index < instances.size(); ++index)
        {
            auto& instance = instances[index];
            if (!instance.Active && std::find(ended.begin(), ended.end(), instance.Owner) == ended.end())
                flipped |= ActivateSplit(world, *scene, instance, *models[index], epoch, registrations);
        }
        InSplitPass = false;
        for (const ECS::EntityHandle owner : ended)
            Splits.End(world, scene, owner);
        if (flipped || !ended.empty())
            Services->RequestResidencyExtraction();
    }

    // Ends the splits whose streamed units no view wanted for the grace period.
    void EndIdleSplits(ECS::World& world, uint64 epoch)
    {
        std::vector<ECS::EntityHandle> idle;
        for (const auto& instance : Splits.Instances())
            if (PolicyBlocked || epoch - instance.LastWantedEpoch >= kUnusedFrameGrace)
                idle.push_back(instance.Owner);
        for (const ECS::EntityHandle owner : idle)
            Splits.End(world, Services->GetGPUScene(), owner);
        if (!idle.empty())
            Services->RequestResidencyExtraction();
    }

    // The GPU entry holds the page now: its decoded copy and work charge are free. A lost entry
    // (a device rebuild) reads the page again.
    void ReleaseDecoded(Page& page)
    {
        page.Decoded.reset();
        ChargedBytes -= page.Charge;
        page.Charge = 0;
    }

    void AdmitPages(uint64 epoch)
    {
        auto& registry = Services->GetMeshGPURegistry();
        uint32 admissions = 0;
        for (auto& [key, page] : Pages)
        {
            (void)key;
            if (!page->Wanted || page->Retiring)
                continue;
            if (const auto* entry = registry.Find(page->FineHandle); entry && registry.IsEntryDrawable(*entry))
            {
                ReleaseDecoded(*page);
                continue;
            }
            Request(*page);
            if (!page->Decoded || admissions >= kMaximumAdmissionsPerFrame || epoch < page->RetryEpoch ||
                epoch < AdmissionRetryEpoch)
                continue;
            ++admissions;
            page->RetryEpoch = epoch + kUnusedFrameGrace;
            // No retained CPU mesh: picking resolves the stable coarse ModelAsset.
            page->FineHandle = registry.RegisterSubmesh(page->Fine, *page->Decoded, false, MeshGPUStorageDomain::ClusterFine);
            if (const auto* entry = registry.Find(page->FineHandle); entry && registry.IsEntryDrawable(*entry))
                ReleaseDecoded(*page);
            else
                // Fine storage is full: no page fits until one retires, so admission waits a grace
                // period instead of copying and hashing a page every frame.
                AdmissionRetryEpoch = epoch + kUnusedFrameGrace;
        }
    }

    void PublishMetrics() const
    {
        auto& metrics = Debug::DebugMetrics::Get();
        uint32 resident = 0;
        for (const auto& [key, page] : Pages)
        {
            (void)key;
            const auto* entry = Services->GetMeshGPURegistry().Find(page->FineHandle);
            resident += entry && Services->GetMeshGPURegistry().IsEntryDrawable(*entry) ? 1u : 0u;
        }
        metrics.PushSample("ClusterLOD.DecodedElementBytes",
            static_cast<float>(DecodedAccounting->ElementBytes.load(std::memory_order_relaxed)));
        metrics.PushSample("ClusterLOD.TrackedPages", static_cast<float>(Pages.size()));
        metrics.PushSample("ClusterLOD.SplitInstances", static_cast<float>(Splits.Count()));
        metrics.PushSample("ClusterLOD.PartChanges", static_cast<float>(PartChanges));
        metrics.PushSample("ClusterLOD.ResidentPages", static_cast<float>(resident));
        metrics.PushSample("ClusterLOD.PendingReads", static_cast<float>(PendingReads));
        metrics.PushSample("ClusterLOD.ChargedPageWorkBytes", static_cast<float>(ChargedBytes));
        metrics.PushSample("ClusterLOD.FineOwnedBufferBytes", static_cast<float>(Services->GetMeshGPURegistry()
            .GetStorageDomainCapacityBytes(MeshGPUStorageDomain::ClusterFine)));
    }
};

ClusterLODFeature::ClusterLODFeature() : m_State(std::make_unique<State>()) {}
ClusterLODFeature::~ClusterLODFeature() = default;

bool ClusterLODFeature::Initialize(RenderServices& services, AssetManager& assets)
{
    auto& state = *m_State;
    if (state.Initialized)
        return state.Services == &services && state.Assets == &assets;
    auto* device = services.GetDevice();
    // Fine geometry needs a coherent write into host-visible device memory (TryWriteCoherentBuffer).
    // Scene acceleration structures skip the fine domain, so ray tracing reads the coarse mesh.
    const GraphicsAPI api = device ? device->GetAPI() : GraphicsAPI::Auto;
    if (!device || (api != GraphicsAPI::Vulkan && api != GraphicsAPI::Metal) ||
        !device->GetCapabilities().supportsBufferDeviceAddress || !services.Spine().TryRequireCurrentWorldEpoch())
    {
        Logger::Log::Warning("Cluster LOD fine pages are off: they need Vulkan or Metal with buffer device addresses, "
                             "and they start only with the first world.");
        return false;
    }
    state.Services = &services;
    state.Assets = &assets;
    state.DeviceGeneration = device->GetDeviceRebuildGeneration();
    state.Initialized = true;
    auto& metrics = Debug::DebugMetrics::Get();
    metrics.RegisterMonitor("ClusterLOD.DecodedElementBytes", Debug::MonitorType::Memory, "bytes");
    metrics.RegisterMonitor("ClusterLOD.TrackedPages", Debug::MonitorType::Quantity, "pages");
    metrics.RegisterMonitor("ClusterLOD.SplitInstances", Debug::MonitorType::Quantity, "instances");
    metrics.RegisterMonitor("ClusterLOD.PartChanges", Debug::MonitorType::Quantity, "parts");
    metrics.RegisterMonitor("ClusterLOD.ResidentPages", Debug::MonitorType::Quantity, "pages");
    metrics.RegisterMonitor("ClusterLOD.PendingReads", Debug::MonitorType::Quantity, "reads");
    metrics.RegisterMonitor("ClusterLOD.ChargedPageWorkBytes", Debug::MonitorType::Memory, "bytes");
    metrics.RegisterMonitor("ClusterLOD.FineOwnedBufferBytes", Debug::MonitorType::Memory, "bytes");
    return true;
}

void ClusterLODFeature::Prepare(ECS::World& world)
{
    auto& state = *m_State;
    if (!state.Initialized)
        return;
    auto& services = *state.Services;
    const uint64 epoch = services.Spine().WorldFrameEpoch();
    if (state.WorldId != 0)
    {
        auto* ownerWorld = ECS::World::FindLiveWorld(state.WorldId);
        if (!ownerWorld || ownerWorld->GetEntityHandleEpoch() != state.WorldGeneration)
        {
            state.Splits.Clear();
            state.CutOwner.reset();
            state.Block();
            state.WorldGeneration = ownerWorld ? ownerWorld->GetEntityHandleEpoch() : 0;
            if (ownerWorld)
                state.RestoreWorld(*ownerWorld);
            else
                state.WorldId = 0;
        }
    }
    if (HasUnsupportedConsumers(world))
        state.Block();
    if (state.WorldId != 0 && state.WorldId != world.GetWorldId())
    {
        if (state.HasClusterConsumers(world))
            state.Block();
        if (state.PolicyBlocked)
            state.RestoreWorld(world);
        if (state.PreparedEpoch == epoch)
            return;
        state.CutOwner.reset();
        state.CollectLeases();
        state.DrainReads();
        state.RetireUnused(epoch);
        state.PreparedEpoch = epoch;
        if (!state.PolicyBlocked || !state.HasFineInstanceReferences())
            state.CutOwner = std::make_shared<const uint8>(0);
        state.PublishMetrics();
        return;
    }
    if (state.OwnerSelectionEpoch == epoch && state.WorldId != 0)
    {
        if (state.PolicyBlocked)
            state.RestoreWorld(world);
        return;
    }
    state.CutOwner.reset();
    state.CollectLeases();
    state.DrainReads();
    for (auto& [key, page] : state.Pages)
    {
        (void)key;
        page->Wanted = false;
    }

    std::vector<PendingSplit> splits;
    state.UnsplitCoarseTriangles = 0;
    CollectDemandViews(services, state.DemandViews);
    world.Query<ECS::Read<Components::MeshRenderer>, ECS::Read<Components::WorldTransform>>()
        .Each([&state, &world, &splits](ECS::EntityHandle entity, const Components::MeshRenderer& renderer,
                                        const Components::WorldTransform& transform)
        {
            state.ConsiderInstance(world, entity, renderer, transform, splits);
        });
    // A new split prepares its first cut in the frame it starts.
    state.BeginSplits(splits, epoch);
    state.UpdateSplits(world, epoch);
    state.AdmitPages(epoch);
    state.EndIdleSplits(world, epoch);
    state.UnbindStrayFineRenderers(world, epoch);
    if (state.PolicyBlocked)
        state.RestoreWorld(world);
    state.RetireUnused(epoch);
    state.PreparedEpoch = epoch;
    state.OwnerSelectionEpoch = epoch;
    if (!state.PolicyBlocked || !state.HasFineInstanceReferences())
        state.CutOwner = std::make_shared<const uint8>(0);
    state.PublishMetrics();
}

bool ClusterLODFeature::BindWorldGeometry(RenderGraph::RGFrame& frame, RenderServices& services,
                                         std::weak_ptr<const void> owner)
{
    auto& state = *m_State;
    if (!state.Initialized)
        return true;
    if (state.Services != &services || state.PreparedEpoch != services.Spine().WorldFrameEpoch())
        return false;
    if (state.PolicyBlocked)
    {
        if (state.HasFineInstanceReferences())
        {
            state.CutOwner.reset();
            return false;
        }
        if (!state.CutOwner)
            state.CutOwner = std::make_shared<const uint8>(0);
    }
    if (!state.CutOwner)
        return false;
    if (!frame.RequireExecutionLifetime(state.CutOwner))
        return false;
    const auto existing = state.Leases.find(&frame);
    if (existing != state.Leases.end())
    {
        if (auto lease = existing->second.lock(); lease && !lease->Released.load(std::memory_order_acquire) &&
            lease->FrameIndex == frame.FrameIndex() && !lease->WorldOwner.owner_before(owner) &&
            !owner.owner_before(lease->WorldOwner))
            return true;
    }
    auto lease = std::make_shared<State::Lease>();
    lease->WorldOwner = owner;
    lease->CutOwner = state.CutOwner;
    lease->FrameIndex = frame.FrameIndex();
    lease->Pages.reserve(state.Pages.size());
    for (const auto& [key, page] : state.Pages)
    {
        (void)key;
        if (!page->FineHandle.IsValid() || page->Retiring)
            continue;
        page->Reservations.fetch_add(1, std::memory_order_acq_rel);
        lease->Pages.push_back(page);
    }
    state.Leases[&frame] = lease;
    frame.OnSubmissionAttempt([lease]() noexcept { lease->MarkReaders(); });
    return true;
}

void ClusterLODFeature::SetTriangleBudget(uint64 triangles)
{
    m_State->TriangleBudget = triangles;
}

void ClusterLODFeature::OnDeviceRebuilt(Rendering::IDevice* device)
{
    auto& state = *m_State;
    state.CutOwner.reset();
    state.PreparedEpoch = 0;
    state.OwnerSelectionEpoch = 0;
    state.CollectLeases();
    if (!state.Initialized || !device)
        return;
    // The callback follows teardown and registry reprovision. Old GPU bindings are gone.
    state.DeviceGeneration = device->GetDeviceRebuildGeneration();
    for (auto& [key, page] : state.Pages)
    {
        (void)key;
        page->MayHaveGpuReaders.store(false, std::memory_order_release);
        page->RetryEpoch = 0;
        page->RetirementTokens = {};
    }
}
} // namespace GameEngine::Engine::Renderer
