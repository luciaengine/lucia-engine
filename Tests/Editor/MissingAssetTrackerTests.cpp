// The missing-asset tracker queues one rescan per frame for a changed-file batch that names a GUID
// the scene references or a deleted file, and notifies only when the set of missing assets changed.

#include <gtest/gtest.h>

#include "AssetCore/Asset.h"
#include "AssetCore/AssetTypes.h"
#include "AssetCore/GUID.h"
#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Components/GameLogicGraphRef.h"
#include "Core/DeferredActionQueue.h"
#include "Core/Engine.h"
#include "ECS/ECSTemplates.h"
#include "EditorContext.h"
#include "MissingAssetTracker.h"

#include <span>

using namespace GameEngine;

namespace
{

EngineCore& StartedEngine()
{
    EngineCore& engine = EngineCore::GetInstance();
    if (!engine.IsInitialized())
    {
        ApplicationConfig config{};
        config.AssetDirectory = ".";
        config.WorkspaceDirectory = ".";
        config.EnableEditor = true;
        EXPECT_TRUE(engine.Initialize(config));
    }
    return engine;
}

// A graph in the registry, with no file behind it: the tracker reads metadata only.
GUID RegisterGraph(AssetRegistry& registry, const std::string& name)
{
    AssetMetadata meta{};
    meta.Guid = GUID::Generate();
    meta.Type = AssetType::Graph;
    meta.Name = name;
    meta.Extension = ".graph";
    meta.Path = registry.GetAssetRoot() / "Graphs" / (name + ".graph");
    EXPECT_TRUE(registry.RegisterAssetMetadata(meta));
    return meta.Guid;
}

// A scene with one entity that references `graph`, and a tracker bound to it after one scan.
struct ScannedScene
{
    ECS::World World;
    DeferredActionQueue PreUiActions;
    EditorContext Context{};
    MissingAssetTracker Tracker;
    int Notifications = 0;
    MissingAssetTracker::Subscription Subscription;

    ScannedScene(AssetManager& assets, const GUID& graph)
    {
        Components::GameLogicGraphRef reference{};
        reference.graphGuid = Components::AssetRef<AssetType::Graph>(graph);
        World.AddComponentImmediate(World.CreateEntity(), reference);
        Context.World = &World;
        Context.Assets = &assets;
        Tracker.Bind(&Context, &PreUiActions);
        Tracker.RescanActive();
        Subscription = Tracker.AddListener([this]() { ++Notifications; });
    }
};

} // namespace

TEST(MissingAssetTrackerTests, ABatchOfUnreferencedFilesNeitherRescansNorNotifies)
{
    AssetManager& assets = StartedEngine().GetAssetManager();
    ScannedScene scene(assets, RegisterGraph(assets.GetRegistry(), "Patrol"));
    const uint32_t version = scene.Tracker.GetVersion();

    const GUID unrelated = RegisterGraph(assets.GetRegistry(), "Unrelated");
    assets.FilesChanged.Invoke(std::span<const GUID>(&unrelated, 1));
    scene.PreUiActions.FlushOnce();

    EXPECT_EQ(scene.Notifications, 0);
    EXPECT_EQ(scene.Tracker.GetVersion(), version) << "an unrelated batch does not walk the scene";
}

TEST(MissingAssetTrackerTests, RemovingAReferencedAssetNotifiesOnce)
{
    AssetManager& assets = StartedEngine().GetAssetManager();
    const GUID graph = RegisterGraph(assets.GetRegistry(), "Guard");
    ScannedScene scene(assets, graph);

    assets.GetRegistry().UnregisterAsset(graph);
    assets.FilesChanged.Invoke(std::span<const GUID>(&graph, 1));
    scene.PreUiActions.FlushOnce();
    assets.FilesChanged.Invoke(std::span<const GUID>(&graph, 1));
    scene.PreUiActions.FlushOnce();

    EXPECT_EQ(scene.Notifications, 1) << "the second batch leaves the missing set as it is";
    EXPECT_TRUE(scene.Tracker.IsAssetMissing(graph));
}

TEST(MissingAssetTrackerTests, TwoBatchesInOneFrameRescanOnce)
{
    AssetManager& assets = StartedEngine().GetAssetManager();
    const GUID graph = RegisterGraph(assets.GetRegistry(), "Sentry");
    ScannedScene scene(assets, graph);
    const uint32_t version = scene.Tracker.GetVersion();

    assets.GetRegistry().UnregisterAsset(graph);
    assets.FilesChanged.Invoke(std::span<const GUID>(&graph, 1));
    assets.FilesChanged.Invoke(std::span<const GUID>(&graph, 1));
    EXPECT_EQ(scene.Tracker.GetVersion(), version) << "the rescan waits for the frame's queue";
    scene.PreUiActions.FlushOnce();

    EXPECT_EQ(scene.Tracker.GetVersion(), version + 1);
    EXPECT_EQ(scene.Notifications, 1);
}

TEST(MissingAssetTrackerTests, AReferenceAddedAfterTheScanIsFoundWhenItsAssetIsDeleted)
{
    AssetManager& assets = StartedEngine().GetAssetManager();
    ScannedScene scene(assets, RegisterGraph(assets.GetRegistry(), "Watch"));

    // A reference assigned in the Inspector changes no world structure, so no rescan runs.
    const GUID assigned = RegisterGraph(assets.GetRegistry(), "Assigned");
    Components::GameLogicGraphRef reference{};
    reference.graphGuid = Components::AssetRef<AssetType::Graph>(assigned);
    scene.World.AddComponentImmediate(scene.World.CreateEntity(), reference);

    assets.GetRegistry().UnregisterAsset(assigned);
    assets.FilesChanged.Invoke(std::span<const GUID>(&assigned, 1));
    scene.PreUiActions.FlushOnce();

    EXPECT_EQ(scene.Notifications, 1);
    EXPECT_TRUE(scene.Tracker.IsAssetMissing(assigned));
}
