#include "Engine/Rendering/MaterialMipStreaming/MaterialScreenDemand.h"

#include "Rendering/Core/GPUScene.h"

#include <algorithm>

namespace GameEngine::Engine::Renderer
{

namespace
{
// The smallest distance to a bounding sphere's surface, in meters. It bounds the coverage of a
// mesh the camera is inside.
constexpr float32 kMinSurfaceDistance = 0.01f;
} // namespace

bool MaterialScreenDemand::Advance(std::span<const Rendering::GPUInstance> rows, const View& view, uint32 rowBudget)
{
    // The scene trims freed rows at its end: a cursor past the end completes the sweep.
    const size_t first = std::min(m_Cursor, rows.size());
    const size_t end = std::min(rows.size(), first + rowBudget);
    for (const Rendering::GPUInstance& row : rows.subspan(first, end - first))
    {
        if (row.materialIndex == Rendering::kTombstoneIndex)
            continue;
        const Mathematics::Vector3 center = Rendering::SectorLocalPointRelativeTo(
            row.boundingCenter, row.sectorPacked[0], row.sectorPacked[1], view.OriginSector);
        const float32 surfaceDistance =
            std::max((center - view.Eye).Length() - row.boundingRadius, kMinSurfaceDistance);
        const float32 coverage = row.boundingRadius * view.ProjScaleY / surfaceDistance;
        if (row.materialIndex >= m_Building.size())
            m_Building.resize(static_cast<size_t>(row.materialIndex) + 1, 0.0f);
        m_Building[row.materialIndex] = std::max(m_Building[row.materialIndex], coverage);
    }
    m_Cursor = end;
    if (m_Cursor < rows.size())
        return false;

    m_Published.swap(m_Building);
    m_Building.assign(m_Published.size(), 0.0f);
    m_Cursor = 0;
    return true;
}

void MaterialScreenDemand::Reset()
{
    m_Building.clear();
    m_Published.clear();
    m_Cursor = 0;
}

} // namespace GameEngine::Engine::Renderer
