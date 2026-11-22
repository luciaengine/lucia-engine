#include "AgentSessionState.h"

#include "AiAssistantSettings.h"
#include "Providers/ClaudeApiProvider.h"
#include "Providers/ClaudeSessionProvider.h"
#include "Providers/CodexSessionProvider.h"
#include "Providers/HarnessSessionProvider.h"
#include "Providers/OpenAiCompatibleProvider.h"
#include "Providers/TensorFoldConnection.h"

#include "Editor/EditorPaths.h"

#include <algorithm>
#include <utility>

namespace GameEngine
{
namespace
{
// A key store that holds no key, for a connection that sends none (TensorFold).
ApiKeyStore NoKeys()
{
    ApiKeyStore::Backend backend;
    backend.Read = [](std::string_view) { return Platform::SecretReadResult{.Status = Platform::SecretStoreStatus::NotFound}; };
    backend.Store = [](std::string_view, std::string_view) { return Platform::SecretStoreStatus::NoStore; };
    backend.Delete = [](std::string_view) { return Platform::SecretStoreStatus::NoStore; };
    return ApiKeyStore(std::move(backend), ApiKeyStore::kEndpointSlot);
}
} // namespace

AgentSessionState& AgentSessionState::Get()
{
    static AgentSessionState state;
    return state;
}

AgentSessionState::AgentSessionState() : m_ModelsUrl(ClaudeApiProvider::kModelsEndpoint)
{
}

std::shared_ptr<IAgentProvider> AgentSessionState::Provider(std::string_view providerId)
{
    // What the provider is made from; a change makes a new provider. The custom endpoint's is its
    // URL and model, which its setup check reads.
    std::string executable;
    std::string endpointUrl;
    std::string endpointModel;
    const int toolRequestLimit = AiAssistantSettings::ToolRequestLimit();
    if (providerId == ClaudeSessionProvider::kId)
        executable = AiAssistantSettings::ClaudeExecutable();
    else if (providerId == CodexSessionProvider::kId)
        executable = AiAssistantSettings::CodexExecutable();
    else if (providerId == HarnessSessionProvider::kId)
        executable = AiAssistantSettings::HarnessExecutable();
    else if (providerId == OpenAiCompatibleProvider::kId)
    {
        endpointUrl = AiAssistantSettings::EndpointUrl();
        endpointModel = AiAssistantSettings::Model(providerId);
        executable = endpointUrl + "\n" + endpointModel + "\n" + std::to_string(toolRequestLimit);
    }
    else if (providerId == TensorFoldConnection::kId)
    {
        endpointUrl = AiAssistantSettings::TensorFoldUrl();
        executable = endpointUrl + "\n" + std::to_string(toolRequestLimit);
    }
    else if (providerId != ClaudeApiProvider::kId)
        return nullptr;
    const std::filesystem::path directory = Editor::GetCurrentEditorProjectPaths().projectRoot;

    std::lock_guard lock(m_Mutex);
    Connection& connection = FindOrAdd(providerId);
    if (connection.Provider && connection.Executable == executable && connection.Directory == directory)
        return connection.Provider;

    // A session belongs to one executable's store and one project directory.
    connection.Executable = executable;
    connection.Directory = directory;
    connection.SessionId.clear();
    if (providerId == ClaudeSessionProvider::kId)
        connection.Provider = std::make_shared<ClaudeSessionProvider>(executable, directory);
    else if (providerId == CodexSessionProvider::kId)
        connection.Provider = std::make_shared<CodexSessionProvider>(executable, directory);
    else if (providerId == HarnessSessionProvider::kId)
        connection.Provider = std::make_shared<HarnessSessionProvider>(executable, directory);
    else if (providerId == OpenAiCompatibleProvider::kId)
        connection.Provider = std::make_shared<OpenAiCompatibleProvider>(
            m_EndpointKeys, endpointUrl, endpointModel, OpenAiCompatibleProvider::kId,
            OpenAiCompatibleProvider::ModelSource::Given, toolRequestLimit);
    else if (providerId == TensorFoldConnection::kId)
        connection.Provider = std::make_shared<OpenAiCompatibleProvider>(
            NoKeys(), endpointUrl, std::string(), TensorFoldConnection::kId,
            OpenAiCompatibleProvider::ModelSource::FirstListed, toolRequestLimit);
    else
        connection.Provider = std::make_shared<ClaudeApiProvider>(m_ApiKeys, m_ModelsUrl);
    return connection.Provider;
}

ApiKeyStore AgentSessionState::ApiKeys() const
{
    std::lock_guard lock(m_Mutex);
    return m_ApiKeys;
}

void AgentSessionState::UseApiConnectionForTesting(ApiKeyStore keys, std::string modelsUrl)
{
    std::lock_guard lock(m_Mutex);
    m_ApiKeys = std::move(keys);
    m_ModelsUrl = std::move(modelsUrl);
    Connection& connection = FindOrAdd(ClaudeApiProvider::kId);
    connection.Provider.reset();
}

ApiKeyStore AgentSessionState::EndpointKeys() const
{
    std::lock_guard lock(m_Mutex);
    return m_EndpointKeys;
}

void AgentSessionState::UseEndpointKeysForTesting(ApiKeyStore keys)
{
    std::lock_guard lock(m_Mutex);
    m_EndpointKeys = std::move(keys);
    FindOrAdd(OpenAiCompatibleProvider::kId).Provider.reset();
}

std::shared_ptr<CliSessionProvider> AgentSessionState::SessionProvider(std::string_view providerId)
{
    if (providerId != ClaudeSessionProvider::kId && providerId != CodexSessionProvider::kId &&
        providerId != HarnessSessionProvider::kId)
        return nullptr;
    return std::static_pointer_cast<CliSessionProvider>(Provider(providerId));
}

std::string AgentSessionState::SessionId(std::string_view providerId) const
{
    std::lock_guard lock(m_Mutex);
    const auto it = std::find_if(m_Connections.begin(), m_Connections.end(),
                                 [providerId](const Connection& c) { return c.ProviderId == providerId; });
    return it == m_Connections.end() ? std::string() : it->SessionId;
}

void AgentSessionState::SetSessionId(const IAgentProvider& provider, std::string sessionId)
{
    std::filesystem::path directory;
    {
        std::lock_guard lock(m_Mutex);
        const auto it = std::find_if(m_Connections.begin(), m_Connections.end(),
                                     [&provider](const Connection& c) { return c.Provider.get() == &provider; });
        if (it == m_Connections.end())
            return;
        it->SessionId = sessionId;
        directory = it->Directory;
    }
    // Outside the lock: the store reads and writes a file.
    AiAssistantSettings::SetLastSession(provider.Id(), directory, sessionId);
}

void AgentSessionState::ResumeSession(std::string_view providerId, std::string sessionId)
{
    if (const std::shared_ptr<IAgentProvider> provider = Provider(providerId))
        SetSessionId(*provider, std::move(sessionId));
}

void AgentSessionState::ForgetSessions()
{
    std::lock_guard lock(m_Mutex);
    for (Connection& connection : m_Connections)
        connection.SessionId.clear();
}

std::optional<AssistantMode> AgentSessionState::SessionMode(std::string_view providerId,
                                                            std::string_view sessionId) const
{
    std::filesystem::path directory = Editor::GetCurrentEditorProjectPaths().projectRoot;
    {
        std::lock_guard lock(m_Mutex);
        const auto it = std::find_if(m_Connections.begin(), m_Connections.end(),
                                     [providerId](const Connection& c) { return c.ProviderId == providerId; });
        if (it != m_Connections.end() && it->Provider)
            directory = it->Directory;
    }
    return AiAssistantSettings::SessionMode(providerId, directory, sessionId);
}

void AgentSessionState::SetSessionMode(std::string_view providerId, AssistantMode mode)
{
    std::filesystem::path directory;
    std::string sessionId;
    {
        std::lock_guard lock(m_Mutex);
        const auto it = std::find_if(m_Connections.begin(), m_Connections.end(),
                                     [providerId](const Connection& c) { return c.ProviderId == providerId; });
        if (it == m_Connections.end() || it->SessionId.empty())
            return;
        directory = it->Directory;
        sessionId = it->SessionId;
    }
    // Outside the lock: the store reads and writes a file.
    AiAssistantSettings::SetSessionMode(providerId, directory, sessionId, mode);
}

AgentSessionState::Connection& AgentSessionState::FindOrAdd(std::string_view providerId)
{
    const auto it = std::find_if(m_Connections.begin(), m_Connections.end(),
                                 [providerId](const Connection& c) { return c.ProviderId == providerId; });
    if (it != m_Connections.end())
        return *it;
    Connection& added = m_Connections.emplace_back();
    added.ProviderId = std::string(providerId);
    return added;
}
} // namespace GameEngine
