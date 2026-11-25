#include "ForestPlacementFeature.h"

#include "Assets/AssetManager.h"
#include "ForestImpostorDraw.h"
#include "ForestNearMeshCandidates.h"
#include "ForestPlacementGpuData.h"
#include "ForestPlacementSession.h"
#include "Engine/Rendering/DepthDrawRecorder.h"
#include "Core/Time.h"
#include "Engine/Rendering/Forest/ForestSplatCells.h"
#include "Engine/Rendering/FrameOrchestrator.h"
#include "Engine/Rendering/Impostors/ImpostorBaker.h"
#include "Engine/Rendering/Impostors/ImpostorStage.h"
#include "Engine/Rendering/MeshLODThresholds.h"
#include "Engine/Rendering/RenderOrigin.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/TextureService.h"
#include "Engine/Rendering/ViewReadbackUtils.h"
#include "Logger/Logger.h"
#include "Platform/Environment.h"
#include "Rendering/Core/GPUScene.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <numbers>

namespace GameEngine::Engine::Renderer
{
using namespace Rendering;
using namespace Rendering::RenderGraph;

namespace
{
// The largest radius scale of a splat disk (forest_splat_disk.glsl: kForestSplatOverheadGrowth).
constexpr float kSplatDiskGrowth = 1.15f;
// The disk radius in cells at full coverage (forest_splats.glsl: kForestSplatDiskRadius).
constexpr float kSplatDiskRadius = 0.79788456f;
// The smallest sway in meters of a splat crown; a calm card still lets the splats move.
constexpr float kSplatMinimumSway = 0.35f;
constexpr float kSplatJitterRadius = 0.519616f; // sqrt(3) times 0.3 cells per axis.

float SplatReach(const ForestSplatCells& splats)
{
    float reach = 0.0f;
    for (const ForestSplatCell& cell : splats.Cells)
    {
        const uint32 word = cell.PositionCoverage;
        const Mathematics::Vector3 grid{static_cast<float>(word & 0xffu), static_cast<float>((word >> 8u) & 0xffu),
                                        static_cast<float>((word >> 16u) & 0xffu)};
        reach = std::max(reach, (splats.Origin + grid * splats.CellSize).Length());
    }
    return reach + splats.CellSize * kSplatDiskRadius * kSplatDiskGrowth;
}

bool FitsPageOcclusionBounds(const ForestPlacementSession& session)
{
    if (!session.Card)
        return false;
    const auto& card = *session.Card;
    const float reach = session.Splats
        ? session.SplatReach + session.Splats->CellSize * kSplatJitterRadius +
              std::max(kSplatMinimumSway, card.Sway.MaxDisplacement)
        : ForestCardReach(card);
    return reach <= session.Placements->Store().BoundRadius();
}
} // namespace

std::shared_ptr<ForestPlacementSession> ForestPlacementFeature::Attach(
    std::shared_ptr<ForestPlacementRuntime> placements, AssetManager& assets, uint64 world, const GUID& model,
    float detailDistance, uint32 nearBudget, std::shared_ptr<const ForestSplatCells> splats)
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
    session->SplatReach = splats ? SplatReach(*splats) : 0.0f;
    if (splats)
        if (auto coarse = CoarsenForestSplatCells(*splats))
        {
            session->CoarseSplatReach = SplatReach(*coarse) + coarse->CellSize * kSplatJitterRadius;
            session->CoarseSplats = std::make_shared<const ForestSplatCells>(std::move(*coarse));
        }
    session->Splats = std::move(splats);
    session->Scope = "Forest.Detail." + std::to_string(++m_NextSession);
    for (uint32 cascade = 0; cascade < kMaxDepthEmitCascades; ++cascade)
        session->ShadowScopes[cascade] = session->Scope + ".Shadow" + std::to_string(cascade);
    m_Sessions.push_back(session);
    return session;
}

bool ForestPlacementFeature::WantsPageOcclusion(uint64 world) const
{
    return std::ranges::any_of(m_Sessions, [world](const auto& weak) {
        const auto session = weak.lock();
        return session && session->World == world && session->DrawsColor && !session->Refused &&
               FitsPageOcclusionBounds(*session);
    });
}

bool ForestPlacementFeature::WantsCard(uint64 world, const GUID& model) const
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

