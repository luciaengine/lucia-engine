#pragma once

#include "SceneLoadTracker/SceneLoadTracker.h"

namespace GameEngine
{
class EngineCore;

/// Holds scene-readiness gates for the engine's scene and rendering work.
class SceneLoadingGates
{
  public:
    explicit SceneLoadingGates(EngineCore& engine);

  private:
    struct Report
    {
        SceneLoadTracker::Category Category;
        SceneLoadTracker::Gate Gate;

        template <typename Count>
        Report(SceneLoadTracker& tracker, const SceneLoadTracker::CategoryDesc& desc,
               std::string_view reason, Count count)
            : Category(tracker.RegisterCategory(desc)),
              Gate(Category.RegisterGate([count, reason]() -> SceneLoadTracker::GateReport {
                  const size_t pending = count();
                  if (pending == 0)
                      return {};
                  return {SceneLoadTracker::GateState::Waiting, reason, pending};
              }))
        {
        }
    };

    Report m_Resolve;
    Report m_BaseCompiles;
    Report m_Variants;
    Report m_DevicePipelines;
    Report m_Textures;
};
} // namespace GameEngine
