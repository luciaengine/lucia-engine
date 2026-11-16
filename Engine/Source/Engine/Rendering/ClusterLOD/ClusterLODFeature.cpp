#include "Engine/Rendering/ClusterLOD/ClusterLODFeature.h"

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
constexpr uint32 kMaximumPageRecords = 256;
constexpr uint32 kMaximumPendingReads = 4;
constexpr uint32 kMaximumAdmissionsPerFrame = 2;
constexpr uint64 kPageWorkMultiplier = 8;
constexpr uint64 kUnusedFrameGrace = 30;
// A split registers the coarse part of every group of one mesh before its first frame.
constexpr uint32 kMaximumCoarsePartsPerFrame = 512;
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

bool WantsFine(const RenderServices& services, const Components::MeshRenderer& renderer,
               const Components::WorldTransform& transform, const MeshClusterLODManifest::Page& page,
               uint64 worldId, bool wasFine, float lodBias)
{
    if (page.IndexCount <= page.CoarseIndexCount)
        return false;
    const double minBounds[3]{page.MinBounds.x, page.MinBounds.y, page.MinBounds.z};
    const double maxBounds[3]{page.MaxBounds.x, page.MaxBounds.y, page.MaxBounds.z};
    double localCenter[3]{};
    double localExtent[3]{};
    double localOffset[3]{};
    for (uint32 axis = 0; axis < 3; ++axis)
    {
        localCenter[axis] = (minBounds[axis] + maxBounds[axis]) * 0.5;
        localExtent[axis] = (maxBounds[axis] - minBounds[axis]) * 0.5;
    }
    for (uint32 row = 0; row < 3; ++row)
        for (uint32 column = 0; column < 3; ++column)
            localOffset[row] += static_cast<double>(transform.Basis.Data()[column * 3 + row]) * localCenter[column];
    const double scale = ScaleBound(transform);
    const double radius = std::hypot(localExtent[0], localExtent[1], localExtent[2]) * scale;
    if (!std::isfinite(scale) || !std::isfinite(radius) ||
        !std::isfinite(localOffset[0]) || !std::isfinite(localOffset[1]) || !std::isfinite(localOffset[2]))
        return false;
    for (const auto& view : services.Views().GetViews())
    {
        if (!view.ActiveRenderLayerMask().Intersects(renderer.RenderLayers) ||
            (view.worldId != 0 && view.worldId != worldId))
            continue;
        const uint32 forced = services.ResolveViewForceLOD(view.id);
        if (forced == 0 || (forced == ~0u && services.GetMeshGPURegistry().GetLodSelectionMode() == LodSelectionMode::Off))
            return true;
        if (forced != ~0u || page.Error <= 0.0f)
            continue;
        const auto camera = services.Views().ResolveCameraData(view.id);
        const uint32 height = services.Views().GetViewWorldViewportHeight(view.id);
        if (height == 0)
            continue;
        const auto eye = CameraEye(camera);
        const double distance = std::max(0.001, std::hypot(
            (transform.Position.X - eye.X) + localOffset[0],
            (transform.Position.Y - eye.Y) + localOffset[1],
            (transform.Position.Z - eye.Z) + localOffset[2]) - radius);
        const double projection = std::abs(static_cast<double>(camera.proj[1][1])) * height * 0.5;
        const bool orthographic = std::abs(camera.proj[3][3]) > 0.5f;
        const double errorPixels = page.Error * scale * projection * std::exp2(static_cast<double>(lodBias)) /
                                   (orthographic ? 1.0 : distance);
        const float budget = ResolveLodBudgetPx(services.GetLODErrorBudgetPx(),
                                               services.GetLODViewBudgetOverride(view.purpose));
        const float threshold = budget * (wasFine ? 0.8f : 1.2f);
        if (!std::isfinite(errorPixels) || !std::isfinite(budget) || projection <= 0.0 || budget <= 0.0f || errorPixels > threshold ||
            services.GetMeshGPURegistry().GetLodSelectionMode() == LodSelectionMode::Off)
            return true;
    }
    return false;
}
// The pages of one coarse mesh: the manifest keeps them in mesh order.
std::pair<uint32, uint32> PageRange(const MeshClusterLODManifest& manifest, uint32 mesh)
{
    const auto byMesh = [](const MeshClusterLODManifest::Page& page, uint32 value) { return page.Mesh < value; };
    const auto first = std::lower_bound(manifest.Pages.begin(), manifest.Pages.end(), mesh, byMesh);
    const auto last = std::lower_bound(first, manifest.Pages.end(), mesh + 1, byMesh);
    return {static_cast<uint32>(first - manifest.Pages.begin()), static_cast<uint32>(last - manifest.Pages.begin())};
}

