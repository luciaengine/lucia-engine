#include "Engine/Rendering/VirtualShadows/VirtualShadowRenderFeature.h"

#include "Engine/Rendering/DepthProducerState.h"
#include "Engine/Rendering/PointShadowAtlasPlanner.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/ShadowCasterChanges.h"
#include "Engine/Rendering/ShadowFrameInfo.h"
#include "Engine/Rendering/VirtualShadows/VirtualShadowFeedback.h"
#include "Rendering/Camera/CameraDerivation.h"
#include "Rendering/Core/GPUCulling.h"
#include "Rendering/Core/RenderGraph/RGReadbackRing.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <string>

namespace GameEngine::Engine::Renderer
{
namespace RenderGraph = Rendering::RenderGraph;

struct VirtualShadowRenderFeature::ViewState
{
    static constexpr uint32_t kMaxPages =
        kVirtualShadowClipLevels * kVirtualLocalShadowPagesPerAxis * kVirtualLocalShadowPagesPerAxis +
        (kVirtualShadowMaxSpotProjections + kMaxPointShadowSlots * kPointShadowFaceCount) *
        ((4 * kVirtualLocalShadowPagesPerAxis * kVirtualLocalShadowPagesPerAxis - 1) / 3);
    static constexpr uint32_t kMaxWords = (kMaxPages + 31) / 32;

    struct Use
    {
        const RenderGraph::RGFrame* Frame = nullptr;
        uint64_t FrameIndex = 0;
        uint64_t Sequence = 0;
        Rendering::IDevice::GpuSyncToken Completion;
    };

    struct Certificate
    {
        VirtualShadowPageWork Work;
        DepthProducerState Producer;
    };

    struct FeedbackSnapshot
    {
        VirtualShadowFeedbackLayout Layout;
        uint64_t Sequence = 0;
    };

    explicit ViewState(uint64_t generation) : Generation(generation) {}

    uint64_t TrackUse(const RenderGraph::RGFrame& frame)
    {
        CancelStaleIncarnations(frame);
        for (const auto& use : Uses)
            if (use.Frame == &frame && use.FrameIndex == frame.FrameIndex())
                return use.Sequence;
        Uses.push_back({&frame, frame.FrameIndex(), ++Sequence, {}});
        return Sequence;
    }

    void RemoveCompletedUses(Rendering::IDevice& device)
    {
        std::erase_if(Uses, [&device](const Use& use)
        {
            return !use.Frame && device.QueryGpuSyncToken(use.Completion) ==
                                    Rendering::IDevice::GpuSyncStatus::Complete;
        });
    }

    void CancelUnsubmitted(const RenderGraph::RGFrame& frame, bool allIncarnations)
    {
        std::erase_if(Uses, [&frame, allIncarnations](const Use& use)
        {
            return use.Frame == &frame &&
                   (allIncarnations || use.FrameIndex == frame.FrameIndex());
        });
    }

    void Stamp(const RenderGraph::RGFrame& frame, const Rendering::IDevice::GpuSyncToken& token)
    {
        Feedback.OnFrameSubmitted(frame, token);
        CancelStaleIncarnations(frame);
        for (auto& use : Uses)
        {
            if (use.Frame != &frame || use.FrameIndex != frame.FrameIndex())
                continue;
            use.Frame = nullptr;
            use.Completion = token;
        }
    }

    void CancelStaleIncarnations(const RenderGraph::RGFrame& frame)
    {
        std::erase_if(Uses, [&frame](const Use& use)
        {
            return use.Frame == &frame && use.FrameIndex != frame.FrameIndex();
        });
    }

    uint64_t CompletedSequence() const
    {
        uint64_t completed = Sequence;
        for (const auto& use : Uses)
            completed = std::min(completed, use.Sequence - 1);
        return completed;
    }

    bool HasUnsubmittedUse(const RenderGraph::RGFrameStamp& frame) const
    {
        return frame.Frame && std::ranges::any_of(Uses, [&frame](const Use& use)
        {
            return use.Frame == frame.Frame && use.FrameIndex == frame.FrameIndex;
        });
    }

