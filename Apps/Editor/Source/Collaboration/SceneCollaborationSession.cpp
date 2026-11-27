#include "Collaboration/SceneCollaborationSession.h"

#include "Core/Engine.h"
#include "Editor/EditorPaths.h"
#include "Engine/Build/CancellableShellProcess.h"
#include "Platform/Process.h"
#include "JobSystem/JobChannel.h"
#include "Platform/HttpClient.h"
#include "SceneView/SceneViewport/SceneViewport.h"
#include "SceneView/SceneViewport/SceneViewportRegistry.h"
#include "Types/PathUtils.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <utility>

#include <nlohmann/json.hpp>

namespace GameEngine::Editor
{
namespace
{
using Clock = std::chrono::steady_clock;
using Json = nlohmann::json;
using namespace std::chrono_literals;

constexpr auto kPresenceInterval = 67ms;
constexpr auto kRetryInterval = 1s;
constexpr auto kLocalPresenceTimeout = 2s;
constexpr std::size_t kMaxPeers = 12;

std::string Environment(const char* key)
{
    if (const auto* value = std::getenv(key))
        return value;
    return {};
}

bool IsRoomIdentifier(std::string_view name)
{
    constexpr std::string_view characters =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-";
    return !name.empty() && name.size() <= 80 && name.find_first_not_of(characters) == std::string_view::npos;
}

struct Peer
{
    CollaborationCursor Marker;
    std::string Scene;
    std::string View;
};

std::vector<Peer> ReadPeers(const Json& answer, const std::string& localId)
{
    std::vector<Peer> peers;
    const auto list = answer.find("peers");
    if (list == answer.end() || !list->is_array() || list->size() > kMaxPeers)
        return peers;
    for (const auto& entry : *list)
    {
        if (!entry.is_object())
            continue;
        const std::string id = entry.value("id", std::string{});
        const std::string name = entry.value("name", std::string{});
        if (!IsRoomIdentifier(id) || id == localId || name.empty() || name.size() > 192)
            continue;
        const auto cursor = entry.find("cursor");
        if (cursor == entry.end() || !cursor->is_object())
            continue;
        const float x = cursor->value("x", -1.0f);
        const float y = cursor->value("y", -1.0f);
        if (!(x >= 0.0f && x <= 1.0f && y >= 0.0f && y <= 1.0f))
            continue;
        const std::string scene = cursor->value("scene", std::string{});
        const std::string view = cursor->value("view", std::string{});
        if (scene.empty() || scene.size() > 300 || !IsRoomIdentifier(view))
            continue;
        peers.push_back({{id, name, {x, y}}, scene, view});
    }
    return peers;
}

struct NetworkState
{
    std::atomic<bool> Cancel{false};
    std::mutex Mutex;
    std::optional<HttpClient::HttpResponse> Answer;
};

struct Configuration
{
    std::string Relay;
    std::string Room;
    std::string Endpoint;
    std::string Key;
    std::string Name;
};

Configuration MakeConfiguration(std::string relay, std::string room, std::string key, std::string name)
{
    std::string endpoint = relay + "/v1/rooms/" + room;
    return {std::move(relay), std::move(room), std::move(endpoint), std::move(key), std::move(name)};
}

std::string FindRelayRuntime()
{
    if (auto path = Environment("LUCIA_COLLAB_NODE"); !path.empty())
        return path;
#if defined(__APPLE__)
    // Finder launches don't inherit the shell's Node paths.
    const auto home = PathFromUtf8(Environment("HOME"));
    for (const auto& candidate : { home / ".local/node/bin/node", std::filesystem::path("/opt/homebrew/bin/node"),
                                  std::filesystem::path("/usr/local/bin/node") })
    {
        std::error_code error;
        if (std::filesystem::is_regular_file(candidate, error))
            return PathToUtf8(candidate);
    }
#endif
    return "node";
}

struct RelayStartup
{
    CancellableShellProcess Process;
    std::atomic<bool> Cancel{false};
    std::atomic<bool> Exited{false};
    std::mutex Mutex;
    std::optional<Configuration> Ready;
};

std::optional<Configuration> ReadConfiguration()
{
    auto base = Environment("LUCIA_COLLAB_RELAY");
    const auto room = Environment("LUCIA_COLLAB_ROOM");
    auto key = Environment("LUCIA_COLLAB_TOKEN");
    auto name = Environment("LUCIA_COLLAB_NAME");
    while (!base.empty() && base.back() == '/')
        base.pop_back();
    if (!(base.starts_with("https://") || base.starts_with("http://127.0.0.1:")) ||
        base.find_first_of("?#\r\n") != std::string::npos || !IsRoomIdentifier(room) || key.size() < 24 || key.size() > 512 ||
        name.empty() || name.size() > 48 || !HttpClient::IsAvailable())
        return std::nullopt;

    return MakeConfiguration(std::move(base), room, std::move(key), std::move(name));
}
} // namespace

class SceneCollaborationSession::State
{
public:
    State(Configuration config)
        : Config(std::move(config)),
          Channel(EngineCore::GetInstance().GetJobSystem(), {.Name = "Collaboration presence", .MaxRunning = 1}),
          Network(std::make_shared<NetworkState>())
    {
    }

