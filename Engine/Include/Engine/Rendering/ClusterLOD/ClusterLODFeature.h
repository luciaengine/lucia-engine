#pragma once

#include "Engine/Rendering/IRenderFeature.h"

#include <memory>

namespace GameEngine
{
class AssetManager;
namespace ECS { class World; }
namespace Engine::Renderer
{

/// Opt-in, raster-only coarse/fine cluster residency. Mutations run before extraction.
/// Preparation, graph declaration and execution share the serialized rendering owner.
class ClusterLODFeature final : public IRenderFeature
{
  public:
    ClusterLODFeature();
    ~ClusterLODFeature() override;

    /// Called during schedule registration, before any geometry declaration.
    bool Initialize(RenderServices& services, AssetManager& assets);
    /// Caller-thread, exclusive ECS preparation; freezes the cut for this app frame.
    void Prepare(ECS::World& world);
    bool BindWorldGeometry(Rendering::RenderGraph::RGFrame& frame, RenderServices& services,
                           std::weak_ptr<const void> owner) override;
    void OnDeviceRebuilt(Rendering::IDevice* device) override;

  private:
    struct State;
    std::unique_ptr<State> m_State;
};

} // namespace Engine::Renderer
} // namespace GameEngine
