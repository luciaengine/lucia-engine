// Scene-build registration of standalone materials and the shader-source read
// it waits for. SceneBuildPump::StepBudgeted registers a material only after a
// tracked worker has read its surface and vertex-modifier sources; the read is
// advisory, so a failed read still registers, the synchronous drain does not
// wait for it, and a read rejected at shutdown is ready at once.
//
// Owns an EngineCore lifecycle (the read runs on its job system and the pump
// loads materials through its AssetManager), so it stays out of the
// EngineCore-free material aggregate. The pump tests need a Vulkan device for
// RenderServices and skip without one.

#include "Assets/AssetManager.h"
#include "Assets/MaterialAsset.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Core/Application.h"
#include "Core/Engine.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "Engine/Rendering/MaterialRegistry.h"
#include "Engine/Rendering/MaterialSystem.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/SceneBuildPump.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Scripting/ScriptsConfig.h"
#include "TestDeviceHelper.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace GameEngine;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace
{
// Occupies every engine worker until Release, so a tracked job stays queued.
class ParkedWorkers
{
  public:
    ParkedWorkers() : m_Released(m_Release.get_future().share())
    {
        auto& pool = EngineCore::GetInstance().GetJobSystem();
        for (std::size_t i = 0; i < pool.GetWorkerCount(); ++i)
            pool.Run([this] { Park(); }, m_Blockers);
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (!AllParked() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
    }
    ~ParkedWorkers()
    {
        Release();
        EngineCore::GetInstance().GetJobSystem().Wait(m_Blockers);
    }
    ParkedWorkers(const ParkedWorkers&) = delete;
    ParkedWorkers& operator=(const ParkedWorkers&) = delete;

    bool AllParked() const
    {
        return m_Parked.load() == EngineCore::GetInstance().GetJobSystem().GetWorkerCount();
    }
    void Release()
    {
        std::call_once(m_ReleaseOnce, [this] { m_Release.set_value(); });
    }

  private:
    void Park()
    {
        ++m_Parked;
        m_Released.wait();
    }

    std::promise<void> m_Release;
    std::shared_future<void> m_Released;
    std::once_flag m_ReleaseOnce;
    std::atomic<std::size_t> m_Parked{0};
    JobSystem::JobCounter m_Blockers;
};

// Releases the parked workers on Disarm, or after two seconds when Disarm has
// not come, so a step that blocks on a queued job fails the test instead of
// hanging it.
class BlockWatchdog
{
  public:
    explicit BlockWatchdog(ParkedWorkers& workers)
        : m_Thread([this, &workers, disarmed = m_Disarm.get_future()] { Watch(workers, disarmed); })
    {
    }
    ~BlockWatchdog()
    {
        if (m_Thread.joinable())
            Disarm();
    }
    BlockWatchdog(const BlockWatchdog&) = delete;
    BlockWatchdog& operator=(const BlockWatchdog&) = delete;

    // True when the watchdog had to release the workers.
    bool Disarm()
    {
        m_Disarm.set_value();
        m_Thread.join();
        return m_Fired;
    }

  private:
    void Watch(ParkedWorkers& workers, const std::future<void>& disarmed)
    {
        m_Fired = disarmed.wait_for(2s) != std::future_status::ready;
        workers.Release();
    }

    std::promise<void> m_Disarm;
    std::atomic<bool> m_Fired{false};
    std::thread m_Thread;
};
} // namespace

namespace GameEngine::Engine::Renderer
{
class MaterialSourcePrefetchTest : public testing::Test
{
  protected:
    static inline fs::path s_Workspace;
    static inline fs::path s_PreviousDirectory;
    Rendering::MaterialBuildContext m_Context;
    std::unique_ptr<Rendering::IDevice> m_Device;
    std::unique_ptr<RenderServices> m_Services;
    std::vector<GUID> m_LoadedMaterials;

    static void SetUpTestSuite()
    {
        s_PreviousDirectory = fs::current_path();
        s_Workspace = fs::temp_directory_path() / ("source-prefetch-" + GUID::Generate().ToString());
        fs::create_directories(s_Workspace / "Assets");
        auto& engine = EngineCore::GetInstance();
        ASSERT_FALSE(engine.IsInitialized());
        ScriptsConfig scripts;
        scripts.disableClr = true;
        scripts.enableHotReload = false;
        scripts.enableAsyncHotReload = false;
        scripts.enableAutoProjectGeneration = false;
        engine.SetScriptsConfig(scripts);
        ApplicationConfig config;
        config.WorkspaceDirectory = s_Workspace.string();
        ASSERT_TRUE(engine.Initialize(config));
    }
    static void TearDownTestSuite()
    {
        EngineCore::GetInstance().Shutdown();
        fs::current_path(s_PreviousDirectory);
        std::error_code error;
        fs::remove_all(s_Workspace, error);
    }

    void SetUp() override
    {
        const fs::path root = s_Workspace / GUID::Generate().ToString();
        m_Context.AdapterShaderDir = root / "adapter";
        m_Context.ProjectRoots = {root / "project"};
        m_Context.CacheRoot = root / "cache";
        fs::create_directories(m_Context.AdapterShaderDir);
        fs::create_directories(m_Context.ProjectRoots[0]);
    }
    void TearDown() override
    {
        auto& assets = EngineCore::GetInstance().GetAssetManager();
        for (const GUID& material : m_LoadedMaterials)
            assets.UnregisterLoadedAsset(material);
        if (m_Services)
        {
            m_Services->Shutdown();
            const MaterialSystem::PrewarmProgress progress = m_Services->Materials().GetPrewarmProgress();
            EXPECT_EQ(progress.submitted, progress.completed) << "shutdown drains every tracked read";
        }
        m_Services.reset();
        if (m_Device)
            m_Device->Shutdown();
        m_Device.reset();
    }

    bool StartRenderServices()
    {
        m_Services = std::make_unique<RenderServices>();
        if (!m_Services->Initialize(m_Device.get()))
            return false;
        m_Services->Materials().SetMaterialBuildContext(m_Context);
        // Registration submits compiles; drain them so a test counts only its reads.
        m_Services->Materials().FlushAsyncMaterialCompiles();
        return true;
    }

    // A standalone material the AssetManager already holds, as a loaded scene
    // leaves it, whose document names `surface`.
    GUID AddStandaloneMaterial(const std::string& surface)
    {
        const GUID guid = GUID::Generate();
        const fs::path path = m_Context.ProjectRoots[0] / (guid.ToString() + ".material");
        auto material = std::make_shared<MaterialAsset>(guid, path);
        const std::string text = R"({"schemaVersion": 3, "materialName": "Prefetch", )"
                                 R"("lightingModel": "StandardPBR", "surfaceShader": ")"
                                 + surface + R"(", "properties": {}})";
        EXPECT_TRUE(material->LoadFromData(std::vector<uint8_t>(text.begin(), text.end())));
        EngineCore::GetInstance().GetAssetManager().RegisterLoadedAsset(guid, material);
        m_LoadedMaterials.push_back(guid);
        return guid;
    }

    static void ReferenceMaterial(ECS::World& world, const GUID& material)
    {
        Components::MeshRenderer renderer{};
        renderer.materialAssetGuid.Set(material);
        world.Create<Components::MeshRenderer>(renderer);
    }

    bool IsRegistered(const GUID& material) const
    {
        return m_Services->Materials().Registry().Find(material) != nullptr;
    }

    void StepUntilComplete(SceneBuildPump& pump, ECS::World& world)
    {
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        while (!pump.IsComplete() && std::chrono::steady_clock::now() < deadline)
        {
            pump.StepBudgeted(world, *m_Services, 5ms);
            std::this_thread::sleep_for(1ms);
        }
    }

    // The one private entry point this suite reaches past the pump for.
    static std::shared_future<void> Prefetch(MaterialSystem& materials, const MaterialDocument& doc)
    {
        return materials.PrefetchMaterialShaderSources(doc, {});
    }
};
} // namespace GameEngine::Engine::Renderer

