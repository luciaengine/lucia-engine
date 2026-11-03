// Manual reflection for MovingPlatform.
//
// EVERY PhysicsECS component needs a hand-written registration like this one, or
// it ships invisible: the build-time ComponentScanner globs
// Engine/Modules/*/Include/Components, and this module nests its components one
// level deeper (Include/PhysicsECS/Components), so the scanner never sees them.
// Without a registration a component has no Add Component entry and the
// Inspector shows "(No inspector registered)" — it is reachable only by
// hand-editing a .scene.
//
// Registration covers the field table, the canonical display name and the
// factory's default bytes. The hand-written MovingPlatformSchema still owns
// scene serialization: SceneSchemaRegistry prefers a registered schema over the
// synthesized reflection one, so the two do not both write the component.
//
// No GE_INSTANTIATE_ENGINE_COMPONENT here: PhysicsComponentInstantiations.cpp
// already instantiates this component's cross-DLL surface.

#include "Components/ComponentRegistration.h" // GE_REGISTER_COMPONENT
#include "PhysicsECS/Components/MovingPlatform.h"

GE_REGISTER_COMPONENT(GameEngine::Components::MovingPlatform, pathMode, direction, finished, path,
                      speed, endpointPause, distance, pauseRemaining);

GE_REFLECT_ENUM_TABLE_BEGIN(PlatformPathMode)
    GE_REFLECT_ENUM_TABLE_VALUE("Once", GameEngine::Components::PlatformPathMode::Once)
    GE_REFLECT_ENUM_TABLE_VALUE("Loop", GameEngine::Components::PlatformPathMode::Loop)
    GE_REFLECT_ENUM_TABLE_VALUE("PingPong", GameEngine::Components::PlatformPathMode::PingPong)
GE_REFLECT_ENUM_TABLE_END()

GE_REFLECT_ENUM_FIELD(GameEngine::Components::MovingPlatform, pathMode, PlatformPathMode)

// Where the platform currently is along its path, not what was authored: the
// scene stores authoring only, so these never round-trip and never show up as
// editable rows.
GE_REFLECT_FIELD_FLAGS(GameEngine::Components::MovingPlatform, direction,
                       ::GameEngine::ECS::FieldFlags::Hidden |
                           ::GameEngine::ECS::FieldFlags::Transient)
GE_REFLECT_FIELD_FLAGS(GameEngine::Components::MovingPlatform, finished,
                       ::GameEngine::ECS::FieldFlags::Hidden |
                           ::GameEngine::ECS::FieldFlags::Transient)
GE_REFLECT_FIELD_FLAGS(GameEngine::Components::MovingPlatform, distance,
                       ::GameEngine::ECS::FieldFlags::Hidden |
                           ::GameEngine::ECS::FieldFlags::Transient)
GE_REFLECT_FIELD_FLAGS(GameEngine::Components::MovingPlatform, pauseRemaining,
                       ::GameEngine::ECS::FieldFlags::Hidden |
                           ::GameEngine::ECS::FieldFlags::Transient)
