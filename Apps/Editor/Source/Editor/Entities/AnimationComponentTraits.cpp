#include "Editor/Entities/AnimationComponentTraits.h"

#include "Components/Animation/AnimatedNodeRef.h"
#include "Components/Animation/AnimatedSprite2D.h"
#include "Components/Animation/AnimatorRef.h"
#include "Components/Animation/HumanoidRetargeterComponent.h"
#include "Components/Animation/SkeletonRef.h"
#include "ECS/ECS.h"
#include "Editor/Entities/EditorComponentTraits.h"

#include <utility>

namespace GameEngine::Editor
{

void RegisterAnimationComponentTraits()
{
    constexpr int kAnimatedNodeSortKey = 35;

    {
        EditorComponentTraits traits;
        traits.DisplayName = "Animated Node";
        traits.Category = "Animation";
        traits.SortKey = kAnimatedNodeSortKey;
        traits.InspectorIconClass = "inspector-section-icon-animated-node";
        traits.CollapsedByDefault = true;
        EditorComponentTraitsRegistry::Get().Register(
            ECS::GetComponentTypeId<Components::AnimatedNodeRef>(), std::move(traits));
    }

    // Runtime playback state; the Animator inspector exposes it.
    {
        EditorComponentTraits traits;
        traits.HiddenInInspector = true;
        EditorComponentTraitsRegistry::Get().Register(
            ECS::GetComponentTypeId<Components::AnimatorRef>(), std::move(traits));
    }

    // Internal skeleton handle.
    {
        EditorComponentTraits traits;
        traits.HiddenInInspector = true;
        EditorComponentTraitsRegistry::Get().Register(
            ECS::GetComponentTypeId<Components::SkeletonRef>(), std::move(traits));
    }

    {
        EditorComponentTraits traits;
        traits.Category = "Animation";
        EditorComponentTraitsRegistry::Get().Register(
            ECS::GetComponentTypeId<Components::AnimatedSprite2D>(), std::move(traits));
    }

    {
        EditorComponentTraits traits;
        traits.Category = "Animation";
        EditorComponentTraitsRegistry::Get().Register(
            ECS::GetComponentTypeId<Components::HumanoidRetargeterComponent>(), std::move(traits));
    }
}

} // namespace GameEngine::Editor
