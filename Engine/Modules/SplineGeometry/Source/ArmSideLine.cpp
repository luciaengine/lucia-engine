#include "ArmSideLine.h"

#include "PavedArmPath.h"

#include <algorithm>
#include <cstddef>

namespace GameEngine::SplineGeometry
{

using Mathematics::Vector2;

ArmSideLine::ArmSideLine(const PavedArmPath& path, ArmSideKind side) : m_Path(path), m_Side(side)
{
    const PavedArmSection& node = path.GetSection(0);
    m_Origin = Pick(node);
    m_Out = OutOf(node);
    const Vector2 right = (node.Right - node.Left).Normalize();
    m_Forward = Vector2(-right.y, right.x);
}

Vector2 ArmSideLine::At(float32 along) const
{
    return along <= 0.0f ? m_Origin + m_Forward * along : Pick(m_Path.SectionAt(along));
}

Vector2 ArmSideLine::OutAt(float32 along) const { return along <= 0.0f ? m_Out : OutOf(m_Path.SectionAt(along)); }

void ArmSideLine::AppendBetween(float32 from, float32 to, AreaRing& out) const
{
    const float32 low = std::min(from, to);
    const float32 high = std::max(from, to);
    const std::size_t count = m_Path.GetStationCount();
    for (std::size_t step = 0; step < count; ++step)
    {
        const std::size_t station = from > to ? count - 1 - step : step;
        const float32 along = m_Path.GetAlong(station);
        if (along > low + kPavedTouchMeters && along < high - kPavedTouchMeters)
            out.push_back(Pick(m_Path.GetSection(station)));
    }
}

Vector2 ArmSideLine::Pick(const PavedArmSection& section) const
{
    return m_Side == ArmSideKind::Right ? section.Right : section.Left;
}

Vector2 ArmSideLine::OutOf(const PavedArmSection& section) const
{
    const Vector2 across = (section.Right - section.Left).Normalize();
    return m_Side == ArmSideKind::Right ? across : across * -1.0f;
}

} // namespace GameEngine::SplineGeometry
