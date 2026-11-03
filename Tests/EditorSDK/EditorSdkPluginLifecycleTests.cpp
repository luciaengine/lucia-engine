#include "Editor/Registries/EditorPluginRegistry.h"
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace
{
using namespace GameEngine;
using namespace GameEngine::Editor;

class LifecyclePlugin final : public IEditorPlugin
{
public:
    explicit LifecyclePlugin(const char* id) : Descriptor{id, "Lifecycle fixture", "1", true} {}
    const Plugins::PluginDescriptor& GetDescriptor() const override { return Descriptor; }
    void OnEditorAttach(const EditorHostServices& host) override
    {
        ++Attached;
        CanWrite = host.CanWriteScene;
    }
    void OnEditorUpdate() override { ++Updated; }
    void OnSceneInvalidated() override { ++Invalidated; }
    void OnEditorShutdown() override { ++Stopped; CanWrite = {}; }
    bool TryInvokeCommand(std::string_view method, const nlohmann::json&, nlohmann::json& result) override
    {
        if (method != "fixture.command") return false;
        result = {{"writable", CanWrite && CanWrite()}};
        return true;
    }
    Plugins::PluginDescriptor Descriptor;
    std::function<bool()> CanWrite;
    int Attached = 0, Updated = 0, Invalidated = 0, Stopped = 0;
};

class EditorPluginLifecycle : public ::testing::Test
{
protected:
    EditorPluginRegistry& Registry = EditorPluginRegistry::Get();
    void SetUp() override { Registry.Shutdown(); }
    void TearDown() override
    {
        Registry.Shutdown();
        Registry.SetEnabledResolver({});
    }
};

TEST_F(EditorPluginLifecycle, LatePackageHasHostServicesAndRunsWithoutAnOpenPanel)
{
    static LifecyclePlugin plugin("fixture.lifecycle.late");
    Registry.SetEnabledResolver([](std::string_view id, bool) { return id == "fixture.lifecycle.late"; });
    bool writable = true;
    Registry.AttachHost({nullptr, {}, [&] { return writable; }});
    Registry.RegisterPlugin(plugin);
    const int starts = plugin.Attached, ticks = plugin.Updated, invalidations = plugin.Invalidated;
    Registry.Update();
    Registry.Update();
    EXPECT_EQ(plugin.Attached, starts + 1);
    EXPECT_EQ(plugin.Updated, ticks + 2);
    nlohmann::json result;
    ASSERT_TRUE(Registry.TryInvokeCommand("fixture.command", {}, result));
    EXPECT_TRUE(result["writable"].get<bool>());
    writable = false;
    Registry.InvalidateScene();
    EXPECT_EQ(plugin.Invalidated, invalidations + 1);
    ASSERT_TRUE(Registry.TryInvokeCommand("fixture.command", {}, result));
    EXPECT_FALSE(result["writable"].get<bool>());
    Registry.Shutdown();
    EXPECT_FALSE(plugin.CanWrite);
    EXPECT_FALSE(Registry.TryInvokeCommand("fixture.command", {}, result));
}

TEST_F(EditorPluginLifecycle, DisableStopsJobsAndReenableAttachesAgain)
{
    static LifecyclePlugin plugin("fixture.lifecycle.toggle");
    bool enabled = false;
    Registry.SetEnabledResolver([&](std::string_view id, bool) {
        return id == "fixture.lifecycle.toggle" && enabled;
    });
    Registry.RegisterPlugin(plugin);
    Registry.AttachHost({});
    const int starts = plugin.Attached, stops = plugin.Stopped;
    Registry.Update();
    EXPECT_EQ(plugin.Attached, starts);
    enabled = true;
    Registry.InvalidateEnabledCache();
    Registry.Update();
    EXPECT_EQ(plugin.Attached, starts + 1);
    enabled = false;
    Registry.InvalidateEnabledCache();
    Registry.Update();
    EXPECT_EQ(plugin.Stopped, stops + 1);
    enabled = true;
    Registry.InvalidateEnabledCache();
    Registry.Update();
    EXPECT_EQ(plugin.Attached, starts + 2);
}

TEST_F(EditorPluginLifecycle, ReplacementStopsTheOldInstanceBeforeTheNewOneRuns)
{
    static LifecyclePlugin oldPlugin("fixture.lifecycle.replace");
    static LifecyclePlugin newPlugin("fixture.lifecycle.replace");
    Registry.SetEnabledResolver([](std::string_view id, bool) { return id == "fixture.lifecycle.replace"; });
    Registry.RegisterPlugin(oldPlugin);
    Registry.AttachHost({});
    Registry.Update();
    const int stops = oldPlugin.Stopped, ticks = oldPlugin.Updated, starts = newPlugin.Attached;
    Registry.RegisterPlugin(newPlugin);
    EXPECT_EQ(oldPlugin.Stopped, stops + 1);
    Registry.Update();
    EXPECT_EQ(newPlugin.Attached, starts + 1);
    EXPECT_EQ(oldPlugin.Updated, ticks);
}
TEST_F(EditorPluginLifecycle, ProjectSwitchStopsAPinnedModuleThatIsNoLongerEnabled)
{
    static LifecyclePlugin plugin("fixture.lifecycle.project");
    Registry.SetEnabledResolver([](std::string_view id, bool) { return id == "fixture.lifecycle.project"; });
    ECS::SetActiveRegistrationModule("Fixture.Optional.Editor", 1);
    Registry.RegisterPlugin(plugin);
    ECS::ClearActiveRegistrationModule();
    bool available = true;
    Registry.AttachHost({nullptr, {}, [] { return true; },
                         [&](std::string_view name) { return available && name == "Fixture.Optional.Editor"; }});
    Registry.Update();
    const int stops = plugin.Stopped, ticks = plugin.Updated;
    available = false;
    nlohmann::json result;
    EXPECT_FALSE(Registry.TryInvokeCommand("fixture.command", {}, result));
    Registry.Update();
    EXPECT_EQ(plugin.Stopped, stops + 1);
    EXPECT_EQ(plugin.Updated, ticks);
}

}
