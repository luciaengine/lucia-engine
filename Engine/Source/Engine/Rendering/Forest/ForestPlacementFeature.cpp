#include "Engine/Rendering/Forest/ForestPlacementFeature.h"

#include "Assets/AssetManager.h"
#include "Engine/Rendering/Forest/ForestImpostorDraw.h"
#include "Engine/Rendering/Forest/ForestNearMeshCandidates.h"
#include "Engine/Rendering/Forest/ForestPlacementGpuData.h"
#include "Engine/Rendering/Forest/ForestPlacementRenderState.h"
#include "Engine/Rendering/Impostors/ImpostorBaker.h"
#include "Engine/Rendering/Impostors/ImpostorStage.h"
#include "Engine/Rendering/MeshLODThresholds.h"
#include "Engine/Rendering/RenderServices.h"
#include "Core/Time.h"
#include "Engine/Rendering/ViewReadbackUtils.h"
#include "Logger/Logger.h"
#include "Platform/Environment.h"
#include "Rendering/Core/GPUScene.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <cstring>
#include <limits>

namespace GameEngine::Engine::Renderer
{
using namespace Rendering;
using namespace Rendering::RenderGraph;

ForestBenchmarkMode GetForestBenchmarkMode()
{
    static const auto mode = [] {
        const char* value = std::getenv("GE_FOREST_BENCHMARK_MODE");
        if (value && std::string_view(value) == "mesh")
            return ForestBenchmarkMode::Mesh;
        if (value && std::string_view(value) == "splats")
            return ForestBenchmarkMode::Splats;
        return ForestBenchmarkMode::Impostor;
    }();
    return mode;
}

bool ForestSplatHandoffEnabled()
{
    static const bool enabled = Platform::EnvironmentSwitchEnabled("GE_FOREST_SPLAT_FADE", true);
    return enabled && GetForestBenchmarkMode() == ForestBenchmarkMode::Splats;
}

struct ForestPlacementCountSample
{
    std::shared_ptr<RGBufferReadbackTicket> Ticket;
    uint64 Frame = 0;
    ViewId View{};
    uint32 StandPages = 0;
    uint32 DetailPages = 0;
    uint32 DetailPlacements = 0;
    uint32 Meshes = 0;
    uint32 MeshBatches = 0;
};

struct ForestPlacementSession
{
    ForestPlacementCountSample CountSample;
    std::shared_ptr<ForestPlacementRuntime> Placements;
    AssetManager* Assets = nullptr;
    float DetailDistance = 0;
    uint32 NearBudget = 0;
    std::vector<ForestNearMeshCandidate> NearDemand;
    std::vector<ForestMeshOwnedPlacement> NearOwners;
    std::vector<ForestMeshOwnedPlacement> PendingNearOwners;
    RGFrameStamp PendingNearFrame;
    ViewId NearOwnerView{};
    ViewId PendingNearView{};
    std::vector<ImpostorBakePart> SourceParts;
    uint64 World = 0;
    GUID Model;
    std::optional<ForestImpostorCard> Card;
    bool WantsCard = false;
    std::string Scope;
    IDevice* Device = nullptr;
    RGFrame* Graph = nullptr;
    BufferHandle Slots;
    BufferHandle Indices;
    ForestSplatDrawData Splats;
    bool FailedSplatLoad = false;
    ForestPlacementCull Cull;
    std::optional<ForestPlacementRenderState> Stream;
    ForestPlacementFrame Plan;
    std::vector<ForestPlacementGpuPage> Pages;
    RGFrameStamp Declared;
    bool ShaderRequested = false;
    bool ShaderAvailable = false;
    bool EmittedDetails = false;
    bool EmittedNearMeshes = false;

    ~ForestPlacementSession() { Reset(false); }

    void Reset(bool deviceRebuilt)
    {
        Stream.reset();
        CountSample = {};
        if (Device && !deviceRebuilt)
        {
            if (Indices.IsValid())
                Device->DestroyBuffer(Indices);
            if (Slots.IsValid())
                Device->DestroyBuffer(Slots);
            if (Splats.Cells.IsValid())
                Device->DestroyBuffer(Splats.Cells);
        }
        Indices = {};
        Slots = {};
        Splats = {};
        FailedSplatLoad = false;
        Cull = {};
        Graph = nullptr;
        Declared = {};
        ShaderRequested = false;
        ShaderAvailable = false;
        EmittedDetails = false;
        EmittedNearMeshes = false;
        WantsCard = false;
        Card.reset();
        NearDemand.clear();
        NearOwners.clear();
        PendingNearOwners.clear();
        PendingNearFrame = {};
        Placements->ResetResidency();
    }

