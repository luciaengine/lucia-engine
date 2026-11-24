#pragma once

#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Core/RenderGraph/RGGraph.h"

#include <algorithm>
#include <span>

namespace GameEngine::Rendering::RenderGraph
{

struct DepthPrepassLineage
{
    const char* Rejection = nullptr;
    RGPassId BlockingPass = kInvalidId;

    explicit operator bool() const { return Rejection == nullptr; }
};

// Call once per world pass after scheduling. A pool-owned persistent import
// shares the graph's deduplicated resource identity. The caller must establish
// that the attachment is that owned texture, without external texture views.
// This proves depth provenance. For packed depth/stencil a draw-level proof
// must also check that the served producer and consumer pipelines disable stencil.
inline DepthPrepassLineage CheckDepthPrepassLineage(const RGGraph& graph, const RGAttachmentRec& prepass,
                                                    const RGAttachmentRec& forward, bool verifiedPoolImport,
                                                    std::span<const RGAttachmentRec* const> continuations = {},
                                                    bool allowMultisampledDepth = false)
{
    const RGResourceId depth = prepass.Tex;
    if (depth >= graph.ResourceCount() || depth != forward.Tex ||
        !prepass.IsDepth || prepass.ReadOnly || !forward.IsDepth || !forward.ReadOnly ||
        prepass.Resolve != kInvalidId || forward.Resolve != kInvalidId ||
        prepass.Ops.Load != RGLoadOp::Clear || prepass.Ops.Store != RGStoreOp::Store ||
        prepass.Range.BaseMip != 0 || prepass.Range.BaseLayer != 0 ||
        forward.Range.BaseMip != 0 || forward.Range.BaseLayer != 0 ||
        forward.Ops.Load != RGLoadOp::Load || forward.Ops.Store != RGStoreOp::None)
        return {"attachment operations"};
    const auto& desc = graph.ResourceDesc(depth);
    const auto format = static_cast<TextureFormat>(desc.Format);
    const bool hasFloat32Depth =
        format == TextureFormat::D32_FLOAT || format == TextureFormat::D32_SFLOAT_S8_UINT;
    if (desc.Kind != RGResourceKind::Texture ||
        !hasFloat32Depth || (!allowMultisampledDepth && desc.SampleCount != 1) ||
        desc.ArrayLayers != 1 || desc.MipLevels != 1 ||
        (graph.IsImported(depth) ? !verifiedPoolImport : graph.IsExternal(depth)))
        return {"texture descriptor or provenance"};

    const auto& order = graph.ScheduledOrder();
    const auto from = std::find(order.begin(), order.end(), prepass.Pass);
    const auto to = std::find(order.begin(), order.end(), forward.Pass);
    if (from == order.end() || to == order.end() || from >= to)
        return {"schedule order"};
    if (graph.PassExecutionQueue(prepass.Pass) != RGQueue::Graphics ||
        graph.PassExecutionQueue(forward.Pass) != RGQueue::Graphics)
        return {"execution queue"};

    auto previous = from;
    for (const auto* continuation : continuations)
    {
        const auto position = std::find(previous + 1, to, continuation->Pass);
        if (continuation->Tex != depth || !continuation->IsDepth || continuation->ReadOnly ||
            continuation->Resolve != kInvalidId ||
            continuation->Range.BaseMip != 0 || continuation->Range.BaseLayer != 0 ||
            continuation->Ops.Load != RGLoadOp::Load || continuation->Ops.Store != RGStoreOp::Store ||
            position == to || graph.PassExecutionQueue(continuation->Pass) != RGQueue::Graphics)
            return {"invalid depth continuation"};
        const bool writesDepth = std::any_of(graph.Accesses().begin(), graph.Accesses().end(),
            [depth, continuation](const auto& access) {
                return access.Pass == continuation->Pass && access.Resource == depth &&
                       access.Access == RGAccess::DepthWrite;
            });
        if (!writesDepth)
            return {"missing continuation depth write", continuation->Pass};
        previous = position;
    }

    bool producerWritesDepth = false;
    bool consumerReadsDepth = false;
    for (const auto& access : graph.Accesses())
    {
        if (access.Resource != depth)
            continue;
        if (access.Pass == prepass.Pass)
        {
            if (IsWrite(access.Access) && access.Access != RGAccess::DepthWrite)
                return {"unexpected producer write", access.Pass};
            producerWritesDepth |= access.Access == RGAccess::DepthWrite;
        }
        else if (access.Pass == forward.Pass)
        {
            if (IsWrite(access.Access))
                return {"consumer writes depth", access.Pass};
            consumerReadsDepth |= access.Access == RGAccess::DepthRead;
        }
        else if (IsWrite(access.Access) && std::find(from + 1, to, access.Pass) != to)
        {
            const bool declaredContinuation = access.Access == RGAccess::DepthWrite &&
                std::any_of(continuations.begin(), continuations.end(), [&](const auto* continuation) {
                    return continuation->Pass == access.Pass;
                });
            if (!declaredContinuation)
                return {"intervening depth write", access.Pass};
        }
    }
    if (!producerWritesDepth || !consumerReadsDepth)
        return {"missing depth access"};
    return {};
}
}
