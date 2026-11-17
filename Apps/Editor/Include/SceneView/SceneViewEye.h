#pragma once

#include "ECS/ECS.h"

namespace GameEngine::ECS
{
class World;
}

namespace GameEngine::Editor
{

// True when the Scene View leaves `entity` out: hidden itself (Components::SceneViewHidden), or under a
// hidden ancestor. Picking and the selection outline ask this, so a hidden entity is neither clicked nor
// outlined there.
bool IsHiddenInSceneView(ECS::World& world, ECS::EntityHandle entity);

// Keeps the derived hidden tag out of the Inspector. SceneViewHidden itself shows there, so a user can
// remove it.
void RegisterSceneViewEyeTraits();

} // namespace GameEngine::Editor
