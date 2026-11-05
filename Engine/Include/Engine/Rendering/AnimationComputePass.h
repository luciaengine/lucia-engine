#pragma once

#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Core/Device.h"

#include <vector>

namespace GameEngine::Engine::Renderer
{

class GPUAnimationDataStore;

// Render graph compute pass that dispatches the GPU animation skinning shader.
// One dispatch per frame for all accumulated animation instances.
// Writes skin matrices directly into the SkinPaletteAtlas SSBO.
class AnimationComputePass final
{
public:
    void SetComputeShader(const std::vector<uint8_t>& spirv) { m_ComputeShader = spirv; }
    void SetDataStore(GPUAnimationDataStore* store) { m_DataStore = store; }

    // Visibility gates skinning when every entity for a runtime is off-screen.
    // runtimeVisibleCap is the visibility buffer capacity in uint32_t elements.
    void SetRuntimeVisibility(bool gateEnabled, uint32_t runtimeVisibleCap)
    {
        m_GateEnabled       = gateEnabled;
        m_RuntimeVisibleCap = runtimeVisibleCap;
    }

    // The declared Read(runtimeVisible) — recorded BEFORE the aggregate's Write —
    // is the WAR edge that pins skinning-before-aggregate. Invalid runtimeVisible ⇒
    // ungated dispatch.
    void Declare(GameEngine::Rendering::RenderGraph::RGFrame& frame,
                 GameEngine::Rendering::RenderGraph::RGBuffer atlas,
                 GameEngine::Rendering::RenderGraph::RGBuffer runtimeVisible);

private:
    struct PreparedDispatch;
    bool PrepareDispatch(GameEngine::Rendering::RenderGraph::RGPrepareContext& context,
                         GameEngine::Rendering::BufferHandle atlasBuffer,
                         GameEngine::Rendering::BufferHandle visibilityBuffer,
                         PreparedDispatch& dispatch);
    void RecordDispatch(GameEngine::Rendering::CommandList* commandList,
                        const PreparedDispatch& dispatch);
    std::vector<uint8_t> m_ComputeShader;
    GPUAnimationDataStore* m_DataStore = nullptr;

    bool     m_GateEnabled       = false;
    uint32_t m_RuntimeVisibleCap = 0;

    GameEngine::Rendering::ComputePipelineId m_PipelineId{};
};

} // namespace GameEngine::Engine::Renderer
