#include "SceneView/SceneViewGridRenderer.h"

#include "Rendering/Core/Pipeline/PipelineTypes.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Rendering;

namespace
{

// The grid fragment shaders read these blocks (infinite_grid.frag, infinite_grid_2d.frag).
// The 2D block adds the authored cell grid vec4 to the 3D layout.
constexpr uint32_t kGridConstantsBytes = 112u;
constexpr uint32_t kGrid2DConstantsBytes = 128u;

const std::vector<uint8_t> kVertexShader{1u, 2u, 3u, 4u};
const std::vector<uint8_t> kFragmentShader{5u, 6u, 7u, 8u};

} // namespace

TEST(SceneViewGridPipeline, TwoDimensionalModeDeclaresTheCellGridConstantRange)
{
    const GraphicsPipelineDesc desc = SceneViewGridRenderer::PipelineDesc(true, kVertexShader, kFragmentShader);
    EXPECT_EQ(desc.PushConstants.Size, kGrid2DConstantsBytes);
    EXPECT_EQ(desc.PushConstants.StageMask, kShaderStageFragment);
}

TEST(SceneViewGridPipeline, ThreeDimensionalModeDeclaresTheGridConstantRange)
{
    const GraphicsPipelineDesc desc = SceneViewGridRenderer::PipelineDesc(false, kVertexShader, kFragmentShader);
    EXPECT_EQ(desc.PushConstants.Size, kGridConstantsBytes);
    EXPECT_EQ(desc.PushConstants.StageMask, kShaderStageFragment);
}

TEST(SceneViewGridPipeline, TheDescriptionKeepsTheGivenShadersAndReadsDepthOnly)
{
    const GraphicsPipelineDesc grid2D = SceneViewGridRenderer::PipelineDesc(true, kVertexShader, kFragmentShader);
    ASSERT_TRUE(grid2D.VertexShader && grid2D.PixelShader);
    EXPECT_EQ(*grid2D.VertexShader, kVertexShader);
    EXPECT_EQ(*grid2D.PixelShader, kFragmentShader);
    // The grid tests against scene depth and never writes it, so gizmos drawn after it stay visible.
    EXPECT_TRUE(grid2D.DepthStencil.depthTestEnable);
    EXPECT_FALSE(grid2D.DepthStencil.depthWriteEnable);
}
