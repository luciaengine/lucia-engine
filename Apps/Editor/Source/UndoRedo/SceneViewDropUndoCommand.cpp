#include "UndoRedo/SceneViewDropUndoCommand.h"

#include "Components/Name.h"
#include "ECS/ECSTemplates.h"

#include <format>

namespace GameEngine::Editor
{

std::string SceneViewDropUndoCommand::NameFor(const ECS::World& world,
                                            const std::vector<ECS::EntityHandle>& droppedRoots)
{
    if (droppedRoots.size() != 1)
        return std::format("Place {} Entities", droppedRoots.size());
    const auto* name = world.GetComponent<Components::Name>(droppedRoots.front());
    return name && !name->View().empty() ? std::format("Place {}", name->View()) : "Place Entity";
}

} // namespace GameEngine::Editor
