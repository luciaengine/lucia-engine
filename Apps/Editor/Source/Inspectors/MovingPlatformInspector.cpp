#include "Inspectors/MovingPlatformInspector.h"

#include "InspectorRegistry.h"
#include "Inspectors/InspectorComponentRowHelpers.h"
#include "Inspectors/InspectorUIHelpers.h"

#include "Components/Spline/SplineComponent.h"
#include "ECS/ECSTemplates.h" // World::GetComponent / AddComponentImmediate definitions
#include "Editor/Entities/EditorComponentTraits.h"
#include "PhysicsECS/Components/MovingPlatform.h"
#include "PhysicsECS/Components/PhysicsBody.h"

#include "UI/Controls/EnumField.h"
#include "UI/Controls/InspectorNotice.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace GameEngine
{
namespace
{
using Components::MovingPlatform;
using Components::PlatformPathMode;

constexpr EnumEntry<PlatformPathMode> kPathModeEntries[] = {
    {PlatformPathMode::Once, "Once"},
    {PlatformPathMode::Loop, "Loop"},
    {PlatformPathMode::PingPong, "Ping Pong"},
};

constexpr const char* kPathTooltip =
    "Entity holding the SplineComponent that describes the path. It must be a separate, "
    "stationary entity: spline control points are stored in their owner's local space, so a "
    "path parented to the platform would move with it.";
constexpr const char* kPathModeTooltip =
    "What happens at the far end of the path. Once: stop there for good. Loop: jump back to "
    "the start and run again. Ping Pong: reverse and run the path backwards.";
constexpr const char* kSpeedTooltip =
    "Meters per second along the path's arc length, so a curve and a straight line of the same "
    "length take the same time.";
constexpr const char* kEndpointPauseTooltip =
    "Seconds to hold still on reaching a terminus. Loop pauses at the seam before restarting; "
    "Ping Pong pauses at both ends. Once never resumes, so it has no pause.";

constexpr float32 kMinSpeed = 0.0f;
constexpr float32 kMaxSpeed = 100.0f;
constexpr float32 kMinEndpointPause = 0.0f;
constexpr float32 kMaxEndpointPause = 60.0f;

// Where the platform got to along its path is not authoring data; a change to the
// route or the end-of-path rule invalidates it, and leaving it would strand a
// finished Once platform or resume a rerouted one mid-path.
void ResetTraversalState(MovingPlatform& platform)
{
    platform.distance = 0.0f;
    platform.pauseRemaining = 0.0f;
    platform.direction = 1;
    platform.finished = false;
}

// A platform is moved with PhysicsWorld::MoveKinematic, which needs a kinematic
// body on the same entity; without one the component silently does nothing.
void AddBodyRequirementNotice(UIElement* parent, ECS::World* world, ECS::EntityHandle entity)
{
    const auto* body = world->GetComponent<Components::PhysicsBody>(entity);
    if (body && body->motionType == Physics::MotionType::Kinematic)
        return;

    const char* text = body
        ? "This entity's PhysicsBody is not Kinematic. A moving platform is driven by the "
          "physics world, so set Motion Type to Kinematic or it will not move."
        : "No PhysicsBody on this entity. A moving platform needs one with Motion Type set to "
          "Kinematic — that is what carries riders through contact friction.";
    parent->AddChild(std::make_unique<EditorUI::InspectorNotice>(text));
}

void BuildMovingPlatformInspector(const InspectorContext& ctx)
{
    ECS::World* world = ctx.World;
    const ECS::EntityHandle entity = ctx.Entity;
    const auto* platform = world->GetComponent<MovingPlatform>(entity);
    if (!platform)
        return;

    const std::vector<ECS::EntityHandle> extras = InspectorDrag::GetAdditionalEntities(ctx);
    Editor::EditorChangeNotifications* changes = ctx.ChangeNotifications;
    Editor::UndoRedoService* undo = ctx.Undo;

    AddBodyRequirementNotice(ctx.Parent, world, entity);

    {
        auto handlers =
            InspectorDrag::MakeComponentInteractiveHandlers<MovingPlatform, ECS::EntityHandle>(
                world, entity, changes, undo, "Moving Platform Path",
                [](MovingPlatform& p, ECS::EntityHandle h)
                {
                    p.path = h;
                    ResetTraversalState(p);
                },
                extras);
        InspectorUI::AddEntityFieldRow(
            ctx.Parent, "Path", platform->path, world, handlers.second,
            {ECS::GetComponentTypeId<Components::SplineComponent>()}, kPathTooltip);
    }

    // Path Mode is built first because it governs the pause row below it; the
    // mode's callback is wired once that row exists.
    EnumField<PlatformPathMode>* modeField = InspectorUI::AddEnumRow(
        ctx.Parent, "Path Mode", kPathModeEntries, platform->pathMode, kPathModeTooltip);

    UIElement* pauseRow = InspectorUI::AddRow(ctx.Parent);
    FloatField* pauseField = nullptr;
    {
        Label* label = InspectorUI::AddLabel(pauseRow, "Endpoint Pause", kEndpointPauseTooltip);
        UIElement* fieldContainer = InspectorUI::AddFieldContainer(pauseRow);
        pauseField = InspectorUI::AddFloat(fieldContainer, platform->endpointPause);
        auto handlers = InspectorDrag::MakeComponentInteractiveHandlers<MovingPlatform, float>(
            world, entity, changes, undo, "Moving Platform Endpoint Pause",
            [](MovingPlatform& p, float v) { p.endpointPause = v; }, extras);
        pauseField->SetOnValueChanging(handlers.first);
        pauseField->SetOnValueChanged(handlers.second);
        InspectorDrag::SetupLabelDragFloat(
            label, pauseField,
            [field = pauseField, changing = handlers.first]() { changing(field->GetValue()); },
            [field = pauseField, changed = handlers.second]() { changed(field->GetValue()); },
            kMinEndpointPause, kMinEndpointPause, kMaxEndpointPause);
    }

    // Row carries the :disabled styling, field refuses the edit — SetEnabled is
    // per-element, so dimming the row alone would still accept typing.
    auto setPauseEnabled = [pauseRow, pauseField](PlatformPathMode mode)
    {
        const bool usable = mode != PlatformPathMode::Once;
        pauseRow->SetEnabled(usable);
        pauseField->SetEnabled(usable);
    };
    setPauseEnabled(platform->pathMode);

    {
        auto handlers =
            InspectorDrag::MakeComponentInteractiveHandlers<MovingPlatform, PlatformPathMode>(
                world, entity, changes, undo, "Moving Platform Path Mode",
                [](MovingPlatform& p, PlatformPathMode v)
                {
                    p.pathMode = v;
                    ResetTraversalState(p);
                },
                extras);
        modeField->SetOnValueChanged(
            [commit = handlers.second, setPauseEnabled](PlatformPathMode v)
            {
                commit(v);
                setPauseEnabled(v);
            });
    }

    InspectorDrag::AddComponentFloatRowWithDrag<MovingPlatform>(
        ctx.Parent, "Speed", platform->speed, world, entity, changes, undo, "Moving Platform Speed",
        [](MovingPlatform& p, float v) { p.speed = v; }, platform->speed, kSpeedTooltip, extras,
        kMinSpeed, kMaxSpeed);
}

} // namespace

void RegisterMovingPlatformInspector()
{
    InspectorRegistry::Get().RegisterComponentInspector<MovingPlatform>(
        [](const InspectorContext& ctx)
        {
            if (!ctx.Parent || !ctx.World || !ctx.Entity.IsValid())
                return;
            BuildMovingPlatformInspector(ctx);
        });

    Editor::EditorComponentTraits traits;
    traits.EnableToggleTooltip = "Off stops driving this platform.";
    Editor::EditorComponentTraitsRegistry::Get().Register(
        ECS::GetComponentTypeId<MovingPlatform>(), std::move(traits));
}

} // namespace GameEngine
