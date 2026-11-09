#pragma once

#include <cstdint>

namespace GameEngine
{
namespace Mathematics
{

/// Axis-aligned rectangle of whole units, for pixel and cell bounds.
/// The caller picks the space. The right and bottom edges are exclusive.
struct RectInt
{
    int32_t X = 0;
    int32_t Y = 0;
    int32_t Width = 0;
    int32_t Height = 0;

    bool operator==(const RectInt&) const = default;

    int32_t Right() const { return X + Width; }
    int32_t Bottom() const { return Y + Height; }

    /// True when the point is inside. A point on the right or bottom edge is outside.
    bool Contains(int32_t x, int32_t y) const { return x >= X && x < Right() && y >= Y && y < Bottom(); }
};

} // namespace Mathematics
} // namespace GameEngine
