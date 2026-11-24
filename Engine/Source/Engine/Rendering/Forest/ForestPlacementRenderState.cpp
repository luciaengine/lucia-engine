#include "ForestPlacementRenderState.h"

#include "Rendering/Core/RenderGraph/RGFrame.h"

namespace GameEngine::Engine::Renderer
{

ForestPlacementRenderState::ForestPlacementRenderState(std::shared_ptr<ForestPlacementRuntime> placements,
    Rendering::BufferHandle destination, uint32 slots)
    : m_Placements(placements), m_Stream(placements), m_Uploader(std::move(placements), destination, slots)
{
}

ForestPlacementRenderState::~ForestPlacementRenderState()
{
    if (m_Placements)
        m_Placements->ResetResidency();
}

bool ForestPlacementRenderState::Prepare(AssetManager& assets, Rendering::IDevice& device,
    Rendering::RenderGraph::RGFrame& graph, const ForestPlacementView& view, ForestPlacementFrame& plan,
    std::span<const ForestShadowPage> shadowPages)
{
    if (m_Prepared.Frame)
        return false;

    m_Uploader.DrainCompleted(device);
    while (!m_ReadFences.empty() &&
           device.QueryGpuSyncToken(m_ReadFences.front().Token) == Rendering::IDevice::GpuSyncStatus::Complete)
        m_ReadFences.pop_front();
    const uint64 completedReaders = m_ReadFences.empty() ? m_LastFinished : m_ReadFences.front().Frame - 1;
    // An unknown fence does not prove that earlier draws finished reading their slots. Then
    // the view and the cascades want no pages.
    ForestPlacementView demand = view;
    m_UnknownReader = !m_ReadFences.empty() &&
        device.QueryGpuSyncToken(m_ReadFences.front().Token) == Rendering::IDevice::GpuSyncStatus::Unknown;
    if (m_UnknownReader)
    {
        demand.DetailDistance = 0;
        shadowPages = {};
        m_PrepareGraphicsToken = graph.SubmissionToken();
    }
    if (!m_Stream.Prepare(assets, demand, m_LastFinished + 1, completedReaders, shadowPages, plan, m_Uploads))
        return false;

    m_Uploader.Record(graph, m_Uploads);
    m_Uploads.clear();
    m_Prepared.Stamp(graph);
    m_DeclaredDetail = !plan.DetailedDraws.empty() || !plan.ShadowDraws.empty();
    return true;
}

void ForestPlacementRenderState::Finish(Rendering::RenderGraph::RGFrame& graph, bool submitted)
{
    if (!m_Prepared.IsFor(graph))
        return;
    m_Uploader.FinishFrame(graph);
    ++m_LastFinished;
    if (submitted && (m_DeclaredDetail || m_UnknownReader))
    {
        const auto token = graph.SubmissionToken();
        // Detail reads are ordered on graphics. A new signal from the overview frame
        // covers the unknown reader too. A token left over from an earlier frame cannot.
        if (m_UnknownReader && token.IsValid() &&
            (token.sem != m_PrepareGraphicsToken.sem || token.value != m_PrepareGraphicsToken.value))
            m_ReadFences.front().Token = token;
        if (m_DeclaredDetail)
            m_ReadFences.push_back({m_LastFinished, token});
    }
    m_Prepared = {};
}

} // namespace GameEngine::Engine::Renderer
