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
    void Stop();
    bool CanInvite() const;
    std::string InviteLink() const;

private:
    class State;
    std::unique_ptr<State> m_State;
    bool m_Initialized = false;
};
}
