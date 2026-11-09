#pragma once

#include <cstdint>

namespace GameEngine::Engine::Renderer
{
// A current prepared depth producer, before any retained-content decision.
struct DepthProducerState
{
    uint64_t MembershipKey = 0;
    uint64_t MembershipCount = 0;
    uint64_t ServedContentKey = 0;
    bool Complete = false;

    bool operator==(const DepthProducerState&) const = default;
};
} // namespace GameEngine::Engine::Renderer
