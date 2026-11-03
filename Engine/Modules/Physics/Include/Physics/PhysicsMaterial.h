#pragma once

#include "Types/Types.h"

#include <algorithm>
#include <limits>

namespace GameEngine::Physics
{
/**
 * @brief How two touching surfaces' friction or restitution values are reduced
 * to the single value the solver uses.
 *
 * Each mode reduces the two surfaces' values v1 and v2 to one:
 *   Average  => (v1 + v2) / 2
 *   Minimum  => min(v1, v2)
 *   Multiply => v1 * v2
 *   Maximum  => max(v1, v2)
 *
 * The enumerator order is also the priority order: when the two materials ask
 * for different modes, the higher one wins, so a surface can insist on its
 * behaviour regardless of what it touches (a Maximum-restitution trampoline
 * stays bouncy against an Average-restitution crate). Resolution is symmetric —
 * contact order never changes the outcome.
 */
enum class MaterialCombine : GameEngine::uint8
{
    Average,
    Minimum,
    Multiply,
    Maximum,
};

// Simple physics contact material (backend-agnostic authoring).
// NOTE: Jolt applies friction/restitution at the body level by default; the
// combine modes below are resolved by the contact-modify seam instead.
struct PhysicsMaterial
{
    GameEngine::float32 friction = 0.5f;    // 0=no friction, higher=more friction (typical 0.2..1.0)
    GameEngine::float32 restitution = 0.1f; // 0=no bounce, 1=perfectly elastic

    MaterialCombine frictionCombine = MaterialCombine::Average;
    MaterialCombine restitutionCombine = MaterialCombine::Average;
};

/** The stronger of two requested modes; see MaterialCombine's priority order. */
constexpr MaterialCombine ResolveCombineMode(MaterialCombine a, MaterialCombine b)
{
    return a > b ? a : b;
}

constexpr GameEngine::float32 CombineValues(GameEngine::float32 a, GameEngine::float32 b,
                                            MaterialCombine mode)
{
    switch (mode)
    {
    case MaterialCombine::Minimum:
        return a < b ? a : b;
    case MaterialCombine::Maximum:
        return a > b ? a : b;
    case MaterialCombine::Multiply:
        // Finite authored coefficients can overflow a float product. Saturate
        // at the solver type's finite limit rather than passing infinity on.
        return static_cast<GameEngine::float32>(std::clamp(
            static_cast<double>(a) * static_cast<double>(b),
            static_cast<double>(std::numeric_limits<GameEngine::float32>::lowest()),
            static_cast<double>(std::numeric_limits<GameEngine::float32>::max())));
    case MaterialCombine::Average:
    default:
        return static_cast<GameEngine::float32>((static_cast<double>(a) + static_cast<double>(b)) * 0.5);
    }
}

/** Friction and restitution of a contact between two materials. */
struct ResolvedContactSurface
{
    GameEngine::float32 friction = 0.5f;
    GameEngine::float32 restitution = 0.1f;
};

/**
 * Resolves the pair a contact should be solved with. Pure and allocation-free:
 * this is what the contact-modify callback runs on a physics worker thread.
 */
constexpr ResolvedContactSurface CombineMaterials(const PhysicsMaterial& a, const PhysicsMaterial& b)
{
    return ResolvedContactSurface{
        CombineValues(a.friction, b.friction, ResolveCombineMode(a.frictionCombine, b.frictionCombine)),
        CombineValues(a.restitution, b.restitution,
                      ResolveCombineMode(a.restitutionCombine, b.restitutionCombine))};
}
} // namespace GameEngine::Physics

