#pragma once

#include "Mathematics/Vector2.h"
#include "Mathematics/Vector3.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace GameEngine::Mathematics
{

/// How a square map covers the directions around an object.
enum class OctahedralLayout : uint8_t
{
    /// The upper hemisphere around +Y only: for objects seen from about their own height or
    /// above, such as trees and props. All the map's resolution goes to the visible half.
    Hemisphere = 0,
    /// The full sphere around +Z: for objects seen from every side, as in flight and space.
    Sphere = 1,
};

/// Octahedral encode of a unit vector into [0,1]^2, with +Z at the center. The exact CPU
/// mirror of GE_OctEncode in Shaders/Includes/octahedral_normal.glsl.
inline Vector2 OctahedralEncode(const Vector3& direction)
{
    const float norm = std::abs(direction.x) + std::abs(direction.y) + std::abs(direction.z);
    Vector2 e(direction.x / norm, direction.y / norm);
    if (direction.z < 0.0f)
    {
        const Vector2 folded(1.0f - std::abs(e.y), 1.0f - std::abs(e.x));
        e = Vector2(e.x >= 0.0f ? folded.x : -folded.x, e.y >= 0.0f ? folded.y : -folded.y);
    }
    return Vector2(e.x * 0.5f + 0.5f, e.y * 0.5f + 0.5f);
}

/// The inverse of OctahedralEncode; the CPU mirror of GE_OctDecode.
inline Vector3 OctahedralDecode(const Vector2& uv)
{
    Vector3 n(uv.x * 2.0f - 1.0f, uv.y * 2.0f - 1.0f, 0.0f);
    n.z = 1.0f - std::abs(n.x) - std::abs(n.y);
    const float fold = std::max(-n.z, 0.0f);
    n.x += n.x >= 0.0f ? -fold : fold;
    n.y += n.y >= 0.0f ? -fold : fold;
    return n.Normalize();
}

/// Hemi-octahedral encode of a unit vector with y >= 0 into [0,1]^2, with +Y at the center and
/// the horizon on the square's edge. A vector below the horizon encodes as its horizon point;
/// straight down, which has none, encodes as the +X horizon point. CPU mirror of GE_HemiOctEncode.
inline Vector2 HemiOctahedralEncode(const Vector3& direction)
{
    constexpr float kStraightDownNorm = 1e-6f;
    const float norm = std::abs(direction.x) + std::max(direction.y, 0.0f) + std::abs(direction.z);
    if (norm < kStraightDownNorm)
        return Vector2(1.0f, 0.0f);
    const float px = direction.x / norm;
    const float pz = direction.z / norm;
    return Vector2((px + pz) * 0.5f + 0.5f, (pz - px) * 0.5f + 0.5f);
}

/// The inverse of HemiOctahedralEncode; CPU mirror of GE_HemiOctDecode.
inline Vector3 HemiOctahedralDecode(const Vector2& uv)
{
    const float a = uv.x * 2.0f - 1.0f;
    const float b = uv.y * 2.0f - 1.0f;
    const float px = (a - b) * 0.5f;
    const float pz = (a + b) * 0.5f;
    return Vector3(px, std::max(1.0f - std::abs(px) - std::abs(pz), 0.0f), pz).Normalize();
}

inline Vector2 OctahedralEncode(OctahedralLayout layout, const Vector3& direction)
{
    return layout == OctahedralLayout::Hemisphere ? HemiOctahedralEncode(direction) : OctahedralEncode(direction);
}

inline Vector3 OctahedralDecode(OctahedralLayout layout, const Vector2& uv)
{
    return layout == OctahedralLayout::Hemisphere ? HemiOctahedralDecode(uv) : OctahedralDecode(uv);
}

/// The view direction (from the object toward the camera) of frame (column, row) in a grid of
/// `framesPerSide` x `framesPerSide` frames. The frames sit on the grid points from edge to edge,
/// so the outer frames of the hemisphere look from the horizon itself.
inline Vector3 OctahedralFrameDirection(OctahedralLayout layout, uint32_t column, uint32_t row, uint32_t framesPerSide)
{
    const float steps = static_cast<float>(framesPerSide > 1u ? framesPerSide - 1u : 1u);
    return OctahedralDecode(layout, Vector2(static_cast<float>(column) / steps, static_cast<float>(row) / steps));
}

} // namespace GameEngine::Mathematics