    bool LoadSplatCells(IDevice& device)
    {
        const char* path = std::getenv("GE_FOREST_BENCHMARK_LVC");
        if (!path || !*path)
            return false;
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        const auto size = file.tellg();
        std::array<uint32, 6> header{};
        file.seekg(0);
        if (!file.read(reinterpret_cast<char*>(header.data()), sizeof(header)) ||
            header[0] != 0x3143564cu || header[1] == 0 || header[1] > 16384 ||
            size != static_cast<std::streamoff>(sizeof(header) + header[1] * 12u))
            return false;
        for (size_t i = 0; i < Splats.Grid.size(); ++i)
        {
            Splats.Grid[i] = std::bit_cast<float>(header[i + 2]);
            if (!std::isfinite(Splats.Grid[i]))
                return false;
        }
        if (Splats.Grid[3] <= 0)
            return false;
        std::vector<uint32> cells(header[1] * 3u);
        if (!file.read(reinterpret_cast<char*>(cells.data()), cells.size() * sizeof(uint32)))
            return false;

        std::vector<uint16> indices;
        indices.reserve(header[1] * 6u);
        for (uint32 i = 0; i < header[1]; ++i)
            for (const uint32 corner : {0u, 1u, 2u, 0u, 2u, 3u})
                indices.push_back(static_cast<uint16>(i * 4u + corner));
        Splats.CellBytes = cells.size() * sizeof(uint32);
        Splats.IndexCount = static_cast<uint32>(indices.size());
        const BufferDesc cellDesc{.size = Splats.CellBytes,
            .usage = static_cast<uint32>(BufferUsage::Storage | BufferUsage::TransferDst),
            .memoryUsage = BufferMemoryUsage::DeviceLocal, .debugName = "Forest.SplatCells"};
        const BufferDesc indexDesc{.size = indices.size() * sizeof(uint16),
            .usage = static_cast<uint32>(BufferUsage::Index | BufferUsage::TransferDst),
            .memoryUsage = BufferMemoryUsage::DeviceLocal, .debugName = "Forest.SplatIndices"};
        Splats.Cells = device.CreateBuffer(cellDesc);
        Indices = device.CreateBuffer(indexDesc);
        if (!Splats.Cells.IsValid() || !Indices.IsValid())
            return false;
        device.UpdateBuffer(Splats.Cells, 0, cellDesc.size, cells.data());
        device.UpdateBuffer(Indices, 0, indexDesc.size, indices.data());
        LOG_INFO("Forest benchmark: loaded {} animated splat cells", header[1]);
        return true;
    }