    uint64_t Generation;
    uint64_t AtlasGeneration = 0;
    uint64_t Sequence = 0;
    uint64_t CasterVersion = 0;
    uint64_t GeometryRevision = 0;
    uint64_t CapturedSequence = 0;
    Rendering::TextureHandle PhysicalAtlas{};
    RenderGraph::RGReadbackRing<FeedbackSnapshot> Feedback;
    VirtualShadowPageCache Cache{kVirtualShadowPhysicalTiles};
    VirtualShadowFeedbackLayout Previous;
    VirtualShadowFeedbackLayout Captured;
    std::vector<uint32_t> Words;
    std::vector<Use> Uses;
    std::array<std::optional<Certificate>, kVirtualShadowPhysicalTiles> Certificates;
    const RenderGraph::RGFrame* PlanFrame = nullptr;
    uint64_t PlanFrameIndex = 0;
    RasterPlan Plan;
};

VirtualShadowRenderFeature::VirtualShadowRenderFeature() = default;

VirtualShadowRenderFeature::~VirtualShadowRenderFeature()
{
    for (auto& [view, state] : m_Views)
        state->Feedback.Destroy(m_Device);
    for (auto& state : m_RetiredViews)
        state->Feedback.Destroy(m_Device);
}

void VirtualShadowRenderFeature::Initialize(Rendering::IDevice* device)
{
    if (!m_Device)
        m_Device = device;
}

bool VirtualShadowRenderFeature::Configure(const VirtualShadowOptions& options)
{
    if (!options.IsValid())
        return false;
    if (options == m_Options)
        return true;
    m_Options = options;
    for (auto& [view, state] : m_Views)
        state->Cache.InvalidateAll();
    return true;
}

bool VirtualShadowRenderFeature::IsEnabled() const
{
    return m_Options.Enabled || VirtualShadowFeedbackEnabled();
}

void VirtualShadowRenderFeature::OnScheduleCulling(const FeatureCullingContext& context)
{
    if (!IsEnabled() || !context.Services || !context.CullingPipeline ||
        !context.Views || !context.Cameras || context.Services->GetProfile().IsCompat())
        return;
    for (const auto& view : *context.Views)
    {
        if (view.cameraId == 0 || view.ActiveRenderLayerMask() == Rendering::RenderLayerMask::None() ||
            (view.purpose != Rendering::ViewPurpose::Game && view.purpose != Rendering::ViewPurpose::EditorScene) ||
            !context.Services->HasShadowCasters(view.id))
            continue;
        const auto camera = std::ranges::find(*context.Cameras, view.cameraId, &Rendering::CameraInfo::id);
        if (camera == context.Cameras->end())
            continue;
        const auto derived = Rendering::DeriveCameraData(camera->data, camera->ranges);
        Rendering::CascadeCullingGroup group{};
        group.viewId = view.id;
        group.renderLayerMask = view.ActiveRenderLayerMask();
        group.cascadeCount = 1;
        group.cascadeIndexBase = kVirtualShadowCasterCullingIndex;
        group.shadowCasterDispatch = true;
        group.dropEditorHidden = view.purpose == Rendering::ViewPurpose::EditorScene;
        group.originSector = Mathematics::Vector3Int{camera->data.renderOriginSector[0],
            camera->data.renderOriginSector[1], camera->data.renderOriginSector[2]};
        group.cameraForward = derived.Forward;
        group.nearPlane = derived.NearPlane;
        group.farPlane = derived.FarPlane;
        group.instanceCount = context.InstanceCount;
        group.frameIndex = context.FrameIndex;
        group.deltaTime = context.DeltaTime;
        // This all-caster slice proves resident pages and keeps off-camera casters animated.
        // A page that renders draws from its own slice, which DeclarePages culls to the page.
        for (auto& plane : group.frustumPlanes[0])
            plane = Mathematics::Vector4{0.0f, 0.0f, 0.0f, 1.0f};
        context.CullingPipeline->SubmitCascadeGroup(group);
    }
}

void VirtualShadowRenderFeature::OnDeviceRebuilt(Rendering::IDevice* device)
{
    // Device teardown already freed these buffers. Forget their old handles.
    for (auto& [view, state] : m_Views)
        state->Feedback.Destroy(nullptr);
    for (auto& state : m_RetiredViews)
        state->Feedback.Destroy(nullptr);
    m_Views.Clear();
    m_RetiredViews.clear();
    m_Device = device;
}

void VirtualShadowRenderFeature::CollectRetiredViews()
{
    if (!m_Device)
        return;
    std::erase_if(m_RetiredViews, [this](const std::unique_ptr<ViewState>& state)
    {
        state->RemoveCompletedUses(*m_Device);
        if (!state->Uses.empty())
            return false;
        state->Feedback.Destroy(m_Device);
        return true;
    });
}

void VirtualShadowRenderFeature::OnViewReleased(Rendering::ViewId view)
{
    const auto found = m_Views.Find(view);
    if (!found)
        return;
    // Recorded work may not have submitted yet. Its state still receives frame hooks.
    m_RetiredViews.push_back(std::move(*found));
    m_Views.Erase(view);
    CollectRetiredViews();
}

void VirtualShadowRenderFeature::OnFrameSubmittedRG(RenderGraph::RGFrame& frame,
                                                    const Rendering::IDevice::GpuSyncToken& token)
{
    for (auto& [view, state] : m_Views)
        state->Stamp(frame, token);
    for (auto& state : m_RetiredViews)
        state->Stamp(frame, token);
    CollectRetiredViews();
}

void VirtualShadowRenderFeature::OnFrameAbortedRG(RenderGraph::RGFrame& frame)
{
    for (auto& [view, state] : m_Views)
    {
        state->Feedback.CancelUnsubmitted(frame);
        state->CancelUnsubmitted(frame, false);
        if (state->PlanFrame == &frame && state->PlanFrameIndex == frame.FrameIndex())
        {
            state->PlanFrame = nullptr;
            state->Plan = {};
        }
    }
    for (auto& state : m_RetiredViews)
    {
        state->Feedback.CancelUnsubmitted(frame);
        state->CancelUnsubmitted(frame, false);
    }
    CollectRetiredViews();
}

void VirtualShadowRenderFeature::OnFrameStreamRetiredRG(RenderGraph::RGFrame& frame)
{
    for (auto& [view, state] : m_Views)
    {
        state->Feedback.OnFrameStreamRetired(frame);
        state->CancelUnsubmitted(frame, true);
        if (state->PlanFrame == &frame)
        {
            state->PlanFrame = nullptr;
            state->Plan = {};
        }
    }
    for (auto& state : m_RetiredViews)
    {
        state->Feedback.OnFrameStreamRetired(frame);
        state->CancelUnsubmitted(frame, true);
    }
    CollectRetiredViews();
}

VirtualShadowRenderFeature::ViewState* VirtualShadowRenderFeature::FindView(
    Rendering::ViewId view, const RasterPlan& plan)
{
    const auto found = m_Views.Find(view);
    if (!found || !(*found)->HasUnsubmittedUse(plan.For) || (*found)->Generation != plan.ViewGeneration ||
        (*found)->AtlasGeneration != plan.AtlasGeneration || (*found)->PhysicalAtlas != plan.PhysicalAtlas)
        return nullptr;
    return found->get();
}

bool VirtualShadowRenderFeature::IsCurrentPage(Rendering::ViewId view, const RasterPlan& plan,
                                               const VirtualShadowPageWork& work)
{
    auto* state = FindView(view, plan);
    return state && state->Cache.Contains(work);
}

bool VirtualShadowRenderFeature::MatchesLayout(Rendering::ViewId view, const RasterPlan& plan,
                                                const VirtualShadowFeedbackLayout& layout)
{
    const auto* state = FindView(view, plan);
    if (!state || state->Previous.World != layout.World ||
        state->Previous.RenderLayerMask != layout.RenderLayerMask ||
        state->Previous.WordCount != layout.WordCount || state->Previous.Projections.size() != layout.Projections.size())
        return false;
    for (size_t index = 0; index < layout.Projections.size(); ++index)
    {
        const auto& previous = state->Previous.Projections[index];
        const auto& current = layout.Projections[index];
        if (previous.BasePage != current.BasePage || previous.FirstPage != current.FirstPage ||
            !SameVirtualShadowProjection(previous, current))
            return false;
    }
    return true;
}

bool VirtualShadowRenderFeature::ValidatePage(Rendering::ViewId view, const RasterPlan& plan,
    const VirtualShadowPageWork& work, const DepthProducerState& producer)
{
    auto* state = FindView(view, plan);
    if (!state || work.NeedsRender || !state->Cache.Contains(work))
        return false;
    const auto& certificate = state->Certificates[work.Tile];
    if (!work.NeedsRender && producer.Complete && certificate &&
        certificate->Work.Page == work.Page && certificate->Work.Revision == work.Revision &&
        certificate->Producer == producer)
        return true;
    state->Cache.MarkDirty(work.Page);
    state->Certificates[work.Tile].reset();
    return false;
}

bool VirtualShadowRenderFeature::CertifyPage(Rendering::ViewId view, const RasterPlan& plan,
    const VirtualShadowPageWork& work, const DepthProducerState& producer, bool recorded)
{
    auto* state = FindView(view, plan);
    if (!state || !recorded || !producer.Complete || !state->Cache.MarkRendered(work))
        return false;
    state->Certificates[work.Tile] = ViewState::Certificate{work, producer};
    return true;
}

VirtualShadowRenderFeature::RasterPlan VirtualShadowRenderFeature::DeclareFeedback(
    RenderGraph::RGFrame& frame, Rendering::ViewId view, RenderGraph::RGBuffer requests,
    const bool* marked, const VirtualShadowFeedbackLayout& layout,
    const ShadowCasterChangeSet& casters, uint64_t geometryRevision, const char* passName)
{
    if (!m_Device || !requests.IsValid() || !marked || layout.WordCount == 0 ||
        layout.WordCount > ViewState::kMaxWords)
        return {};
    CollectRetiredViews();
    auto& entry = m_Views.GetOrInsert(view);
    if (!entry)
        entry = std::make_unique<ViewState>(m_NextViewGeneration++);
    auto& state = *entry;
    if (state.PlanFrame == &frame && state.PlanFrameIndex == frame.FrameIndex())
        return state.Plan;
    state.PlanFrame = &frame;
    state.PlanFrameIndex = frame.FrameIndex();
    state.Plan = {};

    if (!state.Feedback.IsInitialized())
    {
        Rendering::BufferDesc descriptor{};
        descriptor.size = ViewState::kMaxWords * sizeof(uint32_t);
        descriptor.usage = static_cast<uint32_t>(Rendering::BufferUsage::TransferDst);
        descriptor.memoryUsage = Rendering::BufferMemoryUsage::Readback;
        descriptor.debugName = "VirtualShadowFeedback";
        if (!state.Feedback.Init(m_Device, descriptor, std::max(1u, m_Device->GetFramesInFlight()) + 2u,
                                 RenderGraph::RGReadbackRetention::PreserveUnread))
            return {};
    }

    if (state.Previous.World != layout.World || state.Previous.RenderLayerMask != layout.RenderLayerMask ||
        state.GeometryRevision != geometryRevision)
    {
        state.Captured = {};
        state.Words.clear();
        state.Cache.InvalidateAll();
    }
    else if (state.CasterVersion != casters.Version)
    {
        if (casters.Version == state.CasterVersion + 1 && casters.Scope == ShadowCasterChangeScope::VertexDeformation)
            state.Cache.MarkDeformed();
        else if (casters.Version != state.CasterVersion + 1 || casters.Scope != ShadowCasterChangeScope::Bounded)
            state.Cache.InvalidateAll();
        else
            state.Cache.InvalidatePages([&layout, &casters](const VirtualShadowPage& page)
            {
                return std::ranges::any_of(layout.Projections, [&page, &casters](const auto& projection)
                {
                    return VirtualShadowPageTouchedByCasters(projection, page, casters.Spheres);
                });
            });
    }
    for (const auto& previous : state.Previous.Projections)
    {
        if (std::ranges::none_of(layout.Projections, [&previous](const auto& current)
            { return SameVirtualShadowProjection(previous, current); }))
            state.Cache.InvalidateProjection(previous.BasePage);
    }
    state.Previous = layout;
    state.CasterVersion = casters.Version;
    state.GeometryRevision = geometryRevision;

    const uint32_t maxResults = std::max(1u, m_Device->GetFramesInFlight()) + 2u;
    for (uint32_t result = 0; result < maxResults; ++result)
    {
        ViewState::FeedbackSnapshot captured;
        const auto* words = static_cast<const uint32_t*>(state.Feedback.MapOldestReady(m_Device, &captured));
        if (!words)
            break;
        if (captured.Sequence > state.CapturedSequence)
        {
            state.Words.assign(words, words + captured.Layout.WordCount);
            state.Captured = std::move(captured.Layout);
            state.CapturedSequence = captured.Sequence;
        }
        state.Feedback.Unmap(m_Device);
    }
    state.CancelStaleIncarnations(frame);
    state.RemoveCompletedUses(*m_Device);
    const auto demand = DecodeVirtualShadowFeedback(state.Words, state.Captured, layout);
    if (state.Uses.size() >= std::max(1u, m_Device->GetFramesInFlight()) + 2u)
        return {};
    const uint64_t completed = state.CompletedSequence();
    const uint64_t sequence = state.TrackUse(frame);
    if (!demand.empty())
    {
        Rendering::TextureDesc descriptor{};
        descriptor.width = descriptor.height = kVirtualShadowPageTexels;
        descriptor.arrayLayers = kVirtualShadowPhysicalTiles;
        descriptor.depth = descriptor.mipLevels = descriptor.sampleCount = 1;
        descriptor.format = static_cast<uint32_t>(Rendering::TextureFormat::D32_FLOAT);
        descriptor.usage = static_cast<uint32_t>(Rendering::TextureUsage::DepthStencil |
            Rendering::TextureUsage::ShaderResource | Rendering::TextureUsage::TransferSrc);
        descriptor.flags = Rendering::TextureCreateFlags::ForceArrayView;
        const std::string name = "VirtualShadowDepth.View" + std::to_string(view);
        descriptor.debugName = name.c_str();
        const auto atlas = frame.ImportPersistentTexture(name.c_str(), descriptor);
        const auto physical = atlas.IsValid() ? frame.PhysicalTexture(atlas) : Rendering::TextureHandle{};
        if (physical.IsValid())
        {
            if (state.PhysicalAtlas != physical)
            {
                state.Cache.InvalidateAll();
                state.PhysicalAtlas = physical;
                ++state.AtlasGeneration;
                state.Certificates = {};
            }
            const auto* work = frame.AllocatePassData<std::vector<VirtualShadowPageWork>>(
                state.Cache.Plan(demand, sequence, completed));
            frame.MarkOutput(atlas, RenderGraph::RGImageLayout::ShaderReadOnly);
            state.Plan = {*work, atlas, physical, state.Generation, state.AtlasGeneration, {}};
            state.Plan.For.Stamp(frame);
        }
    }

    uint64_t reservation = 0;
    const auto slot = state.Feedback.ReserveWrite(frame, ViewState::FeedbackSnapshot{layout, sequence}, reservation);
    if (!slot.IsValid())
        return state.Plan;
    const size_t bytes = layout.WordCount * sizeof(uint32_t);
    const auto destination = frame.ImportExternalBuffer(passName, slot, bytes);
    if (!destination.IsValid())
        return state.Plan;
    const uint64_t generation = state.Generation;
    auto* copied = frame.AllocatePassData<bool>(false);
    frame.AddPass(passName, Rendering::PassPhase::kDefault,
        [this, view, generation, requests, destination, slot, reservation, copied](RenderGraph::RGPassBuilder& builder)
        {
            builder.Read(requests, RenderGraph::RGBufferRead::CopySrc);
            builder.Write(destination, RenderGraph::RGBufferWrite::CopyDst);
            builder.PreventCulling();
            builder.OnSubmitted([this, view, generation, slot, reservation, copied]
            {
                const auto found = m_Views.Find(view);
                if (*copied && found && (*found)->Generation == generation)
                    (*found)->Feedback.MarkRecorded(slot, reservation);
            });
        },
        [requests, destination, bytes, marked, copied, feedback = &state.Feedback, slot]
        (RenderGraph::RGContext& context)
        {
            if (!*marked || !context.Cmd)
                return;
            const auto source = context.GetBuffer(requests);
            const auto target = context.GetBuffer(destination);
            if (!source.IsValid() || !target.IsValid())
                return;
            context.Cmd->CopyBuffer(source, target, bytes);
            feedback->RecordResolve(context.Cmd, slot);
            *copied = true;
        });
    return state.Plan;
}
} // namespace GameEngine::Engine::Renderer