float ForestCardReach(const ForestImpostorCard& card)
{
    const Mathematics::Vector3 center{card.Bounds.x, card.Bounds.y, card.Bounds.z};
    return center.Length() + card.Bounds.w * std::numbers::sqrt2_v<float> + card.Sway.MaxDisplacement;
}

namespace
{
void RefuseCard(ForestPlacementSession& session, float reach)
{
    session.Reset(false);
    session.Refused = true;
    const auto& store = session.Placements->Store();
    LOG_WARNING("Forest: {} was cooked with a bound radius of {:.2f} m, but the tree reaches {:.2f} m from its root. "
                "The forest draws its overview mesh only. Cook the archive again with a radius of at least {}.",
                store.File().string(), store.BoundRadius(), reach, std::ceil(reach));
}
} // namespace

void ForestPlacementFeature::PublishCard(uint64 world, const GUID& model, const ForestImpostorCard& card,
                                        std::span<const ImpostorBakePart> sources)
{
    const float reach = ForestCardReach(card);
    for (const auto& entry : m_Sessions)
    {
        const auto session = entry.lock();
        if (!session || session->World != world || session->Model != model || session->Refused)
            continue;
        if (reach > session->Placements->Store().BoundRadius())
        {
            RefuseCard(*session, reach);
            continue;
        }
        if (!session->WantsCard)
            continue;
        session->Card = card;
        if (session->NearBudget == 0 && session->ShadowMeshBudget == 0)
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
                            const ForestPlacementCull::DrawBuffers& buffers, const ForestNearDraw* near)
{
    static const bool enabled = Platform::EnvironmentSwitchEnabled("GE_FOREST_STATS", false);
    if (!enabled)
        return;

    auto& sample = session.CountSample;
    if (sample.Ticket)
    {
        const auto readCount = [](const auto& ticket, std::optional<uint32>& count)
        {
            if (!ticket || count)
                return;
            BufferReadbackResult result{};
            uint32 value = 0;
            if (ticket->TryGet(result) && result.bytes.size() == sizeof(value))
            {
                std::memcpy(&value, result.bytes.data(), sizeof(value));
                count = value;
            }
        };
        readCount(sample.Ticket, sample.FineCount);
        readCount(sample.CoarseTicket, sample.CoarseCount);
        if (sample.FineCount && sample.CoarseCount)
        {
            LOG_INFO("Forest.PlacedCount view={} frame={} standPages={} detailPages={} detailPlacements={} "
                     "cards={} meshes={} meshBatches={} coarseSplats={}", sample.View, sample.Frame, sample.StandPages,
                     sample.DetailPages, sample.DetailPlacements, *sample.FineCount + *sample.CoarseCount,
                     sample.Meshes, sample.MeshBatches, *sample.CoarseCount);
        }
        if (sample.Ticket->IsConsumed() && (!sample.CoarseTicket || sample.CoarseTicket->IsConsumed()))
            sample = {};
    }

    auto& frame = *context.Frame;
    if (sample.Ticket || !frame.ProfilingEnabled())
        return;

    auto ticket = RequestBufferReadbackRG(context.Device, frame, buffers.Args,
        offsetof(ForestPlacementIndirectDraw, InstanceCount), sizeof(ForestPlacementIndirectDraw::InstanceCount),
        "Forest.PlacedCount");
    if (!ticket)
        return;
    auto coarseTicket = buffers.CoarseArgs.IsValid() ? RequestBufferReadbackRG(context.Device, frame, buffers.CoarseArgs,
        offsetof(ForestPlacementIndirectDraw, InstanceCount), sizeof(ForestPlacementIndirectDraw::InstanceCount),
        "Forest.CoarsePlacedCount") : nullptr;
    if (buffers.CoarseArgs.IsValid() && !coarseTicket)
        return;

    uint32 placements = 0;
    for (const auto& page : session.Plan.DetailedDraws)
        placements += page.PlacementCount;
    const auto pages = static_cast<uint32>(session.Plan.DetailedDraws.size());
    const bool hasCoarse = coarseTicket != nullptr;
    sample = {std::move(ticket), frame.FrameIndex(), context.ViewId, session.Plan.VisiblePages - pages, pages,
              placements, near ? static_cast<uint32>(near->Owners.size()) : 0u,
              near ? static_cast<uint32>(near->Commands.size()) : 0u, std::move(coarseTicket), std::nullopt,
              hasCoarse ? std::nullopt : std::optional<uint32>(0u)};
}

struct ForestSourcePart
{
    const MeshGPUEntry* Mesh;
    const GPUMesh* Lod;
    const Material* Surface;
    MeshGPUHandle Handle;
    VertexAttributeFlags Attributes;
};

// The drawable source parts of the tree, with the card entry coverage and the deformation margin
// of their materials. Call inside the registry's table scope.
struct ForestNearParts
{
    std::vector<ForestSourcePart> Parts;
    float Cutoff = 0.0f;
    float DeformationMargin = 0.0f;
};

std::optional<ForestNearParts> ResolveNearParts(const ForestPlacementSession& session, RenderServices& services,
    const MeshGPURegistry& registry, const RenderServices::ViewSseScales& scales, ViewId viewId)
{
    const auto& card = *session.Card;
    const auto& meshRows = services.GetGPUScene()->GetMeshes();
    const auto deformationMargins = services.Materials().MaterialDeformationMarginSpan();
    ForestNearParts near;
    near.Cutoff = card.SwitchCoverage > 0.0f ? card.SwitchCoverage : ImpostorStage::DefaultSwitchCoverage(services, viewId);
    near.Parts.reserve(session.SourceParts.size());
    for (const auto& source : session.SourceParts)
    {
        const auto* mesh = registry.Find(source.Mesh);
        const auto* surface = services.Materials().Registry().Find(source.Material);
        MeshGPUEntryBindings bindings{};
        if (!mesh || !surface || mesh->gpuMeshIndex >= meshRows.size() || mesh->indexCount == 0 ||
            surface->GetGpuSceneMaterialIndex() == 0xFFFFFFFFu || IsSkinned(mesh->vertexFlags) ||
            !registry.TryGetDrawableBindings(*mesh, bindings))
            return std::nullopt;
        const auto& lod = meshRows[mesh->gpuMeshIndex];
        near.Cutoff = std::min(near.Cutoff, ForestPartCardEntryCoverage(lod, scales.Default, scales.Tight));
        const uint32 materialIndex = surface->GetGpuSceneMaterialIndex();
        if (materialIndex < deformationMargins.size())
            near.DeformationMargin = std::max(near.DeformationMargin, deformationMargins[materialIndex]);
        near.Parts.push_back({mesh, &lod, surface, source.Mesh,
                              BoundStreamVertexFlags(*mesh, bindings, surface->IgnoresVertexColor())});
    }
    return near;
}

// True when every part's color variant for the near-mesh pass is ready to record.
bool WarmNearParts(RenderServices& services, std::span<const ForestSourcePart> parts, MaterialKeyword pass,
                   const PipelineFormatKey& format)
{
    bool warm = true;
    for (const auto& part : parts)
        warm = services.Materials().Variants().WarmColorVariant(*part.Surface, part.Attributes,
            part.Mesh->topology, pass | kForestMeshKeywords, services.GetFrontFace(), format) && warm;
    return warm;
}

// Writes one draw per part and LOD, and each tree's transform into that draw's range of the upload.
bool WriteNearCommands(RGFrame& frame, ForestNearDraw& draw, std::span<const ForestSourcePart> parts,
                       std::span<const ForestMeshTransform> treeTransforms, std::span<const float> coverages,
                       const RenderServices::ViewSseScales& scales, uint32 force, DrawProducerId producer, std::byte* transforms)
{
    std::vector<uint32> treeLods(treeTransforms.size());
    uint32 firstTransform = 0;
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
                return false;
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
            command.Bindings.Buffers = std::span(&draw.Buffer, 1u);
            command.Producer = producer;
            draw.Commands.push_back(command);
        }
        // Write each LOD range directly to the upload ring in candidate order.
        for (size_t tree = 0; tree < treeTransforms.size(); ++tree)
        {
            const uint32 offset = writeOffsets[treeLods[tree]]++;
            std::memcpy(transforms + size_t{offset} * sizeof(ForestMeshTransform),
                        &treeTransforms[tree], sizeof(ForestMeshTransform));
        }
    }
    return true;
}

