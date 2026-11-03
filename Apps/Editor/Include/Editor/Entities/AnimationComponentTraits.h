#pragma once

namespace GameEngine::Editor
{

// Registers the editor traits of the animation components that have no
// inspector of their own; the Animator and Value Curve inspectors register
// theirs.
void RegisterAnimationComponentTraits();

} // namespace GameEngine::Editor