using GameEngine::Engine::Renderer::MaterialSourcePrefetchTest;
using GameEngine::Engine::Renderer::MaterialSystem;
using GameEngine::Engine::Renderer::SceneBuildPump;

// Both materials wait for their reads across frames; the one whose surface is
// missing still registers once its failed read completes.
TEST_F(MaterialSourcePrefetchTest, BudgetedStepRegistersOnlyAfterSourceReads)
{
    m_Device = CreateVulkanDeviceFast();
    if (!m_Device)
        GTEST_SKIP() << "Vulkan device unavailable";
    ASSERT_TRUE(StartRenderServices());
    std::ofstream(m_Context.ProjectRoots[0] / "surface.glsl") << "// surface";
    const GUID readable = AddStandaloneMaterial("surface.glsl");
    const GUID missing = AddStandaloneMaterial("missing_surface.glsl");
    ECS::World world;
    ReferenceMaterial(world, readable);
    ReferenceMaterial(world, missing);
    const MaterialSystem::PrewarmProgress before = m_Services->Materials().GetPrewarmProgress();

    ParkedWorkers workers;
    ASSERT_TRUE(workers.AllParked());
    BlockWatchdog watchdog(workers);
    SceneBuildPump pump;
    pump.Begin(world, *m_Services);
    std::size_t registeredWhileReading = 0;
    for (int frame = 0; frame < 3; ++frame)
        registeredWhileReading += pump.StepBudgeted(world, *m_Services, 0ms);
    const bool anyRegisteredWhileReading = IsRegistered(readable) || IsRegistered(missing);
    const MaterialSystem::PrewarmProgress reading = m_Services->Materials().GetPrewarmProgress();
    EXPECT_FALSE(watchdog.Disarm()) << "a budgeted step blocked on a queued read";

    EXPECT_EQ(registeredWhileReading, 0u);
    EXPECT_FALSE(anyRegisteredWhileReading);
    EXPECT_EQ(reading.submitted - before.submitted, 2u) << "one tracked read per material";
    EXPECT_EQ(reading.completed, before.completed);
    StepUntilComplete(pump, world);
    EXPECT_TRUE(pump.IsComplete());
    EXPECT_TRUE(IsRegistered(readable));
    EXPECT_TRUE(IsRegistered(missing));
}