ForestNearDraw* BuildNearDraw(RGFrame& frame, std::span<const ForestSourcePart> parts,
    std::span<const ForestNearMeshCandidate> demand, const RenderServices::ViewSseScales& scales,
    uint32 force, DrawProducerId producer)
{
    auto* draw = frame.AllocatePassData<ForestNearDraw>();
    std::vector<ForestMeshTransform> treeTransforms;
    std::vector<float> coverages;
    treeTransforms.reserve(demand.size());
    coverages.reserve(demand.size());
    draw->Owners.reserve(demand.size());
    for (const auto& tree : demand)
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
    if (!WriteNearCommands(frame, *draw, parts, treeTransforms, coverages, scales, force, producer,
                           static_cast<std::byte*>(upload.Ptr)))
        return nullptr;
    return draw;
}

ForestNearDraw* PrepareNearDraw(ForestPlacementSession& session, const ForwardEmitContext& context,
    const CameraData& camera, const ForestPlacementView& view, MaterialKeyword pass, const PipelineFormatKey& format)
{
    RenderServices& services = *context.Services;
    const bool orthographic = camera.cameraPos.w == 1.0f;
    if (session.NearBudget == 0 || session.SourceParts.empty() || !services.GetGPUScene() || orthographic)
        return nullptr;

    const uint32 force = services.ResolveViewForceLOD(context.ViewId);
    const auto scales = services.ComputeViewSseScales(context.ViewId);
    const auto& registry = services.GetMeshGPURegistry();
    const MeshGPURegistry::TableScope scope(registry);
    auto near = ResolveNearParts(session, services, registry, scales, context.ViewId);
    if (!near)
        return nullptr;
    if (force != 0xFFFFFFFFu || registry.GetLodSelectionMode() == LodSelectionMode::Off)
        near->Cutoff = 0.0f;

    const ForestNearMeshView nearView{view, std::abs(camera.proj[1][1]), services.GetLODGlobalBias(),
                                    near->Cutoff, near->DeformationMargin};
    const std::span<const ForestMeshOwnedPlacement> previous = session.NearOwnerView == context.ViewId
        ? std::span<const ForestMeshOwnedPlacement>(session.NearOwners) : std::span<const ForestMeshOwnedPlacement>{};
    CollectForestNearMeshCandidates(*session.Placements, session.Plan.DetailedDraws, nearView, session.Card->Bounds,
                                    session.NearBudget, session.NearDemand, previous);
    if (session.NearDemand.empty() || !WarmNearParts(services, near->Parts, pass, format))
        return nullptr;

    static const auto producer = DrawProducerRegistry::Register("ForestNearMeshes");
    return BuildNearDraw(*context.Frame, near->Parts, session.NearDemand, scales, force, producer);
}

