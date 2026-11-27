#pragma once

#include <filesystem>
#include <memory>
#include <string>

namespace GameEngine::ECS
{
class World;
}

namespace GameEngine::Editor
{
class SceneCollaborationSession
{
public:
    SceneCollaborationSession();
    ~SceneCollaborationSession();

    void Update(const std::filesystem::path& project, const std::filesystem::path& scene, ECS::World* world);
    bool Start(const std::filesystem::path& project);
    void Stop();
    bool IsStarted() const;
    std::string TakeNotice();
    bool CanInvite() const;
    std::string InviteLink() const;

private:
    class State;
    class LocalRelay;
    void PollLocalRelay();
    std::unique_ptr<State> m_State;
    std::unique_ptr<LocalRelay> m_LocalRelay;
    std::string m_Notice;
    bool m_Initialized = false;
};
}
