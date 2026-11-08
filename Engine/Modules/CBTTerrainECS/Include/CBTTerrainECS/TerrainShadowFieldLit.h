#pragma once

#include "Rendering/Core/Device.h"
#include "Rendering/Core/PipelineIdentifiers.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Materials/ShaderMeta.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace GameEngine::CBTTerrain
{
struct TerrainShadowGrid;
}

namespace GameEngine::Engine::Renderer
{
class RenderServices;
}

namespace GameEngine::CBTTerrainECS
{

/// Filtered terrain shadow coverage and occluder distances, stored as an RG16F mip chain.
/// Rebuilt after the clearance map changes. Receivers sample the mip for their pixel footprint.
class TerrainShadowFieldLit
{
  public:
    TerrainShadowFieldLit();
    ~TerrainShadowFieldLit();
    TerrainShadowFieldLit(const TerrainShadowFieldLit&) = delete;
    TerrainShadowFieldLit& operator=(const TerrainShadowFieldLit&) = delete;

    // Declares the field of a `mapSide` square map into `frame`, and its rebuild from `map` (laid out by
    // `grid`; the graph orders it after the map's bake) when `mapBaked` or when the field is new. False
    // when it cannot be published (no pipeline, no bindless slot): the receivers then read the map alone.
    bool Declare(Engine::Renderer::RenderServices& services, Rendering::RenderGraph::RGFrame& frame,
                 Rendering::RenderGraph::RGTexture map, uint32_t mapSide, const CBTTerrain::TerrainShadowGrid& grid,
                 bool mapBaked);

    // This frame's field, its bindless index and its levels (valid after a Declare that returned true).
    Rendering::RenderGraph::RGTexture Field() const { return m_Field; }
    uint32_t BindlessIndex() const { return m_BindlessIndex; }
    uint32_t Levels() const { return m_Levels; }

    // Drops the bindless registration and the level views (a frame without a map).
    void Release(Engine::Renderer::RenderServices& services);
    // Forgets every handle of the dead device; the next Declare re-creates them and rebuilds.
    void OnDeviceRebuilt();

  private:
    bool EnsurePipeline(Rendering::IDevice& device);
    bool DeclareRebuildPass(Rendering::RenderGraph::RGFrame& frame, Rendering::RenderGraph::RGTexture map,
                            Rendering::RenderGraph::RGTexture field, const CBTTerrain::TerrainShadowGrid& grid);
    void DestroyViews(Rendering::IDevice& device);

    bool m_LoadAttempted = false;
    Rendering::ComputePipelineId m_PipelineId{};
    std::unique_ptr<Rendering::ShaderMeta> m_Meta;
    Rendering::DescriptorSetLayoutDesc m_Set0Layout{};
    // The field's physical texture, its bindless registration and one storage view per level
    // (re-created when the pool hands out another physical).
    Rendering::TextureHandle m_Texture{};
    std::vector<Rendering::TextureViewHandle> m_LevelViews;
    uint32_t m_BindlessIndex = 0;
    uint32_t m_Side = 0;
    uint32_t m_Levels = 0;
    Rendering::RenderGraph::RGTexture m_Field{};
};

} // namespace GameEngine::CBTTerrainECS