// DrainNow runs on the owner thread and must not wait for a read queued behind
// busy workers.
TEST_F(MaterialSourcePrefetchTest, DrainNowRegistersWithoutWaitingForSourceRead)
{
    m_Device = CreateVulkanDeviceFast();
    if (!m_Device)
        GTEST_SKIP() << "Vulkan device unavailable";
    ASSERT_TRUE(StartRenderServices());
    std::ofstream(m_Context.ProjectRoots[0] / "surface.glsl") << "// surface";
    const GUID material = AddStandaloneMaterial("surface.glsl");
    ECS::World world;
    ReferenceMaterial(world, material);

    ParkedWorkers workers;
    ASSERT_TRUE(workers.AllParked());
    BlockWatchdog watchdog(workers);
    SceneBuildPump pump;
    pump.Begin(world, *m_Services);
    const std::size_t registeredByBudgetedStep = pump.StepBudgeted(world, *m_Services, 0ms);
    const std::size_t drained = pump.DrainNow(world, *m_Services);
    const bool registeredBeforeRelease = IsRegistered(material);
    EXPECT_FALSE(watchdog.Disarm()) << "DrainNow waited for a queued read";

    EXPECT_EQ(registeredByBudgetedStep, 0u) << "the budgeted step only starts the read";
    EXPECT_EQ(drained, 1u);
    EXPECT_TRUE(registeredBeforeRelease);
    EXPECT_TRUE(pump.IsComplete());
}

// A read submitted after shutdown began is dropped by the tracked submission;
// its future must be ready at once, or a polling pump would wait forever.
TEST_F(MaterialSourcePrefetchTest, ReadRejectedAtShutdownIsReadyAtOnce)
{
    MaterialSystem materials;
    materials.SetMaterialBuildContext(m_Context);
    materials.ShutdownPhaseA();
    MaterialDocument doc;
    doc.surfaceShader = "surface.glsl";
    const std::shared_future<void> sourceRead = Prefetch(materials, doc);
    EXPECT_EQ(sourceRead.wait_for(0s), std::future_status::ready);
    EXPECT_THROW(sourceRead.get(), std::future_error);
    EXPECT_EQ(materials.GetPrewarmProgress().submitted, 0u);
}
