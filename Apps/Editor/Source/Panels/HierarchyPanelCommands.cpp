#include "Panels/HierarchyPanel.h"

#include "Editor/Registries/EditorMenuRegistry.h"
#include "Panels/HierarchyCommandIds.h"

#include <cstdint>
#include <string>

namespace GameEngine
{
namespace
{

// Every fixed id HandleCommand runs; the recent-scene ids are registered from
// kCmdLoadRecentSceneBase.
constexpr std::uint32_t kRegisteredCommands[] = {
    HierarchyCommandIds::kCmdCreateEmptyEntity,
    HierarchyCommandIds::kCmdCreateAsEmptyParent,
    HierarchyCommandIds::kCmdCreateShapeCube,
    HierarchyCommandIds::kCmdCreateShapeSphere,
    HierarchyCommandIds::kCmdCreateShapeCapsule,
    HierarchyCommandIds::kCmdCreateShapePlane,
    HierarchyCommandIds::kCmdCreateCamera,
    HierarchyCommandIds::kCmdCreateLightDirectional,
    HierarchyCommandIds::kCmdCreateLightPoint,
    HierarchyCommandIds::kCmdCreateLightSpot,
    HierarchyCommandIds::kCmdCreateLightAmbient,
    HierarchyCommandIds::kCmdCreateLightArea,
    HierarchyCommandIds::kCmdCreateTerrain,
    HierarchyCommandIds::kCmdCreateTerrainLarge,
    HierarchyCommandIds::kCmdCreatePlanet5km,
    HierarchyCommandIds::kCmdCreatePlanet50km,
    HierarchyCommandIds::kCmdCreatePostProcessVolume,
    HierarchyCommandIds::kCmdCreateOcean,
    HierarchyCommandIds::kCmdCreateParticleEmitter,
    HierarchyCommandIds::kCmdCreateWindVolume,
    HierarchyCommandIds::kCmdCreateReflectionProbe,
    HierarchyCommandIds::kCmdCreateSkyEnvironment,
    HierarchyCommandIds::kCmdCreateDDGIVolume,
    HierarchyCommandIds::kCmdNewScene,
    HierarchyCommandIds::kCmdDuplicate,
    HierarchyCommandIds::kCmdSortCustom,
    HierarchyCommandIds::kCmdSortAlphabetical,
    HierarchyCommandIds::kCmdSortType,
    HierarchyCommandIds::kCmdSortAscending,
    HierarchyCommandIds::kCmdSortDescending,
    HierarchyCommandIds::kCmdShowRenderLayer,
    HierarchyCommandIds::kCmdShowSortingLayer,
    HierarchyCommandIds::kCmdShowVcsSceneDiffDots,
    HierarchyCommandIds::kCmdShowMeshLocation,
    HierarchyCommandIds::kCmdAddToBookmarks,
    HierarchyCommandIds::kCmdLoadRecentSceneEmpty,
};

} // namespace

void HierarchyPanel::RegisterCommands()
{
    for (const std::uint32_t commandId : kRegisteredCommands)
        RegisterCommand(commandId);
    for (std::uint32_t index = 0; index < HierarchyCommandIds::kCmdLoadRecentSceneCount; ++index)
        RegisterCommand(HierarchyCommandIds::kCmdLoadRecentSceneBase + index);
}

void HierarchyPanel::RegisterCommand(std::uint32_t commandId)
{
    Editor::EditorMenuRegistry::Get().RegisterCommand(
        {commandId, [this, commandId](std::string* outError) {
             if (HandleCommand(commandId))
                 return true;
             // A built-in command refuses only when the panel has no world.
             if (outError)
                 *outError = "the Hierarchy has no world to run command " + std::to_string(commandId) + " in";
             return false;
         }});
}

} // namespace GameEngine
