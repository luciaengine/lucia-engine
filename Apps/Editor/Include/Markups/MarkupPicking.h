#pragma once

#include "ECS/Entity.h"
#include "SceneView/SceneOverlayPick.h"
#include "Types/Types.h"

namespace GameEngine::ECS
{
class World;
}

namespace GameEngine::Editor
{
class MarkupEditorBridge;

// The mark-up whose outline, as MarkupGizmo draws it on `frame`, passes within a few pixels of the
// pointer of `event` (its view pixel and camera state), nearest the camera, with the outline's
// point under the pointer; none when no outline is that close. The outlines are a region's rings at
// its base and along its top edge, a path's draped line, a box's twelve edges and a sphere's circle
// facing the camera (its three rings while the camera is inside it). Only the outlines pick, never the
// inside of a shape, so the ground and the objects a mark-up encloses keep their clicks. The
// mark-ups CollectMarkupDrawItems leaves out (hidden ones, switched-off ones, every one in play
// mode) and those `isPickable` refuses pick nothing. Main thread only.
SceneTools::SceneOverlayHit PickMarkupOutline(ECS::World& world, const MarkupEditorBridge& bridge,
                                              const SceneTools::ScenePointerEvent& event, uint64 frame,
                                              const SceneTools::SceneOverlayPickFilter& isPickable);

// The Scene View's overlay picker for mark-ups (SetSceneOverlayPicker): PickMarkupOutline on the
// installed bridge and the application's current frame; none without a bridge.
SceneTools::SceneOverlayHit PickSceneMarkupOutline(ECS::World& world, const SceneTools::ScenePointerEvent& event,
                                                   const SceneTools::SceneOverlayPickFilter& isPickable);

} // namespace GameEngine::Editor
