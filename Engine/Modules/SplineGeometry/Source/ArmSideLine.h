#pragma once

#include "Mathematics/Vector2.h"
#include "SplineGeometry/AreaBoolean.h"
#include "Types/Types.h"

namespace GameEngine::SplineGeometry
{

class PavedArmPath;
struct PavedArmSection;

enum class ArmSideKind : uint8
{
    Right,
    Left,
};

// One side of an arm's ribbon by meters along the arm: the ribbon's own side ahead of the node, a
// straight line behind it.
class ArmSideLine
{
  public:
    ArmSideLine(const PavedArmPath& path, ArmSideKind side);

    Mathematics::Vector2 At(float32 along) const;

    // The side's normal away from the ribbon.
    Mathematics::Vector2 OutAt(float32 along) const;

    // The ribbon's side points strictly between `from` and `to` meters along, in that order.
    void AppendBetween(float32 from, float32 to, AreaRing& out) const;

  private:
    Mathematics::Vector2 Pick(const PavedArmSection& section) const;
    Mathematics::Vector2 OutOf(const PavedArmSection& section) const;

    const PavedArmPath& m_Path;
    ArmSideKind m_Side;
    Mathematics::Vector2 m_Origin{};
    Mathematics::Vector2 m_Out{};
    Mathematics::Vector2 m_Forward{};
};

} // namespace GameEngine::SplineGeometry
