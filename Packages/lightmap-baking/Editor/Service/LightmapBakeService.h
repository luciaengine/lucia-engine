#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <nlohmann/json_fwd.hpp>

namespace GameEngine::Editor
{
class EditorChangeNotifications;

// Main-thread editor service. UI and automation share one job; background work
// owns immutable snapshots. Poll also imports and applies completed outputs.
class LightmapBakeService
{
public:
    explicit LightmapBakeService(EditorChangeNotifications* notifications,
                                std::function<bool()> canBake = {});
    ~LightmapBakeService();
    nlohmann::json Start(const nlohmann::json& parameters);
    nlohmann::json Poll();
    void Cancel();
    void InvalidateScene();
    void Shutdown();
    uint64_t SceneVersion() const { return m_SceneVersion; }

private:
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
    EditorChangeNotifications* m_Notifications;
    std::function<bool()> m_CanBake;
    uint64_t m_SceneVersion = 0;
};
}
