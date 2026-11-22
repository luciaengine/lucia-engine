#pragma once

#include "ApiKeyStore.h"
#include "IAgentProvider.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

#include <nlohmann/json_fwd.hpp>

namespace GameEngine
{
/// A server the user names with an OpenAI-compatible chat API (a local model server, a hosted
/// one), streamed. Stateless: every turn resends the system prompt, the history and the new
/// message. A turn with the editor's tools (AgentTurnRequest::Tools) runs the tool loop itself:
/// it sends EditorToolBridge's definitions, runs each tool call the model makes through the
/// bridge and sends the answers back, until the model answers without a call or kMaxToolRounds. The base URL and the model are settings; the key is optional and comes from the
/// ApiKeyStore's endpoint slot. HttpClient refuses to send a key over plain http to a host that
/// is not loopback, so a key travels only over https or to this computer.
class OpenAiCompatibleProvider final : public IAgentProvider
{
public:
    /// The provider's id (Id()).
    static constexpr std::string_view kId = "custom-endpoint";
    /// The setup steps' names.
    static constexpr std::string_view kEndpointStep = "Endpoint";
    static constexpr std::string_view kModelStep = "Model";
    static constexpr std::string_view kAnswersStep = "Endpoint answers";
    /// The most model requests one turn makes while the model keeps calling tools.
    static constexpr int kMaxToolRounds = 100;

    /// Which model a turn asks for when the request names none.
    enum class ModelSource : uint8_t
    {
        /// The model given to the constructor; the Model step fails when it is empty.
        Given,
        /// The first model the server lists, read when a turn starts (TensorFold).
        FirstListed,
    };

    /// `baseUrl` is the server's API root (".../v1"); `model` the model a turn asks for when the
    /// request names none (ModelSource::Given); `id` the connection it serves (kId, or
    /// TensorFoldConnection::kId).
    OpenAiCompatibleProvider(ApiKeyStore keys, std::string baseUrl, std::string model, std::string_view id = kId,
                             ModelSource modelSource = ModelSource::Given);

    std::string_view Id() const override { return m_Id; }
    ProviderCapabilities Capabilities() const override;

    /// Three steps. Endpoint: the base URL is an http or https URL. Model: a model is named.
    /// Endpoint answers: one GET of <base>/models with the key when one is saved; Failed when the
    /// server cannot be reached or refuses the key, Warning when it answers another error or does
    /// not list the model.
    SetupStatus CheckSetup(const CancelToken& cancel, SetupSurface surface, const SetupStepSink& onStep) const override;
    std::vector<SetupStep> SetupPlan() const override;

    /// Refuses with the Endpoint or Model step's finding and fix when either fails; otherwise
    /// streams the turn from <base>/chat/completions (OpenAiChatStream), with the tool loop when
    /// the request attaches the editor's tools.
    void RunTurn(const AgentTurnRequest& request, AgentTurnEvents& events, const CancelToken& cancel) override;

    /// `baseUrl` without its trailing slashes and without a trailing "/chat/completions" a user
    /// pasted, so the paths below can be appended.
    static std::string NormalizeBaseUrl(std::string_view baseUrl);
    /// The request body: model, stream true, and messages = the system prompt when there is one,
    /// the history (messages with empty text skipped) and the user's message, with its images as
    /// data URLs.
    static std::string BuildRequestBody(const AgentTurnRequest& request, std::string_view model);
    /// The `messages` array BuildRequestBody() sends.
    static nlohmann::json BuildMessages(const AgentTurnRequest& request);
    /// accept (an event stream), and Authorization = "Bearer <key>" when `key` is not empty.
    static std::unordered_map<std::string, std::string> BuildRequestHeaders(std::string_view key);

private:
    /// The tool loop of a turn that attaches the editor's tools.
    void RunToolTurn(const AgentTurnRequest& request, const std::string& model, AgentTurnEvents& events,
                     const CancelToken& cancel);
    /// The model a turn of `request` asks for; empty with the reason in `refusal` when there is none.
    std::string TurnModel(const AgentTurnRequest& request, std::string& refusal, const CancelToken& cancel) const;

    std::string_view m_Id;
    ModelSource m_ModelSource = ModelSource::Given;
    ApiKeyStore m_Keys;
    std::string m_BaseUrl;
    std::string m_Model;
};
} // namespace GameEngine