// Emits the near meshes and records the placements they own for the frame.
void EmitNearMeshes(ForestPlacementSession& session, const ForwardEmitContext& context, const ForestNearDraw& near)
{
    auto& services = *context.Services;
    session.PendingNearOwners.assign(near.Owners.begin(), near.Owners.end());
    std::ranges::sort(session.PendingNearOwners);
    services.EmitForwardSampledBufferRead(*context.Frame, context.ViewId, near.Upload, RGBufferRead::Storage,
                                         ForwardBufferReaders::WorldPass);
    for (const auto& command : near.Commands)
        services.EmitForwardCommand(context.ViewId, command, ForwardDrawDepth::ColourPass);
    if (!session.EmittedNearMeshes)
    {
        session.EmittedNearMeshes = true;
        LOG_INFO("Forest: registered {} near mesh placements in {} batches", near.Owners.size(), near.Commands.size());
    }
}

// True when a Game view or a Scene View may draw the session's details in app frame `appFrame`. The
// Game view comes first while the world simulates, else the Scene View. The other view may draw when
// the first view did not ask in this app frame or the one before. The app frame counts across windows.
bool TakesDetailView(ForestPlacementSession& session, ViewPurpose purpose, uint64 appFrame, bool simulating)
{
    if (purpose != ViewPurpose::Game && purpose != ViewPurpose::EditorScene)
        return false;
    const bool game = purpose == ViewPurpose::Game;
    (game ? session.GameViewFrame : session.SceneViewFrame) = appFrame;
    const std::optional<uint64>& other = game ? session.SceneViewFrame : session.GameViewFrame;
    return game == simulating || !other || *other + 1 < appFrame;
}

