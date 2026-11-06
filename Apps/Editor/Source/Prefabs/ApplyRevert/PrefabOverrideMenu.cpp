#include "Prefabs/PrefabInstanceDescriptions.h"

#include "EditorChangeNotifications.h"
#include "InspectorRegistry.h"
#include "Prefabs/ApplyRevert/ApplyPrefabOverrideCommand.h"
#include "Prefabs/PrefabHierarchyRows.h"
#include "SceneView/SceneViewNoticeOverlay.h"
#include "UI/EditorIcons.h"
#include "UndoRedo/EntityComponentsEdit.h"
#include "UndoRedo/UndoRedoService.h"

#include "ECS/Entity.h"
#include "ECS/World.h"
#include "Scene/PrefabAuthoring.h"
#include "Scene/SceneSchemaRegistry.h"
#include "Types/StringUtils.h"

#include <algorithm>
#include <format>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace GameEngine::Editor
{
namespace
{

// The component of the first override of `entity` among `targets`; 0 when no loaded module registers it.
ECS::ComponentTypeId ComponentTypeOfTargets(std::span<const Scene::PrefabOverrideTarget> targets,
                                            ECS::EntityHandle entity)
{
    for (const Scene::PrefabOverrideTarget& target : targets)
    {
        if (target.Entity != entity)
            continue;
        if (const Scene::ISceneComponentSchema* schema = Scene::SceneSchemaRegistry::Find(target.Override.Component))
            return Scene::SceneSchemaRegistry::ComponentTypeForSchema(*schema);
    }
    return 0;
}

} // namespace

void ApplyPrefabOverridesAsOneStep(ECS::World& world, std::span<const Scene::PrefabOverrideTarget> targets,
                                   UndoRedoService* undo, EditorChangeNotifications* notifications,
                                   const std::string& undoName)
{
    if (targets.empty() || (undo && !undo->CommitPendingEdit()))
        return;
    std::string error;
    std::unique_ptr<ApplyPrefabOverrideCommand> command =
        ApplyPrefabOverrideCommand::Create(world, notifications, targets, undoName, error);
    if (command && undo)
        undo->CommitAlreadyApplied(std::move(command));
    if (!error.empty())
        ShowSceneViewNotice(std::move(error));
}

void RevertPrefabOverridesAsOneStep(ECS::World& world, std::span<const Scene::PrefabOverrideTarget> targets,
                                    UndoRedoService* undo, EditorChangeNotifications* notifications,
                                    const std::string& undoName)
{
    if (targets.empty() || (undo && !undo->CommitPendingEdit()))
        return;
    std::string error;
    // One edit for each entity, in one compound step when the targets span several entities.
    std::vector<ECS::EntityHandle> entities;
    for (const Scene::PrefabOverrideTarget& target : targets)
        if (std::find(entities.begin(), entities.end(), target.Entity) == entities.end())
            entities.push_back(target.Entity);
    const bool compound = undo && entities.size() > 1;
    if (compound)
        undo->BeginCompound(undoName);
    for (const ECS::EntityHandle entity : entities)
    {
        if (!world.IsValid(entity))
            continue;
        const auto revert = [&world, entity, targets, &error]()
        {
            for (const Scene::PrefabOverrideTarget& target : targets)
                if (target.Entity == entity && !Scene::RevertPrefabOverride(world, entity, target.Override, error))
                    return false;
            return true;
        };
        CommitEntityComponentsEdit(world, entity, undo, notifications, undoName, revert);
        if (notifications)
            notifications->NotifyComponentChanged({&world, entity, ComponentTypeOfTargets(targets, entity),
                                                   EditorChangeNotifications::ChangeKind::Commit});
    }
    if (compound)
        undo->EndCompound();
    if (!error.empty())
        ShowSceneViewNotice(std::move(error));
}

} // namespace GameEngine::Editor