// A conservative stand-in for every page of one mesh: the mesh bounds and the largest error.
// When it does not want fine, no page of the mesh does.
MeshClusterLODManifest::Page WholeMeshPage(const MeshClusterLODManifest& manifest, const Mesh& coarse,
                                           std::pair<uint32, uint32> range)
{
    MeshClusterLODManifest::Page whole;
    whole.MinBounds = {coarse.MinBounds[0], coarse.MinBounds[1], coarse.MinBounds[2]};
    whole.MaxBounds = {coarse.MaxBounds[0], coarse.MaxBounds[1], coarse.MaxBounds[2]};
    for (uint32 page = range.first; page < range.second; ++page)
    {
        const auto& source = manifest.Pages[page];
        if (source.IndexCount <= source.CoarseIndexCount)
            continue;
        whole.Error = std::max(whole.Error, source.Error);
        // Only a page with more fine than coarse indices refines anything.
        whole.IndexCount = 1;
    }
    return whole;
}

// Parts copy the owner's renderer; the owner's handle is the only field they do not share.
bool SameRendererState(const Components::MeshRenderer& owner, const Components::MeshRenderer& copied)
{
    return owner.meshId == copied.meshId && owner.MeshNameId == copied.MeshNameId &&
        owner.materialAssetGuid == copied.materialAssetGuid && owner.modelAssetGuid == copied.modelAssetGuid &&
        owner.RenderLayers == copied.RenderLayers && owner.shadowCasting == copied.shadowCasting &&
        owner.receiveShadows == copied.receiveShadows && owner.motionVectors == copied.motionVectors;
}

// The registry key of a page's coarse part: after the model's own submesh keys.
MeshGPUKey CoarsePartKey(const GUID& model, const ModelAsset& asset, uint32 page)
{
    return {model, static_cast<uint32>(asset.GetMeshes().size()) + page};
}

