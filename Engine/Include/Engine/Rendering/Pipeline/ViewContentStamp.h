#pragma once

#include "Rendering/Camera/CameraTypes.h"

#include <cstdint>

namespace GameEngine::Engine::Renderer
{
class RenderServices;
}

namespace GameEngine::Engine::Renderer::Pipeline
{
/// The world versions that change a view's image while its camera holds (render content, shadow
/// casters, the light list) and the app frame of the capture. Depth that changes in one view only
/// (terrain, ocean, grass) comes in through `viewHasMovers` (RenderServices::HasUnversionedMotion).
struct ViewContentStamp
{
    uint64_t WorldId = 0;
    uint64_t RenderContentVersion = 0;
    uint64_t ShadowCasterContentVersion = 0;
    uint64_t LightListVersion = 0;
    /// RenderServices' app frame (FrameOrchestrator::WorldFrameEpoch) when the stamp was captured.
    uint64_t AppFrame = 0;

    /// The stamp of `worldId` for this frame. A world id of 0 gives a stamp that certifies nothing.
    static ViewContentStamp Capture(const RenderServices& rs, uint64_t worldId);

    /// True when the view renders the same scene as the frame of `previous`: a known world with
    /// the same content, captured on the next app frame, the same unjittered view-projection, and
    /// nothing in view that moves (`viewHasMovers` is ViewMotionVectors::ViewHasMovers for the view
    /// on this frame).
    static bool ViewIsStill(const ViewContentStamp& previous, const ViewContentStamp& current,
                            const Rendering::CameraData& previousCamera,
                            const Rendering::CameraData& currentCamera, bool viewHasMovers);

    /// True when every world version is equal. The app frame is not compared.
    bool SameContent(const ViewContentStamp& other) const;
};
} // namespace GameEngine::Engine::Renderer::Pipeline
