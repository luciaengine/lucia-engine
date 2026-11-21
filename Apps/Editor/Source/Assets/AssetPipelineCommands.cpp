#include "Assets/AssetPipelineCommands.h"

#include "Assets/HlodBakeDriver.h"
#include "Assets/ProjectLodReimport.h"
#include "Automation/UiReplayCommandIds.h"
#include "Core/Engine.h"
#include "Editor/Registries/EditorMenuRegistry.h"
#include "SceneLoadTracker/SceneLoadTracker.h"

#include <algorithm>
#include <string>
#include <utility>

namespace GameEngine::Editor
{
namespace
{

bool InvokeReimportProjectLods(std::string*)
{
    ReimportProjectLods(EngineCore::GetInstance().GetAssetManager());
    return true;
}

bool InvokeBakeHlodProject(const ActiveScenePathProvider& activeScenePath, std::string* outError)
{
    auto& engine = EngineCore::GetInstance();
    ECS::World* world = engine.GetPrimaryWorld();
    if (!world)
    {
        if (outError)
            *outError = "no primary world";
        return false;
    }
    const std::optional<std::filesystem::path> scenePath =
        activeScenePath ? activeScenePath() : std::nullopt;
    if (!scenePath)
    {
        if (outError)
            *outError = "no active scene (save the scene first)";
        return false;
    }
    const auto& categories = engine.GetSceneLoadTracker().GetStatus().Categories;
    if (std::ranges::any_of(categories, [](const auto& category)
        { return category.Id == "scene.resolve" && category.HoldsReady(); }))
    {
        if (outError)
            *outError = "Scene models and materials are still loading. Wait for them before baking HLOD.";
        return false;
    }
    return Hlod::BakeHlodForScene(engine.GetAssetManager(), *world, *scenePath, outError);
}

} // namespace

void RegisterAssetPipelineCommands(ActiveScenePathProvider activeScenePath)
{
    auto& registry = EditorMenuRegistry::Get();
    registry.RegisterCommand({UiReplayCommandIds::ReimportProjectLODs, &InvokeReimportProjectLods});
    registry.RegisterCommand(
        {UiReplayCommandIds::BakeHlodProject,
         [activeScenePath = std::move(activeScenePath)](std::string* outError) {
             return InvokeBakeHlodProject(activeScenePath, outError);
         }});
}

} // namespace GameEngine::Editor
