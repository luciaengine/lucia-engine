#include "Engine/Rendering/MaterialMipStreaming/MaterialScreenDemand.h"

#include "Engine/Rendering/CameraUtils.h"
#include "Engine/Rendering/RenderOrigin.h"
#include "Rendering/Camera/CameraTypes.h"
#include "Rendering/Core/GPUScene.h"

#include <algorithm>
#include <cmath>

namespace GameEngine::Engine::Renderer
{

namespace
{
// The smallest distance to a bounding sphere's surface, in meters. It bounds the coverage of a
// mesh that the camera is inside.
constexpr float32 kMinSurfaceDistance = 0.01f;
} // namespace

std::optional<MaterialScreenDemand::View> MaterialScreenDemand::ViewOf(const Rendering::CameraData& camera)
{
    const float32 projectionScale = std::abs(camera.proj[1][1]);
    if (camera.renderOriginSector[3] == 0 || !std::isfinite(projectionScale) || projectionScale <= 0.0f)
        return std::nullopt;
    View view;
    view.OriginSector = CameraRenderOriginSector(camera);
    view.Eye = CameraEyeRelativeToOrigin(camera);
    view.ProjScaleY = projectionScale;
    view.Orthographic = IsOrthographicProjectionLH_ZO(camera.proj);
    return view;
}

bool MaterialScreenDemand::Advance(std::span<const Rendering::GPUInstance> rows, const View& view, const Pace& pace)
{
    // The scene trims freed rows at its end: a cursor past the end has read every row.
    const size_t first = std::min(m_Cursor, rows.size());
    const size_t end = std::min(rows.size(), first + pace.RowsPerUpdate);
    for (const Rendering::GPUInstance& row : rows.subspan(first, end - first))
    {
        if (row.materialIndex == Rendering::kTombstoneIndex)
            continue;
        float32 coverage = row.boundingRadius * view.ProjScaleY;
        if (!view.Orthographic)
        {
            const Mathematics::Vector3 center = Rendering::SectorLocalPointRelativeTo(
                row.boundingCenter, row.sectorPacked[0], row.sectorPacked[1], view.OriginSector);
            coverage /= std::max((center - view.Eye).Length() - row.boundingRadius, kMinSurfaceDistance);
        }
        if (row.materialIndex >= m_Building.size())
            m_Building.resize(static_cast<size_t>(row.materialIndex) + 1, 0.0f);
        m_Building[row.materialIndex] = std::max(m_Building[row.materialIndex], coverage);
    }
    m_Cursor = end;
    ++m_UpdatesInSweep;
    if (m_Cursor < rows.size() || m_UpdatesInSweep < pace.MinUpdatesPerSweep)
        return false;

    const bool changed = m_Building != m_Published;
    m_Published.swap(m_Building);
    m_Building.assign(m_Published.size(), 0.0f);
    m_Cursor = 0;
    m_UpdatesInSweep = 0;
    return changed;
}

void MaterialScreenDemand::Reset()
{
    m_Building.clear();
    m_Published.clear();
    m_Cursor = 0;
    m_UpdatesInSweep = 0;
}

} // namespace GameEngine::Engine::Renderer
