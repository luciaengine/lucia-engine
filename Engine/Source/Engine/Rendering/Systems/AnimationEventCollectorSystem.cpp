#include "ECSModules/Rendering/Systems/AnimationEventCollectorSystem.h"

#include "Animation/AnimationEventCollectorStore.h"
#include "Components/Animation/Animator.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"

namespace GameEngine { namespace Engine::Renderer {

void AnimationEventCollectorSystem::Update(ECS::World& world, float32 /*deltaTime*/)
{
    auto& store = Animation::AnimationEventCollectorStore::Instance();
    world.Query<ECS::Write<Components::Animator>>().Each([&](ECS::Writable<Components::Animator> animatorWrite) {
        const auto& animator = animatorWrite.Get();
        if (Animation::AnimationEventCollector* collector = store.Get(animator.eventCollectorId))
            collector->Clear();
        else
            animatorWrite.Mutable().eventCollectorId = store.Create();
    });
}

} } // namespace GameEngine::Engine::Renderer