    ~State() { Network->Cancel.store(true); }

    void Send(const char* operation, const Json& request)
    {
        const auto shared = Network;
        const std::string endpoint = Config.Endpoint + '/' + operation;
        const auto key = Config.Key;
        const std::string payload = request.dump(-1, ' ', false, Json::error_handler_t::replace);
        Task = Channel.Submit([shared, endpoint, key, payload] {
            auto answer = HttpClient::PostJson(endpoint, {{"Authorization", "Bearer " + key}}, payload,
                {.Timeout = kRetryInterval, .MaxResponseBytes = 32 * 1024, .Cancellation = &shared->Cancel});
            std::lock_guard lock(shared->Mutex);
            shared->Answer = std::move(answer);
        });
    }

    std::optional<HttpClient::HttpResponse> TakeAnswer()
    {
        std::lock_guard lock(Network->Mutex);
        return std::exchange(Network->Answer, std::nullopt);
    }

    void Receive(Clock::time_point now)
    {
        const auto reply = TakeAnswer();
        if (!reply)
            return;
        Task = {};
        if (reply->statusCode == 401)
        {
            Session.clear();
            LocalId.clear();
            Peers.clear();
            NextPoll = now + kRetryInterval;
            return;
        }
        if (reply->statusCode != 200)
        {
            NextPoll = now + kRetryInterval;
            return;
        }
        const Json answer = Json::parse(reply->body, nullptr, false);
        if (!answer.is_object())
        {
            NextPoll = now + kRetryInterval;
            return;
        }
        try
        {
            if (Session.empty())
            {
                auto session = answer.value("session", std::string{});
                auto id = answer.value("id", std::string{});
                if (IsRoomIdentifier(session) && IsRoomIdentifier(id))
                {
                    Session = std::move(session);
                    LocalId = std::move(id);
                }
            }
            else
            {
                Peers = ReadPeers(answer, LocalId);
                LastPresence = now;
            }
        }
        catch (const Json::exception&)
        {
            NextPoll = now + kRetryInterval;
        }
    }