// True when the session may declare into `frame`. A session streams into one graph. It moves to the
// graph of another window only when the frame it declared into has finished. The move frees its stream
// and buffers and keeps its card.
bool BindGraph(ForestPlacementSession& session, const RGFrame& frame)
{
    if (!session.Graph || session.Graph == &frame)
        return true;
    if (session.Declared.Frame)
        return false;
    auto card = std::move(session.Card);
    session.Reset(false);
    session.Card = std::move(card);
    return true;
}

// Streams the pages that the view and the cascades plan. False when the session draws no details this frame.
bool PlanDetails(ForestPlacementSession& session, RGFrame& frame, IDevice& device, ViewId viewId,
                 const ForestPlacementView& view, std::span<const ForestShadowPage> shadowPages)
{
    if (session.NearBudget != 0)
    {
        session.PendingNearFrame.Stamp(frame);
        session.PendingNearView = viewId;
        session.PendingNearOwners.clear();
    }

    // The cascades need the card too when the camera wants no trees.
    const bool shadowDemand = !shadowPages.empty();
    if (!session.Stream)
    {
        // The MeshImpostor system publishes or refuses the card first: a session without one streams nothing.
        session.WantsCard = session.Placements->CanPrefetchDetails(view) || shadowDemand;
        if (!session.WantsCard || !session.Card)
            return false;
    }
    if (!session.EnsureGpu(device))
        return false;
    if (!session.Stream->Prepare(*session.Assets, device, frame, view, session.Plan, shadowPages))
        return false;
    session.Graph = &frame;
    session.Declared.Stamp(frame);
    session.WantsCard = session.Plan.WantsDetails || shadowDemand;
    return true;
}

// Plans the session's frame once, for the first view that takes the details. A later call for the
// same view and frame returns the stored result; a call for another view returns false.
bool PlanFrame(ForestPlacementSession& session, RenderServices& services, RGFrame& frame, IDevice& device,
               ViewId viewId, ForestPlacementView& view, std::span<const ForestShadowPage> shadowPages)
{
    view.DetailDistance = session.DetailDistance;
    view.HandoffWidth = session.Splats ? std::min(ForestPlacementFeature::kSplatHandoffWidth, session.DetailDistance) : 0.0f;
    const auto* desc = services.Views().FindViewDesc(viewId);
    if (!desc || desc->worldPaused || !ImpostorStage::IsEnabled() ||
        !TakesDetailView(session, desc->purpose, services.Spine().WorldFrameEpoch(),
                         services.Views().IsWorldSimulating(session.World)))
        return false;
    if (session.Planned.IsFor(frame))
        return session.PlannedView == viewId && session.PlanReady;
    if (!BindGraph(session, frame))
        return false;
    session.Planned.Stamp(frame);
    session.PlannedView = viewId;
    session.ColorDeclared = false;
    session.ShadowsDeclared = false;
    session.DetailPlanEpoch.reset();
    session.PlanReady = PlanDetails(session, frame, device, viewId, view, shadowPages);
    if (session.PlanReady && session.DrawsColor)
    {
        session.DetailPlanEpoch = services.Spine().WorldFrameEpoch();
        session.DetailPlanView = desc->purpose;
    }
    return session.PlanReady;
}

// The splat material, shared by every stand. It registers on first use and compiles off the main thread.
const Material* EnsureSplatMaterial(RenderServices& services)
{
    static const GUID kSplatMaterial = GUID::Derive(GUID{}, "forest-splats");
    if (const Material* material = services.Materials().Registry().Find(kSplatMaterial))
        return material;
    MaterialDocument doc{};
    doc.schemaVersion = 3;
    doc.materialName = "Forest splats";
    doc.lightingModel = "StandardPBR";
    doc.surfaceShader = "Surfaces/forest_splat.glsl";
    doc.vertexModifier = "VertexModifiers/forest_splats.glsl";
    doc.customVertexShader = true;
    doc.doubleSided = true;
    doc.alphaMode = MaterialAlphaMode::Mask;
    doc.properties["alphaCutoff"] = 0.5f;
    return services.Materials().RegisterMaterialFromDocument(kSplatMaterial, doc, MaterialKeyword::None,
                                                             MaterialSystem::BaseCompileMode::Async);
}

// The farthest point of a drawn tree from its root at unit scale, wind included: the cull radius.
float DrawReach(const ForestPlacementSession& session, const ForestImpostorCard& card)
{
    if (!session.Splats)
        return ForestCardReach(card);
    return session.SplatReach + session.Splats->CellSize * kSplatJitterRadius +
           std::max(kSplatMinimumSway, card.Sway.MaxDisplacement);
}