    bool EnsureGpu(IDevice& device)
    {
        Device = &device;
        if (!ShaderRequested)
        {
            ShaderRequested = true;
            ShaderAvailable = Cull.Initialize(device, GetForestBenchmarkMode() == ForestBenchmarkMode::Splats);
        }
        if (!ShaderAvailable)
            return false;
        if (!Slots.IsValid())
        {
            constexpr size_t kSlotBytes = PageStreaming::PackedPlacementStore::kPlacementsPerPage *
                                          sizeof(PageStreaming::PackedPlacement);
            const BufferDesc desc{.size = Placements->SlotCount() * kSlotBytes,
                .usage = static_cast<uint32>(BufferUsage::Storage | BufferUsage::TransferDst),
                .memoryUsage = BufferMemoryUsage::DeviceLocal, .debugName = "Forest.PlacementSlots"};
            Slots = device.CreateBuffer(desc);
        }
        if (GetForestBenchmarkMode() == ForestBenchmarkMode::Splats)
        {
            if (!Splats.Cells.IsValid() && !FailedSplatLoad && !LoadSplatCells(device))
            {
                FailedSplatLoad = true;
                LOG_ERROR("Forest benchmark: cannot load GE_FOREST_BENCHMARK_LVC");
            }
            if (FailedSplatLoad)
                return false;
        }
        else if (!Indices.IsValid())
        {
            constexpr std::array<uint16, 6> kIndices{0, 1, 2, 3, 4, 5};
            const BufferDesc desc{.size = sizeof(kIndices),
                .usage = static_cast<uint32>(BufferUsage::Index | BufferUsage::TransferDst),
                .memoryUsage = BufferMemoryUsage::DeviceLocal, .debugName = "Forest.CardIndices"};
            Indices = device.CreateBuffer(desc);
            if (Indices.IsValid())
                device.UpdateBuffer(Indices, 0, sizeof(kIndices), kIndices.data());
        }
        if (!Slots.IsValid() || !Indices.IsValid())
            return false;
        if (!Stream)
            Stream.emplace(Placements, Slots, Placements->SlotCount());
        return true;
    }
};

std::shared_ptr<ForestPlacementSession> ForestPlacementFeature::Attach(
    std::shared_ptr<ForestPlacementRuntime> placements, AssetManager& assets, uint64 world, const GUID& model,
    float detailDistance, uint32 nearBudget)
{
    if (!placements || placements->SlotCount() == 0 || !std::isfinite(detailDistance) || detailDistance <= 0 || model.IsNull())
        return {};
    std::erase_if(m_Sessions, [](const auto& session) { return session.expired(); });
    auto session = std::make_shared<ForestPlacementSession>();
    session->Placements = std::move(placements);
    session->Assets = &assets;
    session->DetailDistance = detailDistance;
    session->NearBudget = nearBudget;
    session->World = world;
    session->Model = model;
    session->Scope = "Forest.Detail." + std::to_string(++m_NextSession);
    m_Sessions.push_back(session);
    return session;
}

bool ForestPlacementFeature::WantsAtlas(uint64 world, const GUID& model) const
{
    for (const auto& entry : m_Sessions)
        if (const auto session = entry.lock(); session && session->World == world &&
            session->Model == model && session->WantsCard)
            return true;
    return false;
}

void ForestPlacementFeature::ClearCards(uint64 world)
{
    for (const auto& entry : m_Sessions)
        if (const auto session = entry.lock(); session && session->World == world)
            session->Card.reset();
}

void ForestPlacementFeature::PublishCard(uint64 world, const GUID& model, const ForestImpostorCard& card,
                                        std::span<const ImpostorBakePart> sources)
{
    for (const auto& entry : m_Sessions)
    {
        const auto session = entry.lock();
        if (!session || session->World != world || session->Model != model || !session->WantsCard)
            continue;
        session->Card = card;
        if (session->NearBudget == 0)
            continue;

        session->SourceParts.assign(sources.begin(), sources.end());
    }
}

namespace
{
constexpr auto kForestMeshKeywords = MaterialKeyword::Instanced | MaterialKeyword::ForestMeshInstances;

struct ForestMeshDrawConstants
{
    uint32 FirstTransform;
    uint32 MaterialIndex;
};
static_assert(sizeof(ForestMeshDrawConstants) == 8);

struct ForestNearDraw
{
    DrawBindings::BufferEntry Buffer;
    RGBuffer Upload;
    std::vector<DrawCommand> Commands;
    std::vector<ForestMeshOwnedPlacement> Owners;
};

void ObservePlacementCounts(ForestPlacementSession& session, const ForwardEmitContext& context,
                            const ForestPlacementCull::DrawBuffers* buffers, const ForestNearDraw* near)
{
    static const bool enabled = Platform::EnvironmentSwitchEnabled("GE_FOREST_STATS", false);
    if (!enabled)
        return;

    auto& sample = session.CountSample;
    if (sample.Ticket)
    {
        BufferReadbackResult result{};
        uint32 cards = 0;
        if (sample.Ticket->TryGet(result) && result.bytes.size() == sizeof(cards))
        {
            std::memcpy(&cards, result.bytes.data(), sizeof(cards));
            LOG_INFO("Forest.PlacedCount view={} frame={} standPages={} detailPages={} detailPlacements={} "
                     "cards={} meshes={} meshBatches={}", sample.View, sample.Frame, sample.StandPages,
                     sample.DetailPages, sample.DetailPlacements, cards, sample.Meshes, sample.MeshBatches);
        }
        if (sample.Ticket->IsConsumed())
            sample = {};
    }

    auto& frame = *context.Frame;
    if (sample.Ticket || !frame.ProfilingEnabled())
        return;

    uint32 placements = 0;
    for (const auto& page : session.Plan.DetailedDraws)
        placements += page.PlacementCount;
    const auto pages = static_cast<uint32>(session.Plan.DetailedDraws.size());
    const auto meshes = near ? static_cast<uint32>(near->Owners.size()) : 0u;
    const auto batches = near ? static_cast<uint32>(near->Commands.size()) : 0u;
    if (!buffers)
    {
        LOG_INFO("Forest.PlacedCount view={} frame={} standPages={} detailPages={} detailPlacements={} "
                 "cards=0 meshes={} meshBatches={}", context.ViewId, frame.FrameIndex(),
                 session.Plan.VisibleCells - pages, pages, placements, meshes, batches);
        return;
    }

    auto ticket = RequestBufferReadbackRG(context.Device, frame, buffers->Args,
        offsetof(ForestPlacementIndirectDraw, InstanceCount), sizeof(ForestPlacementIndirectDraw::InstanceCount),
        "Forest.PlacedCount");
    if (ticket)
        sample = {std::move(ticket), frame.FrameIndex(), context.ViewId, session.Plan.VisibleCells - pages, pages,
                  placements, meshes, batches};
}

struct ForestSourcePart
{
    const MeshGPUEntry* Mesh;
    const GPUMesh* Lod;
    const Material* Surface;
    MeshGPUHandle Handle;
    VertexAttributeFlags Attributes;
};

ForestNearDraw* PrepareNearDraw(ForestPlacementSession& session, const ForwardEmitContext& context,
    const CameraData& camera, const ForestPlacementView& view, MaterialKeyword pass, const PipelineFormatKey& format)
{
    RenderServices& services = *context.Services;
    const auto* scene = services.GetGPUScene();
    const bool meshOnly = GetForestBenchmarkMode() == ForestBenchmarkMode::Mesh;
    if ((!meshOnly && session.NearBudget == 0) || session.SourceParts.empty() || !scene || camera.cameraPos.w == 1.0f)
        return nullptr;

    const auto& card = *session.Card;
    float cutoff = card.SwitchCoverage > 0.0f ? card.SwitchCoverage :
        ImpostorStage::DefaultSwitchCoverage(services, context.ViewId);
    const uint32 force = services.ResolveViewForceLOD(context.ViewId);
    const auto scales = services.ComputeViewSseScales(context.ViewId);
    const auto& registry = services.GetMeshGPURegistry();
    const MeshGPURegistry::TableScope scope(registry);
    const auto& meshRows = scene->GetMeshes();
    const auto deformationMargins = services.Materials().MaterialDeformationMarginSpan();
    float deformationMargin = 0.0f;
    std::vector<ForestSourcePart> parts;
    parts.reserve(session.SourceParts.size());
    for (const auto& source : session.SourceParts)
    {
        const auto* mesh = registry.Find(source.Mesh);
        const auto* surface = services.Materials().Registry().Find(source.Material);
        MeshGPUEntryBindings bindings{};
        if (!mesh || !surface || mesh->gpuMeshIndex >= meshRows.size() || mesh->indexCount == 0 ||
            surface->GetGpuSceneMaterialIndex() == 0xFFFFFFFFu || IsSkinned(mesh->vertexFlags) ||
            !registry.TryGetDrawableBindings(*mesh, bindings))
            return nullptr;
        const auto& lod = meshRows[mesh->gpuMeshIndex];
        cutoff = std::min(cutoff, ForestPartCardEntryCoverage(lod, scales.Default, scales.Tight));
        const uint32 materialIndex = surface->GetGpuSceneMaterialIndex();
        if (materialIndex < deformationMargins.size())
            deformationMargin = std::max(deformationMargin, deformationMargins[materialIndex]);
        parts.push_back({mesh, &lod, surface, source.Mesh,
                         BoundStreamVertexFlags(*mesh, bindings, surface->IgnoresVertexColor())});
    }
    if (meshOnly || force != 0xFFFFFFFFu || registry.GetLodSelectionMode() == LodSelectionMode::Off)
        cutoff = 0.0f;

    const ForestNearMeshView nearView{view, std::abs(camera.proj[1][1]), services.GetLODGlobalBias(),
                                    cutoff, deformationMargin};
    const std::span<const ForestMeshOwnedPlacement> previous = session.NearOwnerView == context.ViewId
        ? std::span<const ForestMeshOwnedPlacement>(session.NearOwners) : std::span<const ForestMeshOwnedPlacement>{};
    CollectForestNearMeshCandidates(*session.Placements, session.Plan, nearView, card.Bounds,
                                    meshOnly ? session.Placements->SlotCount() *
                                        PageStreaming::PackedPlacementStore::kPlacementsPerPage : session.NearBudget,
                                    session.NearDemand, previous);
    if (session.NearDemand.empty())
        return nullptr;

    bool warm = true;
    for (const auto& part : parts)
        warm = services.Materials().Variants().WarmColorVariant(*part.Surface, part.Attributes,
            part.Mesh->topology, pass | kForestMeshKeywords, services.GetFrontFace(), format) && warm;
    if (!warm)
        return nullptr;

    auto& frame = *context.Frame;
    auto* draw = frame.AllocatePassData<ForestNearDraw>();
    std::vector<ForestMeshTransform> treeTransforms;
    std::vector<float> coverages;
    treeTransforms.reserve(session.NearDemand.size());
    coverages.reserve(session.NearDemand.size());
    draw->Owners.reserve(session.NearDemand.size());
    for (const auto& tree : session.NearDemand)
    {
        if (const auto transform = PackForestMeshTransform(tree))
        {
            treeTransforms.push_back(*transform);
            coverages.push_back(tree.Coverage);
            draw->Owners.push_back({tree.Ticket, tree.LocalPlacement});
        }
    }
    if (draw->Owners.empty())
        return nullptr;

    const auto bytes = treeTransforms.size() * parts.size() * sizeof(ForestMeshTransform);
    const auto upload = frame.AllocUpload(bytes);
    if (!upload.Valid())
        return nullptr;
    draw->Upload = frame.ImportExternalBuffer("Forest.NearMeshTransforms", upload.Buffer);
    if (!draw->Upload.IsValid())
        return nullptr;
    draw->Buffer = {HashStringId("ForestMeshTransforms"), upload.Buffer, upload.Offset, bytes};

    auto* transforms = static_cast<std::byte*>(upload.Ptr);
    std::vector<uint32> treeLods(treeTransforms.size());
    uint32 firstTransform = 0;
    static const auto producer = DrawProducerRegistry::Register("ForestNearMeshes");
    for (const auto& part : parts)
    {
        std::array<uint32, kMaxMeshLODs> lodCounts{};
        for (size_t tree = 0; tree < treeTransforms.size(); ++tree)
        {
            const uint32 lod = SelectForestMeshLod(*part.Lod, coverages[tree], scales.Default, scales.Tight, force);
            treeLods[tree] = lod;
            ++lodCounts[lod];
        }

        std::array<uint32, kMaxMeshLODs> writeOffsets{};
        for (uint32 lod = 0; lod < lodCounts.size(); ++lod)
        {
            const uint32 count = lodCounts[lod];
            if (count == 0)
                continue;
            const auto range = part.Mesh->DrawForLod(lod);
            if (range.IndexCount == 0 || range.VertexOffset > static_cast<uint32>(std::numeric_limits<int32>::max()))
                return nullptr;
            auto* constants = frame.AllocatePassData<ForestMeshDrawConstants>();
            *constants = {firstTransform, part.Surface->GetGpuSceneMaterialIndex()};
            writeOffsets[lod] = firstTransform;
            firstTransform += count;

            DrawCommand command{};
            command.Material = part.Surface;
            command.Geometry.Mesh = part.Handle;
            command.PassKeywords = kForestMeshKeywords;
            command.IndexCount = range.IndexCount;
            command.FirstIndex = range.FirstIndex;
            command.VertexOffset = static_cast<int32>(range.VertexOffset);
            command.InstanceCount = count;
            command.Bindings.PushConstants = std::as_bytes(std::span(constants, 1u));
            command.Bindings.Buffers = std::span(&draw->Buffer, 1u);
            command.Producer = producer;
            draw->Commands.push_back(command);
        }
        // Write each LOD range directly to the upload ring in candidate order.
        for (size_t tree = 0; tree < treeTransforms.size(); ++tree)
        {
            const uint32 offset = writeOffsets[treeLods[tree]]++;
            std::memcpy(transforms + size_t{offset} * sizeof(ForestMeshTransform),
                        &treeTransforms[tree], sizeof(ForestMeshTransform));
        }
    }
    return draw;
}

} // namespace

bool ForestPlacementFeature::EmitDetails(ForestPlacementSession& session, const ForwardEmitContext& context,
    const CameraData& camera, ForestPlacementView view, std::vector<DrawSubCommand>& standRanges,
    std::vector<ForestStandFadeDraw>& standFades)
{
    auto& frame = *context.Frame;
    auto& services = *context.Services;
    const auto* desc = services.Views().FindViewDesc(context.ViewId);
    if (!context.Device || !desc || desc->purpose != ViewPurpose::Game || desc->worldPaused ||
        (session.Graph && session.Graph != &frame) || session.Declared.IsFor(frame) ||
        !ImpostorStage::IsEnabled(services))
        return false;

    if (session.NearBudget != 0)
    {
        session.PendingNearFrame.Stamp(frame);
        session.PendingNearView = context.ViewId;
        session.PendingNearOwners.clear();
    }

    constexpr float kSplatHandoffWidth = 64.0f;
    const bool fade = ForestSplatHandoffEnabled();
    view.DetailFadeWidth = fade ? std::min(kSplatHandoffWidth, session.DetailDistance) : 0.0f;
    // The handoff starts at the detail distance, so full detail reaches as far as a hard switch.
    view.DetailDistance = session.DetailDistance + view.DetailFadeWidth;
    if (!session.Stream && !session.Placements->CanRequestDetails(view))
    {
        session.WantsCard = session.Placements->CanPrefetchAtlas(view);
        return false;
    }
    if (!session.EnsureGpu(*context.Device))
        return false;
    if (!session.Stream->Prepare(*session.Assets, *context.Device, frame, view, session.Plan))
        return false;
    session.Graph = &frame;
    session.Declared.Stamp(frame);
    session.WantsCard = session.Plan.WantsDetails;
    if (session.Plan.DetailedDraws.empty())
    {
        standRanges.swap(session.Plan.StandDraws);
        return true;
    }
    const auto& card = session.Card;
    if (!card)
        return false;

    const auto mode = GetForestBenchmarkMode();
    const bool meshOnly = mode == ForestBenchmarkMode::Mesh;
    const bool useSplats = mode == ForestBenchmarkMode::Splats;
    const auto pass = services.Views().GetViewDeclaredWorldPassKeywords(context.ViewId);
    const auto format = services.Views().GetViewWorldColorFormatKey(context.ViewId);
    if (!pass || format.ColorCount == 0)
        return false;

    const auto fadeKeyword = fade ? MaterialKeyword::LodCrossfade : MaterialKeyword::None;
    const Material* material = nullptr;
    if (!meshOnly)
    {
        const GUID guid = useSplats ? GUID::Derive(session.Model, "forest-benchmark-splats") : card->Material;
        material = services.Materials().Registry().Find(guid);
        if (!material && useSplats)
        {
            MaterialDocument doc{};
            doc.schemaVersion = 3;
            doc.materialName = "Forest benchmark splats";
            doc.lightingModel = "StandardPBR";
            doc.surfaceShader = "Surfaces/forest_benchmark_splat.glsl";
            doc.vertexModifier = "VertexModifiers/forest_benchmark_splats.glsl";
            doc.customVertexShader = true;
            doc.doubleSided = true;
            doc.alphaMode = MaterialAlphaMode::Mask;
            doc.properties["alphaCutoff"] = 0.5f;
            material = services.Materials().RegisterMaterialFromDocument(
                guid, doc, MaterialKeyword::None, MaterialSystem::BaseCompileMode::Async);
        }
        if (!material || !services.Materials().Variants().WarmColorVariant(*material, VertexAttributeFlags::None,
                PrimitiveTopology::TriangleList, (*pass & ~MaterialKeyword::Instanced) | fadeKeyword,
                services.GetFrontFace(), format))
            return false;
    }

    const ForestNearDraw* near = PrepareNearDraw(session, context, camera, view, *pass, format);
    const std::span<const ForestMeshOwnedPlacement> owners = near ? std::span(near->Owners) :
        std::span<const ForestMeshOwnedPlacement>{};
    if (meshOnly && !near)
        return false;
    if (!meshOnly)
    {
        ForestPlacementGpuView gpuView{};
        constexpr float kCardCornerRadiusScale = 1.414214f;
        const Mathematics::Vector3 center{card->Bounds.x, card->Bounds.y, card->Bounds.z};
        const float radius = center.Length() + card->Bounds.w * kCardCornerRadiusScale +
                             (useSplats ? std::max(0.65f, card->Sway.MaxDisplacement) : card->Sway.MaxDisplacement);
        if (!BuildForestPlacementGpuInputs(*session.Placements, session.Plan, camera, session.Placements->SlotCount(),
                radius, card->Bounds, gpuView, session.Pages, owners))
            return false;
        ForestSplatWindGpu wind{};
        if (useSplats)
        {
            ForwardLightUBO weather{};
            services.PackViewWeather(context.ViewId, desc->worldId, weather);
            std::copy_n(camera.renderOriginSector, 4, wind.Sector.begin());
            wind.TimeSeconds = services.TemporalHistory().ResolveDeformationClock(
                Time::GetCumulativeSeconds()).TimeSeconds;
            wind.MaxDisplacement = std::max(0.35f, card->Sway.MaxDisplacement);
            wind.Weather = weather.WeatherWind;
        }
        const auto buffers = session.Cull.Declare(frame, session.Scope, session.Slots,
            session.Placements->SlotCount(), gpuView, session.Pages, useSplats ? session.Splats.IndexCount : 6u,
            useSplats ? &wind : nullptr);
        if (!buffers || !EmitForestImpostorDraw(context, *buffers, session.Indices, card->Sway,
                DrawCommand{.Material = material, .PassKeywords = fadeKeyword}, nullptr, useSplats ? &session.Splats : nullptr))
            return false;
        ObservePlacementCounts(session, context, &*buffers, near);
    }
    else
    {
        ObservePlacementCounts(session, context, nullptr, near);
    }
    if (near)
    {
        session.PendingNearOwners.assign(owners.begin(), owners.end());
        std::ranges::sort(session.PendingNearOwners);
        services.EmitForwardSampledBufferRead(frame, context.ViewId, near->Upload, RGBufferRead::Storage,
                                             ForwardBufferReaders::WorldPass);
        for (const auto& command : near->Commands)
            services.EmitForwardCommand(context.ViewId, command, ForwardDrawDepth::ColourPass);
        if (!session.EmittedNearMeshes)
        {
            session.EmittedNearMeshes = true;
            LOG_INFO("Forest: registered {} near mesh placements in {} batches",
                     near->Owners.size(), near->Commands.size());
        }
    }
    if (!session.EmittedDetails)
    {
        session.EmittedDetails = true;
        LOG_INFO("Forest: registered packed {} draw with {} detail pages",
                 meshOnly ? "mesh LOD" : (useSplats ? "animated splat" : "impostor"), session.Plan.DetailedDraws.size());
    }
    standRanges.swap(session.Plan.StandDraws);
    standFades.swap(session.Plan.StandFades);
    return true;
}

void ForestPlacementFeature::Finish(RGFrame& frame, bool submitted)
{
    for (const auto& entry : m_Sessions)
        if (const auto session = entry.lock())
        {
            if (session->Stream)
                session->Stream->Finish(frame, submitted);
            if (!session->PendingNearFrame.IsFor(frame))
                continue;
            if (submitted)
            {
                session->NearOwners.swap(session->PendingNearOwners);
                session->NearOwnerView = session->PendingNearView;
            }
            session->PendingNearOwners.clear();
            session->PendingNearFrame = {};
        }
}

void ForestPlacementFeature::OnFrameSubmittedRG(RGFrame& frame, const IDevice::GpuSyncToken&)
{
    Finish(frame, true);
}

void ForestPlacementFeature::OnFrameAbortedRG(RGFrame& frame)
{
    Finish(frame, false);
}

void ForestPlacementFeature::OnFrameStreamRetiredRG(RGFrame& frame)
{
    for (const auto& entry : m_Sessions)
        if (const auto session = entry.lock(); session && session->Graph == &frame)
            session->Reset(false);
}

void ForestPlacementFeature::OnDeviceRebuilt(IDevice*)
{
    for (const auto& entry : m_Sessions)
        if (const auto session = entry.lock())
            session->Reset(true);
}

} // namespace GameEngine::Engine::Renderer
