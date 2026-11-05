#include "Engine/Rendering/2D/Systems/SpriteAnimationSystem.h"

#include "Assets/AssetManager.h"
#include "Components/Animation/AnimatedSprite2D.h"
#include "Components/Transform.h"
#include "Core/Application.h"
#include "Core/Engine.h"
#include "ECS/ECS.h"
#include "Engine/Rendering/RenderServices.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <gtest/gtest.h>

using namespace GameEngine;
using namespace GameEngine::Engine::Renderer;

namespace
{
class SpriteAnimationWrites : public testing::Test
{
protected:
    void SetUp() override
    {
        m_OriginalDirectory = std::filesystem::current_path();
        m_Workspace = std::filesystem::temp_directory_path() / ("ge-sprite-stamps-" + GUID::Generate().ToString());
        std::filesystem::create_directories(m_Workspace / "Assets");
        auto& engine = EngineCore::GetInstance();
        ScriptsConfig scripts;
        scripts.disableClr = true;
        scripts.enableHotReload = false;
        scripts.enableAsyncHotReload = false;
        scripts.enableAutoProjectGeneration = false;
        engine.SetScriptsConfig(scripts);
        ApplicationConfig config;
        config.WorkspaceDirectory = m_Workspace.string();
        config.EnableEditor = false;
        ASSERT_TRUE(engine.Initialize(config));

        const auto path = m_Workspace / "Assets" / "idle.spriteframes";
        std::ofstream(path) << R"({"schemaVersion":1,"animations":[{"name":"Idle","loop":false,
            "frames":[{"durationSeconds":0.1},{"durationSeconds":0.1}]},
            {"name":"Loop","loop":true,"frames":[{"durationSeconds":0.1},{"durationSeconds":0.1}]}]})";
        auto& assets = engine.GetAssetManager();
        const auto registered = assets.GetRegistry().RegisterAssetByPath(path);
        ASSERT_TRUE(registered.IsOk());
        m_Frames = registered.Value();
        auto completed = std::make_shared<std::promise<bool>>();
        auto ready = completed->get_future();
        assets.LoadAsset(m_Frames, [completed](auto result) { completed->set_value(result.IsOk()); });
        ASSERT_EQ(ready.wait_for(std::chrono::seconds(10)), std::future_status::ready);
        ASSERT_TRUE(ready.get());
    }

    void TearDown() override
    {
        EngineCore::GetInstance().Shutdown();
        std::error_code error;
        std::filesystem::current_path(m_OriginalDirectory, error);
        EXPECT_FALSE(error);
        if (!error)
            std::filesystem::remove_all(m_Workspace, error);
    }

    ECS::Entity AddSprite(Components::AnimatedSprite2D sprite)
    {
        auto entity = m_World.Create();
        entity.Set(sprite);
        entity.Set(Components::WorldTransform{});
        m_World.ProcessCommands();
        return entity;
    }

    Components::AnimatedSprite2D Sprite() const
    {
        Components::AnimatedSprite2D sprite;
        sprite.SpriteFrames.Set(m_Frames);
        sprite.SetAnimationName("Idle");
        return sprite;
    }

    size_t UpdateAndCountChanged()
    {
        const uint64 version = m_World.GetGlobalSystemVersion();
        SpriteAnimationSystem system(&m_Services);
        for (int32 frame = 0; frame < 3; ++frame)
            system.Update(m_World, 0.02f);
        ECS::ChangeGate gate;
        gate.LastRunVersion = version;
        size_t changed = 0;
        m_World.Query<ECS::Read<Components::AnimatedSprite2D>>()
            .Changed<Components::AnimatedSprite2D>(gate)
            .BatchEach([&changed](const Components::AnimatedSprite2D*, size_t count) { changed += count; });
        return changed;
    }

    ECS::World m_World;
    RenderServices m_Services;

private:
    GUID m_Frames;
    std::filesystem::path m_Workspace;
    std::filesystem::path m_OriginalDirectory;
};
}

