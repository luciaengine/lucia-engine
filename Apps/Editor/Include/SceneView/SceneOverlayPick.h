#pragma once

#include "ECS/Entity.h"
#include "Mathematics/Vector3.h"

#include <functional>

namespace GameEngine::ECS
{
class World;
}

namespace GameEngine::Editor::SceneTools
{

struct ScenePointerEvent;

// Whether a Scene View click may pick `entity`: false for a locked one or one under a locked parent.
using SceneOverlayPickFilter = std::function<bool(ECS::EntityHandle)>;

// The outline an overlay picker found under the pointer: the entity it draws, and the outline's
// world point under the pointer. No entity when no outline is close enough.
struct SceneOverlayHit
{
    ECS::EntityHandle Entity{};
    Mathematics::Vector3 Point{};
};

// How a feature that draws an entity as outlines in the Scene View's overlay pass, with no mesh
// for the scene raycast to hit (a mark-up's region, path or volume), lets a click on those
// outlines select it: the outline nearest the camera within a few pixels of the pointer of the
// event (its view pixel and the camera state PopulatePointerCameraState wrote), among the entities
// the filter accepts. The Scene View asks through here and never names the feature. Main thread
// only.
using SceneOverlayPicker =
    std::function<SceneOverlayHit(ECS::World&, const ScenePointerEvent&, const SceneOverlayPickFilter&)>;

// Registers the one overlay picker (the mark-ups module's); an empty picker picks nothing.
void SetSceneOverlayPicker(SceneOverlayPicker picker);
// The entity the registered picker picks under the pointer of `event`, when its outline point is
// in sight: none without a picker, and none when scene geometry lies in front of that point, as
// the overlay pass dims an occluded outline. Every entity that draws in the scene occludes, a
// locked one included. The scene TLAS must be current (SceneTlas::RequestCurrent reported Ready).
ECS::EntityHandle PickSceneOverlay(ECS::World& world, const ScenePointerEvent& event,
                                   const SceneOverlayPickFilter& isPickable);

} // namespace GameEngine::Editor::SceneTools
