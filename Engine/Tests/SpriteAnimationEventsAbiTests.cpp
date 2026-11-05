#include "Scripting/SpriteAnimationABI.h"

#include "Components/Animation/AnimatedSprite2D.h"
#include "Core/Engine.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "JobSystem/JobCounter.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Scripting/ScriptManager.h"

#include <chrono>
#include <future>
#include <gtest/gtest.h>

using namespace GameEngine;

namespace
{
GE_Handle WorldHandle(ECS::World& world)
{
    return static_cast<GE_Handle>(reinterpret_cast<uintptr_t>(&world));
}
}

TEST(SpriteAnimationEventsAbi, PollConsumesTheSnapshotOnceAndEmptyPollsDoNotWrite)
{
    ECS::World world;
    Components::AnimatedSprite2D sprite;
    sprite.PendingPlaybackEvents = 3;
    sprite.PlaybackEventFrame = 7;
    sprite.PlaybackEventLoops = -2;
    sprite.TimeSeconds = 0.25f;
    auto entity = world.Create(sprite);
    world.ProcessCommands();
    GE_SpriteAnimationEvents events{};
    ASSERT_EQ(GE_SpriteAnimation_PollEvents(WorldHandle(world), entity.GetHandle().id, &events), GE_Result_Ok);
    EXPECT_EQ(events.flags, 3u);
    EXPECT_EQ(events.frameIndex, 7u);
    EXPECT_EQ(events.loopCount, -2);
    EXPECT_FLOAT_EQ(events.timeSeconds, 0.25f);
    EXPECT_EQ(entity.Get<Components::AnimatedSprite2D>()->PendingPlaybackEvents, 0u);
    ECS::ChangeGate gate;
    gate.LastRunVersion = world.GetGlobalSystemVersion();
    ASSERT_EQ(GE_SpriteAnimation_PollEvents(WorldHandle(world), entity.GetHandle().id, &events), GE_Result_Ok);
    EXPECT_EQ(events.flags, 0u);
    size_t changed = 0;
    world.Query<ECS::Read<Components::AnimatedSprite2D>>().Changed<Components::AnimatedSprite2D>(gate)
        .BatchEach([&changed](const Components::AnimatedSprite2D*, size_t count) { changed += count; });
    EXPECT_EQ(changed, 0u);
}

TEST(SpriteAnimationEventsAbi, InvalidArgumentsAndDeadEntitiesHaveNoPayload)
{
    ECS::World world;
    auto entity = world.Create(Components::AnimatedSprite2D{});
    world.ProcessCommands();
    const auto handle = WorldHandle(world);
    const auto id = entity.GetHandle().id;
    GE_SpriteAnimationEvents events{7, 7, 7, 7};
    EXPECT_EQ(GE_SpriteAnimation_PollEvents(handle, id, nullptr), GE_Result_InvalidArg);
    EXPECT_EQ(GE_SpriteAnimation_PollEvents(0, id, &events), GE_Result_InvalidArg);
    EXPECT_EQ(events.flags, 0u);
    EXPECT_EQ(GE_SpriteAnimation_PollEvents(handle, uint64{id} | (uint64{1} << 32), &events), GE_Result_InvalidArg);
    world.DestroyEntityImmediate(entity.GetHandle());
    EXPECT_EQ(GE_SpriteAnimation_PollEvents(handle, id, &events), GE_Result_NotFound);
    EXPECT_EQ(events.flags, 0u);
    auto empty = world.Create();
    world.ProcessCommands();
    EXPECT_EQ(GE_SpriteAnimation_PollEvents(handle, empty.GetHandle().id, &events), GE_Result_NotFound);
}

TEST(SpriteAnimationEventsAbi, WorkerPollIsRefusedAndLeavesEventsForTheMainThread)
{
    EngineCore::GetInstance().GetScriptManager().MarkMainThread();
    ECS::World world;
    Components::AnimatedSprite2D sprite;
    sprite.PendingPlaybackEvents = 4;
    auto entity = world.Create(sprite);
    world.ProcessCommands();
    const auto handle = WorldHandle(world);
    const auto id = entity.GetHandle().id;
    JobSystem::WorkStealingThreadPool pool(1);
    JobSystem::JobCounter counter;
    auto completed = std::make_shared<std::promise<GE_Result>>();
    auto result = completed->get_future();
    pool.Run([completed, handle, id]
    {
        GE_SpriteAnimationEvents events{};
        completed->set_value(GE_SpriteAnimation_PollEvents(handle, id, &events));
    }, counter);
    ASSERT_EQ(result.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    EXPECT_EQ(result.get(), GE_Result_Fail);
    pool.Wait(counter);
    GE_SpriteAnimationEvents events{};
    ASSERT_EQ(GE_SpriteAnimation_PollEvents(handle, id, &events), GE_Result_Ok);
    EXPECT_EQ(events.flags, 4u);
}
