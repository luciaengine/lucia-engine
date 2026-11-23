#include "Engine/Rendering/Forest/ForestPlacementRenderState.h"

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
    Rendering::RenderGraph::RGFrame& graph, const ForestPlacementView& view, ForestPlacementFrame& plan)
{
    if (m_Prepared.Frame)
        return false;

    m_Uploader.DrainCompleted(device);
    while (!m_ReadFences.empty() &&
           device.QueryGpuSyncToken(m_ReadFences.front().Token) == Rendering::IDevice::GpuSyncStatus::Complete)
        m_ReadFences.pop_front();
    const uint64 completedReaders = m_ReadFences.empty() ? m_LastFinished : m_ReadFences.front().Frame - 1;
    ForestPlacementView demand = view;
    if (!m_ReadFences.empty() &&
        device.QueryGpuSyncToken(m_ReadFences.front().Token) == Rendering::IDevice::GpuSyncStatus::Unknown)
        demand.DetailDistance = 0;
    if (!m_Stream.Prepare(assets, demand, m_LastFinished + 1, completedReaders, plan, m_Uploads))
        return false;

    m_Uploader.Record(graph, m_Uploads);
    m_Uploads.clear();
    m_Prepared.Stamp(graph);
    m_DeclaredDetail = !plan.DetailedDraws.empty();
    return true;
}

void ForestPlacementRenderState::Finish(Rendering::RenderGraph::RGFrame& graph, bool submitted)
{
    if (!m_Prepared.IsFor(graph))
        return;
    m_Uploader.FinishFrame(graph);
    ++m_LastFinished;
    // An unknown token keeps the reservation: it does not prove that the draws
    // finished reading the slot.
    if (submitted && m_DeclaredDetail)
        m_ReadFences.push_back({m_LastFinished, graph.SubmissionToken()});
    m_Prepared = {};
}

} // namespace GameEngine::Engine::Renderer