    Configuration Config;
    JobSystem::JobChannel Channel;
    std::shared_ptr<NetworkState> Network;
    JobSystem::TaskHandle Task;
    std::filesystem::path InitialProject;
    std::filesystem::path ScenePath;
    std::string SceneKey;
    std::string Session;
    std::string LocalId;
    std::vector<Peer> Peers;
    Clock::time_point NextPoll{};
    Clock::time_point LastPresence{};
    uint64_t Sequence = 0;
};

class SceneCollaborationSession::LocalRelay
{
public:
    LocalRelay(std::filesystem::path project, const std::filesystem::path& script, std::string name)
        : Project(std::move(project)),
          Channel(EngineCore::GetInstance().GetJobSystem(), {.Name = "Local collaboration relay", .MaxRunning = 1}),
          Startup(std::make_shared<RelayStartup>()),
          Deadline(Clock::now() + 10s)
    {
        const auto shared = Startup;
        ProcessLaunch launch;
        launch.Executable = FindRelayRuntime();
        launch.Arguments = {PathToUtf8(script), "--editor", std::to_string(Platform::GetCurrentProcessId())};
        // Do not pass a hosted room credential to the owned local relay.
        launch.Environment = {{"LUCIA_COLLAB_TOKEN", std::nullopt}, {"LUCIA_COLLAB_STATE", std::nullopt}};
        (void)Channel.Submit([shared, launch = std::move(launch), name = std::move(name)] {
            if (!shared->Cancel.load())
            {
                (void)shared->Process.Run(launch, [shared] { return shared->Cancel.load(); },
                    [shared, &name](const std::string& line) {
                        const auto ready = Json::parse(line, nullptr, false);
                        if (!ready.is_object())
                            return;
                        try
                        {
                            const int port = ready.value("port", 0);
                            auto room = ready.value("room", std::string{});
                            auto token = ready.value("token", std::string{});
                            if (port <= 0 || port > 65535 || !IsRoomIdentifier(room) || token.size() < 24 ||
                                token.size() > 512)
                                return;
                            std::lock_guard lock(shared->Mutex);
                            shared->Ready = MakeConfiguration("http://127.0.0.1:" + std::to_string(port),
                                std::move(room), std::move(token), name);
                        }
                        catch (const Json::exception&)
                        {
                        }
                    });
            }
            shared->Exited.store(true);
        });
    }

    ~LocalRelay()
    {
        Startup->Cancel.store(true);
        Startup->Process.Kill();
    }

