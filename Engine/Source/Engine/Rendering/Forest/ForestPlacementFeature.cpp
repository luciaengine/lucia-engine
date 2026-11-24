#include "ForestPlacementFeature.h"

#include "Assets/AssetManager.h"
#include "ForestImpostorDraw.h"
#include "ForestNearMeshCandidates.h"
#include "ForestPlacementGpuData.h"
#include "ForestPlacementSession.h"
#include "Engine/Rendering/FrameOrchestrator.h"
#include "Engine/Rendering/Impostors/ImpostorBaker.h"
#include "Engine/Rendering/Impostors/ImpostorStage.h"
#include "Engine/Rendering/MeshLODThresholds.h"
#include "Engine/Rendering/RenderServices.h"
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

namespace
{
// The farthest point of the card from the tree root at unit scale: its corner and its largest sway.
float CardReach(const ForestImpostorCard& card)
{
    const Mathematics::Vector3 center{card.Bounds.x, card.Bounds.y, card.Bounds.z};
    return center.Length() + card.Bounds.w * std::numbers::sqrt2_v<float> + card.Sway.MaxDisplacement;
}

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
    const float reach = CardReach(card);
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
                            const ForestPlacementCull::DrawBuffers& buffers, const ForestNearDraw* near)
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

    auto ticket = RequestBufferReadbackRG(context.Device, frame, buffers.Args,
        offsetof(ForestPlacementIndirectDraw, InstanceCount), sizeof(ForestPlacementIndirectDraw::InstanceCount),
        "Forest.PlacedCount");
    if (!ticket)
        return;

    uint32 placements = 0;
    for (const auto& page : session.Plan.DetailedDraws)
        placements += page.PlacementCount;
    const auto pages = static_cast<uint32>(session.Plan.DetailedDraws.size());
    sample = {std::move(ticket), frame.FrameIndex(), context.ViewId, session.Plan.VisiblePages - pages, pages,
              placements, near ? static_cast<uint32>(near->Owners.size()) : 0u,
              near ? static_cast<uint32>(near->Commands.size()) : 0u};
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
                       const RenderServices::ViewSseScales& scales, uint32 force, std::byte* transforms)
{
    static const auto producer = DrawProducerRegistry::Register("ForestNearMeshes");
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
    CollectForestNearMeshCandidates(*session.Placements, session.Plan, nearView, session.Card->Bounds,
                                    session.NearBudget, session.NearDemand, previous);
    if (session.NearDemand.empty() || !WarmNearParts(services, near->Parts, pass, format))
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

    const auto bytes = treeTransforms.size() * near->Parts.size() * sizeof(ForestMeshTransform);
    const auto upload = frame.AllocUpload(bytes);
    if (!upload.Valid())
        return nullptr;
    draw->Upload = frame.ImportExternalBuffer("Forest.NearMeshTransforms", upload.Buffer);
    if (!draw->Upload.IsValid())
        return nullptr;
    draw->Buffer = {HashStringId("ForestMeshTransforms"), upload.Buffer, upload.Offset, bytes};
    if (!WriteNearCommands(frame, *draw, near->Parts, treeTransforms, coverages, scales, force,
                           static_cast<std::byte*>(upload.Ptr)))
        return nullptr;
    return draw;
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

// Streams the pages the view plans. False when the session draws no details this frame.
bool PlanDetails(ForestPlacementSession& session, const ForwardEmitContext& context, const ForestPlacementView& view)
{
    auto& frame = *context.Frame;
    if (session.NearBudget != 0)
    {
        session.PendingNearFrame.Stamp(frame);
        session.PendingNearView = context.ViewId;
        session.PendingNearOwners.clear();
    }

    if (!session.Stream)
    {
        // The MeshImpostor system publishes or refuses the card first: a session without one streams nothing.
        session.WantsCard = session.Placements->CanPrefetchDetails(view);
        if (!session.WantsCard || !session.Card)
            return false;
    }
    if (!session.EnsureGpu(*context.Device))
        return false;
    if (!session.Stream->Prepare(*session.Assets, *context.Device, frame, view, session.Plan))
        return false;
    session.Graph = &frame;
    session.Declared.Stamp(frame);
    session.WantsCard = session.Plan.WantsDetails;
    return true;
}

} // namespace

bool ForestPlacementFeature::EmitDetails(ForestPlacementSession& session, const ForwardEmitContext& context,
    const CameraData& camera, ForestPlacementView view, std::vector<DrawSubCommand>& standRanges)
{
    if (session.Refused)
        return false;
    auto& frame = *context.Frame;
    auto& services = *context.Services;
    const auto* desc = services.Views().FindViewDesc(context.ViewId);
    if (!context.Device || !desc || desc->worldPaused || !ImpostorStage::IsEnabled() ||
        !TakesDetailView(session, desc->purpose, services.Spine().WorldFrameEpoch(),
                         services.Views().IsWorldSimulating(session.World)) ||
        session.Declared.IsFor(frame) || !BindGraph(session, frame))
        return false;
    view.DetailDistance = session.DetailDistance;
    session.DetailPlanEpoch.reset();
    if (!PlanDetails(session, context, view))
        return false;
    session.DetailPlanEpoch = services.Spine().WorldFrameEpoch();
    session.DetailPlanView = desc->purpose;
    if (session.Plan.DetailedDraws.empty())
    {
        standRanges.swap(session.Plan.StandDraws);
        return true;
    }
    const auto& card = session.Card;
    if (!card)
        return false;

    // The keywords and formats are the view's previous frame: on the frame they change, the card
    // warms the old variant, the record skips it, and the near forest comes back a frame later.
    const Material* material = services.Materials().Registry().Find(card->Material);
    const auto pass = services.Views().GetViewDeclaredWorldPassKeywords(context.ViewId);
    const auto format = services.Views().GetViewWorldColorFormatKey(context.ViewId);
    if (!material || !pass || format.ColorCount == 0)
        return false;
    const auto keywords = *pass & ~MaterialKeyword::Instanced;
    if (!services.Materials().Variants().WarmColorVariant(*material, VertexAttributeFlags::None,
            PrimitiveTopology::TriangleList, keywords, services.GetFrontFace(), format))
        return false;

    const ForestNearDraw* near = PrepareNearDraw(session, context, camera, view, *pass, format);
    const std::span<const ForestMeshOwnedPlacement> owners = near ? std::span(near->Owners) :
        std::span<const ForestMeshOwnedPlacement>{};
    ForestPlacementGpuView gpuView{};
    if (!BuildForestPlacementGpuInputs(*session.Placements, session.Plan, camera, session.Placements->SlotCount(),
            CardReach(*card), card->Bounds, gpuView, session.Pages, owners))
        return false;
    const auto buffers = session.Cull.Declare(frame, session.Scope, session.Slots, session.Placements->SlotCount(),
                                              gpuView, session.Pages);
    if (!buffers || !EmitForestImpostorDraw(context, *buffers, session.Indices, card->Sway,
            DrawCommand{.Material = material}))
        return false;
    if (near)
        EmitNearMeshes(session, context, *near);
    ObservePlacementCounts(session, context, *buffers, near);
    if (!session.EmittedDetails)
    {
        session.EmittedDetails = true;
        LOG_INFO("Forest: registered packed impostor draw with {} detail pages", session.Plan.DetailedDraws.size());
    }
    standRanges.swap(session.Plan.StandDraws);
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