TEST_F(SpriteAnimationWrites, PausedSpriteLeavesChangeTrackingQuiet)
{
    auto sprite = Sprite();
    sprite.Playing = false;
    AddSprite(sprite);
    EXPECT_EQ(UpdateAndCountChanged(), 0u);
}

TEST_F(SpriteAnimationWrites, ZeroSpeedLeavesChangeTrackingQuiet)
{
    auto sprite = Sprite();
    sprite.SpeedScale = 0.0f;
    AddSprite(sprite);
    EXPECT_EQ(UpdateAndCountChanged(), 0u);
}

TEST_F(SpriteAnimationWrites, FinishedClockLeavesChangeTrackingQuiet)
{
    auto sprite = Sprite();
    sprite.TimeSeconds = 0.2f;
    auto entity = AddSprite(sprite);
    SpriteAnimationSystem system(&m_Services);
    system.Update(m_World, 0.1f);
    EXPECT_FALSE(entity.Get<Components::AnimatedSprite2D>()->Playing);
    system.Update(m_World, 0.1f);
    EXPECT_EQ(UpdateAndCountChanged(), 0u);
}

TEST_F(SpriteAnimationWrites, UnresolvedAnimationLeavesChangeTrackingQuiet)
{
    auto sprite = Sprite();
    sprite.SetAnimationName("Missing");
    AddSprite(sprite);
    EXPECT_EQ(UpdateAndCountChanged(), 0u);
}

TEST_F(SpriteAnimationWrites, PlayingSpriteStillAdvancesAndPublishesAChange)
{
    auto entity = AddSprite(Sprite());
    EXPECT_EQ(UpdateAndCountChanged(), 1u);
    const auto* sprite = entity.Get<Components::AnimatedSprite2D>();
    ASSERT_NE(sprite, nullptr);
    EXPECT_NEAR(sprite->TimeSeconds, 0.06f, 1.0e-6f);
}

TEST_F(SpriteAnimationWrites, OneShotPublishesCompletionOnceAndHoldsItsLastFrame)
{
    auto entity = AddSprite(Sprite());
    SpriteAnimationSystem system(&m_Services);
    system.Update(m_World, 0.25f);
    const auto* sprite = entity.Get<Components::AnimatedSprite2D>();
    ASSERT_NE(sprite, nullptr);
    EXPECT_FALSE(sprite->Playing);
    EXPECT_FLOAT_EQ(sprite->TimeSeconds, 0.2f);
    EXPECT_EQ(sprite->PlaybackEventFrame, 1u);
    EXPECT_EQ(sprite->PendingPlaybackEvents, 5u);
    system.Update(m_World, 0.25f);
    EXPECT_EQ(sprite->PendingPlaybackEvents, 0u);
    EXPECT_EQ(UpdateAndCountChanged(), 0u);
}

TEST_F(SpriteAnimationWrites, LoopEventsAdvanceHiddenSpritesAndExpireAtTheNextUpdate)
{
    auto initial = Sprite();
    initial.SetAnimationName("Loop");
    initial.Visible = false;
    initial.SpeedScale = -1;
    auto entity = AddSprite(initial);
    SpriteAnimationSystem system(&m_Services);
    system.Update(m_World, 0.45f);
    const auto* sprite = entity.Get<Components::AnimatedSprite2D>();
    ASSERT_NE(sprite, nullptr);
    EXPECT_TRUE(sprite->Playing);
    EXPECT_EQ(sprite->PendingPlaybackEvents, 3u);
    EXPECT_EQ(sprite->PlaybackEventLoops, -3);
    EXPECT_EQ(sprite->PlaybackEventFrame, 1u);
    auto paused = *sprite;
    paused.Playing = false;
    entity.Set(paused);
    m_World.ProcessCommands();
    system.Update(m_World, 0.1f);
    EXPECT_EQ(entity.Get<Components::AnimatedSprite2D>()->PendingPlaybackEvents, 0u);
}