    std::filesystem::path Project;
    JobSystem::JobChannel Channel;
    std::shared_ptr<RelayStartup> Startup;
    Clock::time_point Deadline;
};

bool SceneCollaborationSession::Start(const std::filesystem::path& project)
{
    if (IsStarted() || project.empty())
        return false;
    m_Initialized = true;
    if (auto config = ReadConfiguration())
    {
        m_State = std::make_unique<State>(std::move(*config));
        m_State->InitialProject = project;
        return true;
    }
    if (!HttpClient::IsAvailable())
    {
        m_Notice = "Collaboration is unavailable in this build.";
        return false;
    }

    const auto script = GetEditorGlobalPaths().installToolsRoot / "SceneCollaboration" / "relay.mjs";
    std::error_code error;
    if (!std::filesystem::is_regular_file(script, error))
    {
        m_Notice = "This build does not include the local collaboration relay.";
        return false;
    }

    auto name = Environment("LUCIA_COLLAB_NAME");
    if (name.empty() || name.size() > 48)
        name = "Host";
    m_LocalRelay = std::make_unique<LocalRelay>(project, script, std::move(name));
    return true;
}

void SceneCollaborationSession::PollLocalRelay()
{
    if (!m_LocalRelay)
        return;
    const auto shared = m_LocalRelay->Startup;
    if (shared->Exited.load() || (!m_State && Clock::now() >= m_LocalRelay->Deadline))
    {
        const bool hadSession = m_State != nullptr;
        Stop();
        m_Notice = hadSession ? "The local collaboration relay stopped." :
            "Could not start a local session. Install Node.js or set LUCIA_COLLAB_NODE to its executable.";
        return;
    }
    if (m_State)
        return;
    std::lock_guard lock(shared->Mutex);
    if (auto ready = std::exchange(shared->Ready, std::nullopt))
    {
        m_State = std::make_unique<State>(std::move(*ready));
        m_State->InitialProject = m_LocalRelay->Project;
        m_Notice = "Local session started. Its invite works on this computer.";
    }
}

bool SceneCollaborationSession::IsStarted() const
{
    return m_State != nullptr || m_LocalRelay != nullptr;
}

std::string SceneCollaborationSession::TakeNotice()
{
    return std::exchange(m_Notice, {});
}

SceneCollaborationSession::SceneCollaborationSession() = default;

SceneCollaborationSession::~SceneCollaborationSession() = default;

void SceneCollaborationSession::Update(const std::filesystem::path& project,
                                      const std::filesystem::path& scene, ECS::World* world)
{
    if (!m_Initialized)
    {
        m_Initialized = true;
        if (auto config = ReadConfiguration())
            m_State = std::make_unique<State>(std::move(*config));
    }
    if (m_LocalRelay && project != m_LocalRelay->Project)
        Stop();
    PollLocalRelay();
    if (!m_State || project.empty())
        return;
    auto& state = *m_State;
    if (state.InitialProject.empty() && !scene.empty())
        state.InitialProject = project;
    if (!state.InitialProject.empty() && project != state.InitialProject)
    {
        Stop();
        return;
    }

    const auto now = Clock::now();
    state.Receive(now);
    if (now - state.LastPresence > kLocalPresenceTimeout)
        state.Peers.clear();

    if (scene != state.ScenePath)
    {
        state.ScenePath = scene;
        state.SceneKey = scene.empty() ? std::string{} :
            PathToUtf8(RelativePathUnderRoot(scene.is_absolute() ? scene : project / scene, project));
    }
    const bool canSend = !state.InitialProject.empty() && now >= state.NextPoll && state.Task.IsDone();
    Json cursor = nullptr;
    for (auto* pane : SceneViewportRegistry::GetLive())
    {
        std::vector<CollaborationCursor> markers;
        if (world && !state.SceneKey.empty() && pane->GetWorld() == world)
        {
            const auto& view = pane->GetSlot();
            for (const auto& peer : state.Peers)
            {
                if (peer.Scene == state.SceneKey && peer.View == view)
                    markers.push_back(peer.Marker);
            }
            const float width = pane->GetLayoutWidth();
            const float height = pane->GetLayoutHeight();
            const auto local = pane->GetPointerLocalPosition();
            if (canSend && pane->IsPointerInside() && width > 0.0f && height > 0.0f)
            {
                cursor = {{"scene", state.SceneKey}, {"view", view},
                    {"x", std::clamp(local.x / width, 0.0f, 1.0f)},
                    {"y", std::clamp(local.y / height, 0.0f, 1.0f)}};
            }
        }
        pane->SetCollaborationCursors(markers);
    }

    if (!canSend)
        return;
    state.NextPoll = now + (state.Session.empty() ? kRetryInterval : kPresenceInterval);
    if (state.Session.empty())
        state.Send("join", {{"name", state.Config.Name}});
    else
        state.Send("sync", {{"session", state.Session}, {"sequence", ++state.Sequence},
            {"cursor", std::move(cursor)}, {"cursorOnly", true}});
}

bool SceneCollaborationSession::CanInvite() const
{
    return m_State && !m_State->Session.empty() &&
           Clock::now() - m_State->LastPresence <= kLocalPresenceTimeout;
}

std::string SceneCollaborationSession::InviteLink() const
{
    if (!CanInvite())
        return {};
    const auto& config = m_State->Config;
    return config.Relay + "/#room=" + HttpClient::UrlEncode(config.Room) +
           "&token=" + HttpClient::UrlEncode(config.Key);
}

void SceneCollaborationSession::Stop()
{
    if (m_State)
        for (auto* pane : SceneViewportRegistry::GetLive())
            pane->SetCollaborationCursors({});
    m_State.reset();
    m_LocalRelay.reset();
}
}
