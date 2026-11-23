// The ocean reads its author maps (flow maps, wave masks) through the AssetManager. A map that the
// AssetManager cannot load is not read from its file again in each update.

#include <gtest/gtest.h>

#include "Assets/AssetManager.h"
#include "Components/Rendering/Ocean.h"
#include "Components/Transform.h"
#include "Core/Application.h"
#include "Core/Engine.h"
#include "ECS/ECS.h"
#include "ECS/ECSTemplates.h"
#include "Engine/Rendering/RenderServices.h"
#include "EngineLogCapture.h"
#include "Ocean/Systems/OceanExtractionSystem.h"
#include "TestTempDir.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace GameEngine;
using GameEngine::Ocean::OceanExtractionSystem;

namespace
{
namespace fs = std::filesystem;

constexpr int kUpdates = 5;

class OceanAuthorMapLoad : public testing::Test
{
  protected:
    void SetUp() override
    {
        m_Workspace = TestUtils::MakeUniqueTempDirectory("ge-ocean-author-map");
        fs::create_directories(m_Workspace / "Assets");
        // EngineCore::Initialize makes the workspace the working directory.
        m_PreviousDirectory = fs::current_path();
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
    }

    void TearDown() override
    {
        EngineCore::GetInstance().Shutdown();
        std::error_code error;
        fs::current_path(m_PreviousDirectory, error);
        fs::remove_all(m_Workspace, error);
    }

    fs::path m_Workspace;
    fs::path m_PreviousDirectory;
};

TEST_F(OceanAuthorMapLoad, AWaveMaskThatDoesNotLoadIsNotReadAgainInEachUpdate)
{
    const fs::path maskPath = m_Workspace / "Assets" / "mask.png";
    std::ofstream(maskPath, std::ios::binary) << "not an image";
    const GUID maskGuid = EngineCore::GetInstance().GetAssetManager().ResolveAssetGuid(maskPath);
    ASSERT_FALSE(maskGuid.IsNull());

    ECS::World world;
    world.AddComponentImmediate(world.CreateEntity(), Components::OceanSurface{});
    const ECS::EntityHandle mask = world.CreateEntity();
    world.AddComponentImmediate(mask, Components::WorldTransform{});
    Components::OceanWaveMaskTextureSource source{};
    source.WaveMaskMap = Components::TextureRef(maskGuid);
    world.AddComponentImmediate(mask, source);

    Engine::Renderer::RenderServices rs;
    OceanExtractionSystem system(&rs);
    // The first update asks the AssetManager for the map, and that load fails.
    system.Update(world, 1.0f / 60.0f);

    std::vector<std::string> lines;
    {
        TestLog::ScopedEngineLogCapture capture(&lines, Logger::LogLevel::Debug);
        Logger::Log::Debug("ocean author map capture is live");
        for (int update = 0; update < kUpdates; ++update)
            system.Update(world, 1.0f / 60.0f);
        Logger::Log::Flush();
    }
    ASSERT_EQ(TestLog::CountLinesContaining(lines, "ocean author map capture is live"), 1u);
    // TextureAsset::Load logs this line for each read of a file. The AssetManager decodes from
    // memory and logs "Loading texture from memory data" instead.
    EXPECT_EQ(TestLog::CountLinesContaining(lines, "Loading texture: "), 0u)
        << "the map was decoded from its file again in an update";
}

} // namespace
