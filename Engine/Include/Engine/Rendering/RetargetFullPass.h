#pragma once

#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Core/Device.h"

#include <vector>

namespace GameEngine::Engine::Renderer
{

class RetargetGPUDataStore;

// Samples and retargets each character into its skin palette in one workgroup.
// Declared in early setup before graphics passes read the shared atlas.
class RetargetFullPass final
{
public:
    void SetComputeShader(const std::vector<uint8_t>& spirv) { m_ComputeShader = spirv; }
    void SetDataStore(RetargetGPUDataStore* store) { m_DataStore = store; }

    // Declared on the SAME atlas RGBuffer value the skinning pass writes —
    // handle-dedup makes them one resource, so RenderGraph orders the two writers.
    void Declare(GameEngine::Rendering::RenderGraph::RGFrame& frame,
                 GameEngine::Rendering::RenderGraph::RGBuffer atlas);

private:
    struct PreparedDispatch;
    bool PrepareDispatch(GameEngine::Rendering::RenderGraph::RGPrepareContext& context,
                         GameEngine::Rendering::BufferHandle atlasBuffer,
                         PreparedDispatch& dispatch);
    void RecordDispatch(GameEngine::Rendering::CommandList* commandList,
                        const PreparedDispatch& dispatch);
    std::vector<uint8_t> m_ComputeShader;
    RetargetGPUDataStore* m_DataStore = nullptr;

    GameEngine::Rendering::ComputePipelineId m_PipelineId{};
};

} // namespace GameEngine::Engine::Renderer
