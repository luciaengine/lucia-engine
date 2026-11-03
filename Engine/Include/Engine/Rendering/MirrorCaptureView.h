#pragma once
#include "Rendering/CameraTypes.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"

namespace GameEngine::Engine::Renderer
{
class RenderServices;
namespace Pipeline { struct ViewDeclare; }

// Camera/view ownership is explicit: release before the owning RenderServices.
// Consumers own their color textures; winding, extraction and world submission
// are shared so ocean and component mirrors cannot acquire different rules.
struct MirrorCaptureView
{
    Rendering::CameraId Camera = 0;
    Rendering::ViewId View = 0;
    Rendering::ViewId SourceView = 0;
};
void ReleaseMirrorCaptureView(RenderServices& services, MirrorCaptureView& capture);
void UpdateMirrorCaptureView(RenderServices& services, const Rendering::ViewDesc& mainView,
                             const Rendering::CameraData& reflected, uint32_t mask,
                             MirrorCaptureView& capture);
bool PrepareMirrorCaptureView(Pipeline::ViewDeclare& d, const MirrorCaptureView& capture);
bool DeclareMirrorCaptureWorld(Pipeline::ViewDeclare& d, const MirrorCaptureView& capture,
    Rendering::RenderGraph::RGTexture color, Rendering::RenderGraph::RGTexture depth,
    Rendering::RenderGraph::RGTexture resolve = {});
}