// The wind that the splat cull resolves for each visible tree.
void ResolveSplatWind(RenderServices& services, ViewId viewId, uint64 worldId, const CameraData& camera,
                      const ForestImpostorCard& card, ForestSplatWindGpu& wind)
{
    ForwardLightUBO light{};
    services.PackViewWeather(viewId, worldId, light);
    std::copy_n(camera.renderOriginSector, wind.Sector.size(), wind.Sector.begin());
    wind.TimeSeconds = services.TemporalHistory().ResolveDeformationClock(Time::GetCumulativeSeconds()).TimeSeconds;
    wind.MaxDisplacement = std::max(kSplatMinimumSway, card.Sway.MaxDisplacement);
    wind.Weather = light.WeatherWind;
}

const ForestNearDraw* PrepareNearShadowMeshes(
    ForestPlacementSession& session, const DepthEmitContext& context, uint32 cascade, ForestPlacementView view)
{
    auto& services = *context.Services;
    const auto& cascades = *context.Cascades;
    const float halfExtent = cascades.Cascades[cascade].OrthoHalfExtent;
    if (session.ShadowMeshBudget == 0 || session.SourceParts.empty() || !services.GetGPUScene() ||
        halfExtent <= 0.0f || cascades.Resolution == 0)
        return {};

    const auto& registry = services.GetMeshGPURegistry();
    const MeshGPURegistry::TableScope scope(registry);
    const auto scales = services.ComputeViewSseScales(context.ViewId);
    auto near = ResolveNearParts(session, services, registry, scales, context.ViewId);
    if (!near)
        return {};

    // A sphere of radius r spans r / halfExtent of this orthographic map.
    // Keep the card where the atlas frame holds as many pixels as that footprint.
    const float frameCoverage = float(ImpostorAtlasKey{}.FrameSize) / float(cascades.Resolution);
    const ForestNearMeshView shadowView{view, 1.0f / halfExtent, 0.0f, frameCoverage, near->DeformationMargin, true};
    CollectForestNearMeshCandidates(*session.Placements, session.CascadeDraws, shadowView, session.Card->Bounds,
                                    session.ShadowMeshBudget, session.NearDemand);
    if (session.NearDemand.empty())
        return {};

    bool ready = true;
    for (const auto& part : near->Parts)
    {
        const auto keywords = part.Surface->GetVariantKey().materialKeywords;
        if (FilterDepthPassMaterial(DepthPassType::ShadowCascade, part.Surface->GetAlphaMode(),
                                    HasKeyword(keywords, MaterialKeyword::Transmission)) != DepthPassMaterialFilter::Draws)
            return {};
        const auto head = ChooseContributorDepthHead(*part.Surface, DepthPassType::ShadowCascade, kForestMeshKeywords,
                                                     StrippedDepthVertexFlags(part.Mesh->vertexFlags), part.Attributes);
        ready = services.Materials().Variants().WarmCascadeVariant(*part.Surface, head.VertexFlags,
            part.Mesh->topology, head.Head.Keywords, services.GetFrontFace()) && ready;
        if (head.ReadsTextures && !services.Textures().IsMaterialTextureBindingComplete(part.Surface->GetGuid()))
            ready = false;
    }
    if (!ready)
        return {};

    auto& frame = *context.Frame;
    const uint32 force = services.ResolveViewForceLOD(context.ViewId);
    static const auto producer = DrawProducerRegistry::Register("ForestNearShadowMeshes");
    return BuildNearDraw(frame, near->Parts, session.NearDemand, scales, force, producer);
}
} // namespace

