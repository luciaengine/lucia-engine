#pragma once

#include "Mathematics/Vector3.h"

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace GameEngine {
namespace Mathematics {

/// A 3D vector of 32-bit signed integers, for exact lattice points such as cube corners.
/// The components are plain members, not a union, so constant expressions can read them by index.
struct Vector3Int
{
    int32_t X = 0;
    int32_t Y = 0;
    int32_t Z = 0;

    constexpr Vector3Int() = default;
    constexpr Vector3Int(int32_t xValue, int32_t yValue, int32_t zValue) : X(xValue), Y(yValue), Z(zValue) {}

    /// Component by index: 0 is `X`, 1 is `Y`, 2 is `Z`. The caller keeps the index below 3.
    constexpr int32_t operator[](std::size_t index) const { return index == 0 ? X : (index == 1 ? Y : Z); }

    constexpr bool operator==(const Vector3Int& other) const = default;

    constexpr Vector3Int operator+(const Vector3Int& other) const { return Vector3Int(X + other.X, Y + other.Y, Z + other.Z); }

    /// Each component divided by `divisor`, rounded toward zero.
    constexpr Vector3Int operator/(int32_t divisor) const { return Vector3Int(X / divisor, Y / divisor, Z / divisor); }

    /// The same point as a float vector.
    explicit operator Vector3() const
    {
        return Vector3(static_cast<float>(X), static_cast<float>(Y), static_cast<float>(Z));
    }
};

static_assert(std::is_standard_layout_v<Vector3Int>);
static_assert(sizeof(Vector3Int) == 3 * sizeof(int32_t));

} // namespace Mathematics
} // namespace GameEngine