struct PendingSplit
{
    ECS::EntityHandle Owner;
    Components::MeshRenderer Renderer;
    GUID Model;
    uint32 Mesh;
};

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
                                      const MeshClusterLODManifest::Page& directory, const Mesh& descriptor,
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
        MeshClusterLODManifest::Page Directory;
        Mesh Descriptor;
        std::filesystem::path Store;
        MeshGPUHandle CoarseHandle;
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

    struct PendingWorldRestore
    {
        uint64 WorldId;
        uint64 EntityEpoch;
        std::vector<Binding> Bindings;
    };

    struct Candidate
    {
        ECS::EntityHandle Entity;
        Components::MeshRenderer Renderer;
        std::shared_ptr<State::Page> Page;
        bool Fine;
        bool KeepCover;
    };

    RenderServices* Services = nullptr;
    AssetManager* Assets = nullptr;
    FastHashMap<MeshGPUKey, std::shared_ptr<Page>> Pages;
    FastHashMap<RenderGraph::RGFrame*, std::weak_ptr<Lease>> Leases;
    std::vector<Binding> Bindings;
    ClusterLODSplits Splits;
    std::vector<PendingWorldRestore> PendingWorldRestores;
    std::shared_ptr<DecodeAccounting> DecodedAccounting = std::make_shared<DecodeAccounting>();
    std::shared_ptr<const void> CutOwner;
    uint64 PreparedEpoch = 0;
    uint64 OwnerSelectionEpoch = 0;
    uint64 WorldId = 0;
    uint64 WorldGeneration = 0;
    uint64 DeviceGeneration = 0;
    uint64 ChargedBytes = 0;
    uint32 PendingReads = 0;
    bool Initialized = false;
    bool PolicyBlocked = false;

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
        if (page.Directory.Bytes > kMaximumPageWorkBytes / kPageWorkMultiplier)
        {
            page.Failed = true;
            return;
        }
        const uint64 charge = page.Directory.Bytes * kPageWorkMultiplier;
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

    bool MatchesSource(const Components::MeshRenderer& renderer, const Page& page) const
    {
        // A part has no model: its fine page always belongs to its own coarse part.
        return renderer.modelAssetGuid.ToGuid().IsNull();
    }

    bool RestoreBinding(ECS::World& world, const Binding& binding)
    {
        const auto* renderer = world.GetComponent<Components::MeshRenderer>(binding.Entity);
        const auto page = Pages.find(binding.Source);
        if (!renderer || page == Pages.end() || renderer->meshGpuHandleId != page->second->FineHandle.id)
            return false;
        auto& registry = Services->GetMeshGPURegistry();
        const bool sameSource = MatchesSource(*renderer, *page->second);
        const auto coarse = sameSource ? registry.FindHandle(page->second->Source) : MeshGPUHandle{};
        const auto* entry = registry.Find(coarse);
        if (sameSource && (!entry || !registry.IsEntryDrawable(*entry)))
            return false;
        auto* writable = world.GetComponentForWrite<Components::MeshRenderer>(binding.Entity);
        if (!writable || writable->meshGpuHandleId != page->second->FineHandle.id)
            return false;
        writable->meshGpuHandleId = coarse.id;
        return true;
    }

    void RestoreBindings(ECS::World& world, std::vector<Binding>& bindings)
    {
        bool changed = false;
        std::vector<Binding> pending;
        for (const auto& binding : bindings)
        {
            changed |= RestoreBinding(world, binding);
            const auto* renderer = world.GetComponent<Components::MeshRenderer>(binding.Entity);
            const auto page = Pages.find(binding.Source);
            if (renderer && page != Pages.end() && renderer->meshGpuHandleId == page->second->FineHandle.id)
                pending.push_back(binding);
        }
        bindings = std::move(pending);
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

    void RestoreWorld(ECS::World& world)
    {
        auto bindings = CollectFineBindings(world);
        RestoreBindings(world, bindings);
        std::erase_if(PendingWorldRestores, [&world](const PendingWorldRestore& pending)
            { return pending.WorldId == world.GetWorldId(); });
        if (!bindings.empty())
        {
            CutOwner.reset();
            PendingWorldRestores.push_back({world.GetWorldId(), world.GetEntityHandleEpoch(), std::move(bindings)});
        }
    }

    void RestorePendingWorlds()
    {
        for (auto it = PendingWorldRestores.begin(); it != PendingWorldRestores.end();)
        {
            auto* world = ECS::World::FindLiveWorld(it->WorldId);
            if (world)
            {
                if (world->GetEntityHandleEpoch() != it->EntityEpoch)
                {
                    it->Bindings = CollectFineBindings(*world);
                    it->EntityEpoch = world->GetEntityHandleEpoch();
                }
                RestoreBindings(*world, it->Bindings);
            }
            else
                it->Bindings.clear();
            if (it->Bindings.empty())
                it = PendingWorldRestores.erase(it);
            else
                ++it;
        }
        if (!PendingWorldRestores.empty())
            CutOwner.reset();
    }

    bool HasPendingRestore(const MeshGPUKey& source) const
    {
        return std::any_of(PendingWorldRestores.begin(), PendingWorldRestores.end(),
            [&source](const PendingWorldRestore& pending)
            {
                return std::any_of(pending.Bindings.begin(), pending.Bindings.end(),
                    [&source](const Binding& binding) { return binding.Source == source; });
            });
    }

    bool HasPendingPolicyCover() const
    {
        return !PendingWorldRestores.empty() || !Bindings.empty() || HasFineInstanceReferences();
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
        // The next owner preparation ends every split (EndIdleSplits): Block can run inside the split pass.
        if (auto* world = ECS::World::FindLiveWorld(WorldId))
            RestoreBindings(*world, Bindings);
        else
            Bindings.clear();
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
                page.Pending || HasPendingRestore(page.Source) || page.Reservations.load(std::memory_order_acquire) != 0)
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

    // Selects coarse or fine for one part of a split instance. `transform` is the owner's world
    // transform: a part sits at its owner's origin. Returns whether a view wants the fine page.
    bool Consider(ECS::World& world, ECS::EntityHandle entity, const Components::MeshRenderer& renderer,
                  const Components::WorldTransform& transform, const GUID& guid, const ModelAsset& model,
                  uint32 pageIndex, float bias, uint64 epoch, std::vector<Candidate>& candidates, uint32& coarseRepairs)
    {
        auto& services = *Services;
        auto& registry = services.GetMeshGPURegistry();
        const auto manifest = model.GetClusterLODManifest();
        const auto& directory = manifest->Pages[pageIndex];
        const MeshGPUKey key = CoarsePartKey(guid, model, pageIndex);
        auto found = Pages.find(key);
        const bool wasFine = found != Pages.end() && found->second->FineHandle.IsValid() &&
            renderer.meshGpuHandleId == found->second->FineHandle.id;
        const bool supported = SupportsFineInstance(services, world, entity, renderer);
        const bool demand = supported &&
            WantsFine(services, renderer, transform, directory, world.GetWorldId(), wasFine, bias);
        if (found == Pages.end())
        {
            if (!demand || PolicyBlocked || Pages.size() >= kMaximumPageRecords)
                return demand;
            const auto handle = registry.FindHandle(key);
            const auto* coarseEntry = registry.Find(handle);
            if (!coarseEntry || !registry.IsEntryDrawable(*coarseEntry))
                return demand;
            auto page = std::make_shared<State::Page>();
            page->Source = key;
            page->Incarnation = manifest->Incarnation;
            page->Directory = directory;
            page->Descriptor = PageDescriptor(model.GetMeshes()[directory.Mesh]);
            page->Store = ResolveMeshClusterLODStore(model.GetPath(), Assets->GetAssetRoot(), *manifest);
            page->CoarseHandle = handle;
            found = Pages.emplace(key, std::move(page)).first;
        }
        auto page = found->second;
        if (page->Incarnation != manifest->Incarnation)
        {
            Block();
            return demand;
        }
        page->CoarseHandle = registry.FindHandle(key);
        const auto* coarseEntry = registry.Find(page->CoarseHandle);
        bool coarseReady = coarseEntry && registry.IsEntryDrawable(*coarseEntry);
        if (!coarseReady && coarseRepairs < kMaximumAdmissionsPerFrame)
        {
            ++coarseRepairs;
            page->CoarseHandle = RegisterCoarsePart(guid, model, pageIndex);
            coarseEntry = registry.Find(page->CoarseHandle);
            coarseReady = coarseEntry && registry.IsEntryDrawable(*coarseEntry);
        }
        const auto* fineEntry = registry.Find(page->FineHandle);
        const bool keepCover = wasFine && !coarseReady && fineEntry && registry.IsEntryDrawable(*fineEntry);
        if (keepCover && !supported)
            Block();
        const bool fine = keepCover || (coarseReady && !PolicyBlocked && !page->Retiring && demand);
        page->Wanted |= fine;
        if (fine)
            page->LastWantedEpoch = epoch;
        candidates.push_back({entity, renderer, std::move(page), fine, keepCover});
        return demand;
    }

    MeshGPUHandle RegisterCoarsePart(const GUID& guid, const ModelAsset& model, uint32 pageIndex)
    {
        const auto& directory = model.GetClusterLODManifest()->Pages[pageIndex];
        return Services->GetMeshGPURegistry().RegisterSubmesh(
            CoarsePartKey(guid, model, pageIndex), ExtractMeshClusterLODCoarseGroup(model.GetMeshes()[directory.Mesh], directory));
    }

    float LodBias(ECS::World& world, ECS::EntityHandle entity) const
    {
        const auto* lod = world.GetComponent<Components::LODGroup>(entity);
        return Services->GetLODGlobalBias() + (lod ? lod->Bias : 0.0f);
    }

    // An unsplit instance of a cluster mesh: it splits when a view wants one of its fine pages.
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
        if (PolicyBlocked || Splits.Find(entity) || IsHlodEvicted(world, entity))
            return;
        const auto& manifest = *model->GetClusterLODManifest();
        const uint32 mesh = ResolveSubmeshIndex(guid, *model, renderer);
        if (mesh >= model->GetMeshes().size() || !SupportsFineInstance(*Services, world, entity, renderer))
            return;
        // An ended split restores the owner, so the owner's coarse mesh must draw before a split starts.
        auto& registry = Services->GetMeshGPURegistry();
        const auto* coverage = registry.Find(MeshGPUHandle(renderer.meshGpuHandleId));
        if (!coverage || !registry.IsEntryDrawable(*coverage))
            return;
        const auto range = PageRange(manifest, mesh);
        const float bias = LodBias(world, entity);
        const uint64 worldId = world.GetWorldId();
        if (!WantsFine(*Services, renderer, transform, WholeMeshPage(manifest, model->GetMeshes()[mesh], range), worldId, false, bias))
            return;
        for (uint32 page = range.first; page < range.second; ++page)
            if (WantsFine(*Services, renderer, transform, manifest.Pages[page], worldId, false, bias))
            {
                splits.push_back({entity, renderer, guid, mesh});
                return;
            }
    }

    // Starts each pending split once every coarse part of its mesh can draw.
    void BeginSplits(ECS::World& world, const std::vector<PendingSplit>& splits, uint64 epoch)
    {
        auto& registry = Services->GetMeshGPURegistry();
        uint32 coarseRepairs = 0;
        for (const PendingSplit& split : splits)
        {
            const auto model = std::dynamic_pointer_cast<ModelAsset>(Assets->GetAsset(split.Model));
            if (!model || !model->GetClusterLODManifest())
                continue;
            const auto range = PageRange(*model->GetClusterLODManifest(), split.Mesh);
            std::vector<MeshGPUHandle> parts;
            std::vector<Mathematics::BoundingBox> bounds;
            parts.reserve(range.second - range.first);
            bounds.reserve(range.second - range.first);
            for (uint32 page = range.first; page < range.second; ++page)
            {
                auto handle = registry.FindHandle(CoarsePartKey(split.Model, *model, page));
                const auto* entry = registry.Find(handle);
                if ((!entry || !registry.IsEntryDrawable(*entry)) && coarseRepairs < kMaximumCoarsePartsPerFrame)
                {
                    ++coarseRepairs;
                    handle = RegisterCoarsePart(split.Model, *model, page);
                    entry = registry.Find(handle);
                }
                if (!entry || !registry.IsEntryDrawable(*entry))
                    break;
                const auto& directory = model->GetClusterLODManifest()->Pages[page];
                parts.push_back(handle);
                bounds.push_back(Mathematics::BoundingBox::FromMinMax(directory.MinBounds, directory.MaxBounds));
            }
            if (parts.size() == range.second - range.first)
                Splits.Begin(world, split.Owner, split.Renderer, split.Model, split.Mesh, range.first, parts, bounds, epoch);
        }
    }

    // Selects every part of every split instance, then swaps owners for parts that can draw.
    void ConsiderSplits(ECS::World& world, uint64 epoch, std::vector<Candidate>& candidates, uint32& coarseRepairs)
    {
        auto* scene = Services->GetGPUScene();
        std::vector<ECS::EntityHandle> ended;
        bool flipped = false;
        for (auto& instance : Splits.Instances())
        {
            const auto* owner = world.GetComponent<Components::MeshRenderer>(instance.Owner);
            const auto* transform = world.GetComponent<Components::WorldTransform>(instance.Owner);
            const auto model = std::dynamic_pointer_cast<ModelAsset>(Assets->GetAsset(instance.Model));
            if (!owner || !transform || !model || !model->GetClusterLODManifest() || owner->modelAssetGuid.ToGuid() != instance.Model ||
                ResolveSubmeshIndex(instance.Model, *model, *owner) != instance.Mesh || !scene ||
                !SameRendererState(*owner, instance.Renderer) || !SupportsFineInstance(*Services, world, instance.Owner, *owner) ||
                !ECS::Entity(&world, instance.Owner).IsEnabledInHierarchy() || IsHlodEvicted(world, instance.Owner))
            {
                ended.push_back(instance.Owner);
                continue;
            }
            const float bias = LodBias(world, instance.Owner);
            for (uint32 part = 0; part < instance.Parts.size(); ++part)
            {
                const auto* renderer = world.GetComponent<Components::MeshRenderer>(instance.Parts[part]);
                if (!renderer)
                {
                    ended.push_back(instance.Owner);
                    break;
                }
                if (Consider(world, instance.Parts[part], *renderer, *transform, instance.Model, *model,
                             instance.FirstPage + part, bias, epoch, candidates, coarseRepairs))
                    instance.LastWantedEpoch = epoch;
            }
            flipped |= Splits.TryActivate(world, *scene, instance);
        }
        for (const ECS::EntityHandle owner : ended)
            Splits.End(world, scene, owner);
        if (flipped || !ended.empty())
            Services->RequestResidencyExtraction();
    }

    // Ends the splits whose fine pages no view wanted for the grace period.
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

    void AdmitPages(uint64 epoch)
    {
        auto& registry = Services->GetMeshGPURegistry();
        uint32 admissions = 0;
        for (auto& [key, page] : Pages)
        {
            (void)key;
            if (!page->Wanted || page->Retiring)
                continue;
            Request(*page);
            const auto* entry = registry.Find(page->FineHandle);
            if (entry && registry.IsEntryDrawable(*entry))
                continue;
            if (!page->Decoded || admissions >= kMaximumAdmissionsPerFrame || epoch < page->RetryEpoch)
                continue;
            ++admissions;
            page->RetryEpoch = epoch + kUnusedFrameGrace;
            // No retained CPU mesh: picking resolves the stable coarse ModelAsset.
            page->FineHandle = registry.RegisterSubmesh(page->Fine, *page->Decoded, false, MeshGPUStorageDomain::ClusterFine);
        }
    }

    void ApplySelections(ECS::World& world, const std::vector<Candidate>& candidates)
    {
        auto& services = *Services;
        auto& registry = services.GetMeshGPURegistry();
        auto oldBindings = CollectFineBindings(world);
        Bindings.clear();
        FastHashMap<ECS::EntityHandle, bool, ECS::EntityHandleHash> selected;
        bool changed = false;
        for (auto& candidate : candidates)
        {
            const auto& page = *candidate.Page;
            const auto* entry = registry.Find(page.FineHandle);
            const bool fine = (!PolicyBlocked || candidate.KeepCover) && !page.Retiring &&
                candidate.Fine && entry && registry.IsEntryDrawable(*entry);
            const auto handle = fine ? page.FineHandle : page.CoarseHandle;
            if (candidate.Renderer.meshGpuHandleId != handle.id)
            {
                auto* writable = world.GetComponentForWrite<Components::MeshRenderer>(candidate.Entity);
                if (!writable || writable->modelAssetGuid != candidate.Renderer.modelAssetGuid ||
                    writable->meshGpuHandleId != candidate.Renderer.meshGpuHandleId)
                    continue;
                writable->meshGpuHandleId = handle.id;
                changed = true;
            }
            if (fine)
            {
                Bindings.push_back({candidate.Entity, page.Source});
                selected.emplace(candidate.Entity, true);
            }
        }
        // Disabled entities keep their cached handle outside the selection query.
        for (const auto& binding : oldBindings)
        {
            if (selected.contains(binding.Entity))
                continue;
            changed |= RestoreBinding(world, binding);
            const auto* renderer = world.GetComponent<Components::MeshRenderer>(binding.Entity);
            const auto page = Pages.find(binding.Source);
            if (renderer && page != Pages.end() && renderer->meshGpuHandleId == page->second->FineHandle.id)
            {
                Bindings.push_back(binding);
                page->second->Wanted = true;
            }
        }
        if (changed)
        {
            services.NotifyRenderContentChanged(world.GetWorldId());
            services.NotifyShadowCasterContentChanged(world.GetWorldId(), {}, ShadowCasterChangeScope::Unbounded);
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
            state.Bindings.clear();
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
    state.RestorePendingWorlds();
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
        if (!state.PolicyBlocked || !state.HasPendingPolicyCover())
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

    std::vector<State::Candidate> candidates;
    uint32 coarseRepairs = 0;
    std::vector<PendingSplit> splits;
    world.Query<ECS::Read<Components::MeshRenderer>, ECS::Read<Components::WorldTransform>>()
        .Each([&state, &world, &splits](ECS::EntityHandle entity, const Components::MeshRenderer& renderer,
                                        const Components::WorldTransform& transform)
        {
            state.ConsiderInstance(world, entity, renderer, transform, splits);
        });
    // A new split requests its pages in the frame it starts.
    state.BeginSplits(world, splits, epoch);
    state.ConsiderSplits(world, epoch, candidates, coarseRepairs);

    state.AdmitPages(epoch);

    state.ApplySelections(world, candidates);
    state.EndIdleSplits(world, epoch);
    if (state.PolicyBlocked)
        state.RestoreWorld(world);
    state.RetireUnused(epoch);
    state.PreparedEpoch = epoch;
    state.OwnerSelectionEpoch = epoch;
    if (!state.PolicyBlocked || !state.HasPendingPolicyCover())
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
        if (state.HasPendingPolicyCover())
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
