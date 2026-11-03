#pragma once

namespace GameEngine
{
class RenderDocCapture;
namespace Rendering
{
class IDevice;
}
namespace Rendering::RenderGraph
{
class RGFrame;
}
} // namespace GameEngine

namespace GameEngine::Editor
{

// What the editor hands a panel's per-frame update (EditorPanelTypeDescriptor::Update).
struct PanelUpdateContext
{
    Rendering::IDevice* Device = nullptr;
    RenderDocCapture* RenderDoc = nullptr;
    // The main window's live render graph for this frame: the source the
    // render-graph, profiler and VRAM panels introspect.
    Rendering::RenderGraph::RGFrame* RenderGraphFrame = nullptr;
};

} // namespace GameEngine::Editor
