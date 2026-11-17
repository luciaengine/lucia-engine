#include "PhysicsECS/Systems/CharacterControllerWritebackSystem.h"

#include "PhysicsPoseWriteback.h"

#include "PhysicsECS/PhysicsWorldService.h"
#include "PhysicsECS/Components/CharacterController.h"

#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Query.h"

namespace GameEngine::PhysicsECS
{
void CharacterControllerWritebackSystem::Update(ECS::World& world, float32 /*deltaTime*/)
{
    auto* pw = PhysicsWorldService::TryGet();
    if (!pw)
        return;

    uint32 composed = 0;
    world.Query<ECS::Read<GameEngine::Components::Transform>,
                ECS::Read<GameEngine::Components::WorldTransform>,
                ECS::Read<GameEngine::Components::CharacterController>>()
        .Each([&](ECS::EntityHandle e,
                  const GameEngine::Components::Transform& t,
                  const GameEngine::Components::WorldTransform& wt,
                  const GameEngine::Components::CharacterController& cc)
              {
                  if (!cc.initialized || !pw->IsCharacterValid(cc.character))
                      return;

                  // The controller is taken for write only when its ground state or record changes, so
                  // an idle character leaves its column version alone.
                  const bool grounded = pw->IsCharacterGrounded(cc.character);
                  const Physics::Vector3 n = pw->GetCharacterGroundNormal(cc.character);
                  if (grounded != cc.grounded || n.x != cc.groundNormalX || n.y != cc.groundNormalY ||
                      n.z != cc.groundNormalZ)
                  {
                      auto& written = *world.GetComponentForWrite<GameEngine::Components::CharacterController>(e);
                      written.grounded = grounded;
                      written.groundNormalX = n.x;
                      written.groundNormalY = n.y;
                      written.groundNormalZ = n.z;
                  }

                  if (const auto record = WritePhysicsPose(world, e, pw->GetCharacterTransform(cc.character),
                                                           nullptr, 1.0f, t, wt, cc.WritebackRecord))
                  {
                      world.GetComponentForWrite<GameEngine::Components::CharacterController>(e)->WritebackRecord =
                          *record;
                      ++composed;
                  }
              });
    PhysicsWorldService::SetLastCharactersComposed(composed);
}
} // namespace GameEngine::PhysicsECS