// Culls the cascade's resident placements to its caster planes and emits their light-facing cards.
bool ForestPlacementFeature::EmitCascadeCasters(ForestPlacementSession& session,
    const DepthEmitContext& context, uint32 cascade, const Material& material, ForestPlacementView view)
{
    const auto bit = static_cast<uint8>(1u << cascade);
    session.CascadeDraws.clear();
    for (const auto& draw : session.Plan.ShadowDraws)
        if ((draw.CascadeMask & bit) != 0)
            session.CascadeDraws.push_back(draw.Draw);
    if (session.CascadeDraws.empty())
        return false;

    const auto& cascades = *context.Cascades;
    const auto origin = Mathematics::WorldPosition::FromSector(cascades.RenderOriginSector, {});
    const auto eyeOffset = view.Eye - origin;
    view.EyeRelativePlanes = cascades.Cascades[cascade].CasterPlanes;
    for (auto& plane : view.EyeRelativePlanes)
        plane.w += plane.x * eyeOffset.x + plane.y * eyeOffset.y + plane.z * eyeOffset.z;
    const auto* nearDraw = PrepareNearShadowMeshes(session, context, cascade, view);
    const std::span<const ForestMeshOwnedPlacement> owners = nearDraw ? nearDraw->Owners : std::span<const ForestMeshOwnedPlacement>{};

    const auto& card = *session.Card;
    const uint32 slotCount = session.Placements->SlotCount();
    ForestPlacementGpuView gpuView{};
    if (!BuildForestPlacementGpuInputs(*session.Placements, session.CascadeDraws,
            MakeForestShadowGpuViewSource(*context.Cascades, cascade), slotCount, ForestCardReach(card),
            card.Bounds, gpuView, session.Pages, owners))
        return false;
    const auto buffers = session.ShadowCulls[cascade].Declare(*context.Frame, session.ShadowScopes[cascade],
                                                              session.Slots, slotCount, gpuView, session.Pages, 6u, nullptr);
    if (!buffers || !EmitForestImpostorShadowDraw(context, cascade, *buffers, session.Indices, card.Sway,
                                                   DrawCommand{.Material = &material}))
        return false;
    // Commit mesh replacements only once their complementary card draw has accepted the page.
    if (nearDraw)
    {
        context.Services->EmitCascadeDepthBufferRead(*context.Frame, context.ViewId, cascade, nearDraw->Upload, RGBufferRead::Storage);
        for (const auto& draw : nearDraw->Commands)
            context.Services->EmitCascadeDepthCommand(context.ViewId, cascade, draw);
        if (!session.EmittedNearShadows)
        {
            session.EmittedNearShadows = true;
            LOG_INFO("Forest: registered {} near mesh shadow casters in cascade {}", nearDraw->Owners.size(), cascade);
        }
    }
    return true;
}

bool ForestPlacementFeature::PlanShadowFrame(ForestPlacementSession& session, const DepthEmitContext& context,
    ForestPlacementView view, std::span<const ForestShadowPage> shadowPages)
{
    if (session.Refused || !context.Frame || !context.Device || !context.Services)
        return false;
    if (!PlanFrame(session, *context.Services, *context.Frame, *context.Device, context.ViewId, view, shadowPages) ||
        session.ShadowsDeclared)
        return false;
    session.ShadowsDeclared = true;
    return true;
}

bool ForestPlacementFeature::EmitDetails(ForestPlacementSession& session, const ForwardEmitContext& context,
    const CameraData& camera, ForestPlacementView view, std::vector<DrawSubCommand>& standRanges,
    std::vector<ForestStandFade>& standFades)
{
    if (session.Refused)
        return false;
    auto& frame = *context.Frame;
    auto& services = *context.Services;
    if (!context.Device || !PlanFrame(session, services, frame, *context.Device, context.ViewId, view, {}) ||
        session.ColorDeclared)
        return false;
    session.ColorDeclared = true;
    if (session.Plan.DetailedDraws.empty())
    {
        standRanges.swap(session.Plan.StandDraws);
        standFades.swap(session.Plan.StandFades);
        return true;
    }
    const auto& card = session.Card;
    if (!card)
        return false;

    // The keywords and formats are the view's previous frame: on the frame they change, the material
    // warms the old variant, the record skips it, and the near forest comes back a frame later.
    const auto pass = services.Views().GetViewDeclaredWorldPassKeywords(context.ViewId);
    const auto format = services.Views().GetViewWorldColorFormatKey(context.ViewId);
    const Material* material = session.Splats ? EnsureSplatMaterial(services) :
        services.Materials().Registry().Find(card->Material);
    if (!material || !pass || format.ColorCount == 0)
        return false;
    const auto fadeKeyword = session.Splats ? MaterialKeyword::LodCrossfade : MaterialKeyword::None;
    if (!services.Materials().Variants().WarmColorVariant(*material, VertexAttributeFlags::None,
            PrimitiveTopology::TriangleList, (*pass & ~MaterialKeyword::Instanced) | fadeKeyword,
            services.GetFrontFace(), format))
        return false;

    const ForestNearDraw* near = PrepareNearDraw(session, context, camera, view, *pass, format);
    const std::span<const ForestMeshOwnedPlacement> owners = near ? std::span(near->Owners) :
        std::span<const ForestMeshOwnedPlacement>{};
    const auto source = MakeForestPlacementGpuViewSource(camera);
    ForestSplatWindGpu wind{};
    uint32 coarseIndexCount = 0;
    float radius = DrawReach(session, *card);
    if (session.Splats)
    {
        ResolveSplatWind(services, context.ViewId, session.World, camera, *card, wind);
        const float coarseRadius = session.CoarseSplatReach + wind.MaxDisplacement;
        const uint32 height = services.Views().GetViewWorldViewportHeight(context.ViewId);
        if (session.CoarseSplats && session.CoarseSplatCells.IsValid() && session.CoarseSplatIndices.IsValid() &&
            height != 0 && coarseRadius <= session.Placements->Store().BoundRadius() &&
            services.ResolveViewForceLOD(context.ViewId) == 0xFFFFFFFFu &&
            services.GetMeshGPURegistry().GetLodSelectionMode() != LodSelectionMode::Off)
        {
            constexpr float kBudgetPixels = 1.0f;
            const float pixelsPerUnit = 0.5f * static_cast<float>(height) * std::abs(camera.proj[1][1]) *
                std::exp2(services.GetLODGlobalBias());
            if (std::isfinite(pixelsPerUnit) && pixelsPerUnit > 0.0f)
            {
                wind.Lod = {session.Splats->CellSize, pixelsPerUnit, kBudgetPixels, camera.cameraPos.w};
                coarseIndexCount = static_cast<uint32>(session.CoarseSplats->Cells.size()) * 6u;
                radius = std::max(radius, coarseRadius);
            }
        }
    }
    ForestPlacementGpuView gpuView{};
    if (!source || !BuildForestPlacementGpuInputs(*session.Placements, session.Plan.DetailedDraws, *source,
            session.Placements->SlotCount(), radius, card->Bounds, gpuView, session.Pages, owners))
        return false;
    const auto occlusion = FitsPageOcclusionBounds(session) ? context.ForestOcclusionHZB : RGTexture{};
    const auto buffers = session.Cull.Declare(frame, session.Scope, session.Slots, session.Placements->SlotCount(),
                                              gpuView, session.Pages, session.TreeIndexCount(),
                                              session.Splats ? &wind : nullptr, occlusion,
                                              services.Textures().GetSampler(SamplerPreset::PointClamp),
                                              coarseIndexCount);
    if (!buffers)
        return false;
    const bool drawn = session.Splats
        ? EmitForestSplatDraw(context, *buffers, {session.Splats.get(), session.SplatCells, session.SplatIndices},
                              DrawCommand{.Material = material, .PassKeywords = fadeKeyword},
                              {session.CoarseSplats.get(), session.CoarseSplatCells, session.CoarseSplatIndices})
        : EmitForestImpostorDraw(context, *buffers, session.Indices, card->Sway, DrawCommand{.Material = material});
    if (!drawn)
        return false;
    if (near)
        EmitNearMeshes(session, context, *near);
    ObservePlacementCounts(session, context, *buffers, near);
    if (!session.EmittedDetails)
    {
        session.EmittedDetails = true;
        LOG_INFO("Forest: registered packed {} draw with {} detail pages", session.Splats ? "splat" : "impostor",
                 session.Plan.DetailedDraws.size());
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
            if (session->Declared.IsFor(frame))
                session->Declared = {};
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

ForestPlacementFeature::~ForestPlacementFeature()
{
    // The feature dies in RenderServices::Shutdown while the device lives. A session that a stand
    // still holds frees its GPU objects now and forgets the device.
    for (const auto& entry : m_Sessions)
        if (const auto session = entry.lock())
        {
            session->Reset(false);
            session->Device = nullptr;
        }
}

void ForestPlacementFeature::OnDeviceRebuilt(IDevice*)
{
    for (const auto& entry : m_Sessions)
        if (const auto session = entry.lock())
            session->Reset(true);
}

} // namespace GameEngine::Engine::Renderer
