#include "Providers/CancelToken.h"
#include "Providers/OpenAiChatStream.h"
#include "Providers/LocalServerScan.h"
#include "Providers/OpenAiCompatibleProvider.h"
#include "FakeApiKeyStore.h"
#include "RecordingTurnEvents.h"

#include "LoopbackServer.h"
#include "Platform/HttpClient.h"

#include <gtest/gtest.h>

#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace GameEngine
{
namespace
{
// A recorded OpenAI-compatible stream: a role chunk, two text chunks, a finish chunk with usage, [DONE].
constexpr std::string_view kRecordedReply =
    "data: {\"id\":\"c1\",\"model\":\"llama3.1:8b\",\"choices\":[{\"index\":0,\"delta\":{\"role\":\"assistant\"}}]}\n\n"
    "data: {\"id\":\"c1\",\"model\":\"llama3.1:8b\",\"choices\":[{\"index\":0,\"delta\":{\"content\":\"Hello\"}}]}\n\n"
    "data: {\"id\":\"c1\",\"model\":\"llama3.1:8b\",\"choices\":[{\"index\":0,\"delta\":{\"content\":\", world\"}}]}\n\n"
    "data: {\"id\":\"c1\",\"model\":\"llama3.1:8b\",\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"stop\"}],"
    "\"usage\":{\"prompt_tokens\":21,\"completion_tokens\":4}}\n\n"
    "data: [DONE]\n\n";

HttpClient::HttpResponse Completed()
{
    HttpClient::HttpResponse response;
    response.statusCode = 200;
    response.success = true;
    return response;
}

std::string Chunk(std::string_view bytes)
{
    std::ostringstream size;
    size << std::hex << bytes.size();
    return size.str() + "\r\n" + std::string(bytes) + "\r\n";
}

std::string ChunkedHead()
{
    return "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nTransfer-Encoding: chunked\r\n"
           "Connection: close\r\n\r\n";
}

std::string JsonAnswer(std::string_view status, std::string_view body)
{
    return "HTTP/1.1 " + std::string(status) + "\r\nContent-Type: application/json\r\nContent-Length: " +
           std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + std::string(body);
}

OpenAiCompatibleProvider Provider(const FakeApiKeyStore& keys, std::string url, std::string model = "llama3.1:8b")
{
    return OpenAiCompatibleProvider(keys.Store("the test store", ApiKeyStore::kEndpointSlot), std::move(url),
                                    std::move(model));
}
} // namespace

TEST(OpenAiCompatibleProviderTests, RecordedReplyYieldsTheTextDeltasTheModelAndTheAccounting)
{
    RecordingTurnEvents events;
    OpenAiChatStream stream(events);
    stream.Feed(kRecordedReply);
    stream.Finish(Completed());

    const std::vector<std::string> expected = {"delta:Hello", "delta:, world", "finished:Hello, world"};
    EXPECT_EQ(events.Log, expected);
    EXPECT_EQ(events.Result.Outcome, TurnOutcome::Succeeded);
    EXPECT_EQ(events.Result.Model, "llama3.1:8b");
    EXPECT_EQ(events.Result.InputTokens, 21u);
    EXPECT_EQ(events.Result.OutputTokens, 4u);
    EXPECT_TRUE(events.Result.Note.empty());
}

TEST(OpenAiCompatibleProviderTests, AReplyCutAtTheTokenLimitSucceedsWithANote)
{
    RecordingTurnEvents events;
    OpenAiChatStream stream(events);
    stream.Feed("data: {\"choices\":[{\"delta\":{\"content\":\"Half\"},\"finish_reason\":\"length\"}]}\n\n");
    stream.Finish(Completed());

    EXPECT_EQ(events.Result.Outcome, TurnOutcome::Succeeded);
    EXPECT_EQ(events.Result.Text, "Half");
    EXPECT_NE(events.Result.Note.find("token limit"), std::string::npos) << events.Result.Note;
}

TEST(OpenAiCompatibleProviderTests, AStreamThatStopsMidReplyFailsAndKeepsItsText)
{
    RecordingTurnEvents events;
    OpenAiChatStream stream(events);
    stream.Feed("data: {\"choices\":[{\"delta\":{\"content\":\"Par\"}}]}\n\n");
    stream.Finish(Completed());

    EXPECT_EQ(events.Result.Outcome, TurnOutcome::Failed);
    EXPECT_EQ(events.Result.Text, "Par");
    EXPECT_NE(events.Result.Error.find("ended before the reply finished"), std::string::npos);
}

TEST(OpenAiCompatibleProviderTests, AnErrorChunkFailsTheTurnWithItsMessage)
{
    RecordingTurnEvents events;
    OpenAiChatStream stream(events);
    stream.Feed("data: {\"error\":{\"message\":\"model 'x' not found\"}}\n\n");
    stream.Finish(Completed());

    EXPECT_EQ(events.Result.Outcome, TurnOutcome::Failed);
    EXPECT_NE(events.Result.Error.find("model 'x' not found"), std::string::npos) << events.Result.Error;
}

TEST(OpenAiCompatibleProviderTests, TheBaseUrlLosesItsTrailingSlashAndAPastedChatPath)
{
    EXPECT_EQ(OpenAiCompatibleProvider::NormalizeBaseUrl(" http://localhost:11434/v1/ "), "http://localhost:11434/v1");
    EXPECT_EQ(OpenAiCompatibleProvider::NormalizeBaseUrl("http://localhost:1234/v1/chat/completions"),
              "http://localhost:1234/v1");
}

TEST(OpenAiCompatibleProviderTests, TheBodyCarriesTheSystemPromptTheHistoryAndTheModel)
{
    AgentTurnRequest request;
    request.SystemPrompt = "Be brief.";
    request.UserText = "Second";
    ConversationMessage first;
    first.Role = MessageRole::User;
    first.Text = "First";
    ConversationMessage reply;
    reply.Role = MessageRole::Assistant;
    reply.Text = "Reply";
    const std::vector<ConversationMessage> history = {first, reply};
    request.History = history;

    const nlohmann::json body = nlohmann::json::parse(OpenAiCompatibleProvider::BuildRequestBody(request, "m1"));

    EXPECT_EQ(body["model"], "m1");
    EXPECT_EQ(body["stream"], true);
    ASSERT_EQ(body["messages"].size(), 4u);
    EXPECT_EQ(body["messages"][0]["role"], "system");
    EXPECT_EQ(body["messages"][1]["role"], "user");
    EXPECT_EQ(body["messages"][2]["role"], "assistant");
    EXPECT_EQ(body["messages"][3]["content"], "Second");
}

TEST(OpenAiCompatibleProviderTests, TheKeyTravelsOnlyAsABearerTokenAndOnlyWhenSaved)
{
    EXPECT_EQ(OpenAiCompatibleProvider::BuildRequestHeaders("").count("Authorization"), 0u);
    const auto headers = OpenAiCompatibleProvider::BuildRequestHeaders("fixture-key");
    EXPECT_EQ(headers.at("Authorization"), "Bearer fixture-key");
}

TEST(OpenAiCompatibleProviderTests, AMissingUrlOrModelFailsTheCheckWithTheFix)
{
    FakeApiKeyStore keys;
    const SetupStatus noUrl = Provider(keys, "").CheckSetup({}, SetupSurface::SettingsPage, {});
    ASSERT_FALSE(noUrl.Steps.empty());
    EXPECT_EQ(noUrl.Steps[0].State, SetupStepState::Failed);
    EXPECT_NE(noUrl.Steps[0].Fix.find("http://localhost:11434/v1"), std::string::npos) << noUrl.Steps[0].Fix;
    EXPECT_FALSE(noUrl.Ready());

    const SetupStatus noModel = Provider(keys, "http://127.0.0.1:1/v1", "").CheckSetup({}, SetupSurface::Panel, {});
    ASSERT_GE(noModel.Steps.size(), 2u);
    EXPECT_EQ(noModel.Steps[1].State, SetupStepState::Failed);
    EXPECT_TRUE(noModel.Steps[1].Fix.starts_with("Open Settings > AI Assistant and type the model"))
        << noModel.Steps[1].Fix;
}

// The check asks <base>/models: a listed model passes, an unlisted one warns, no server fails.
TEST(OpenAiCompatibleProviderTests, TheEndpointAnswersStepReadsTheModelList)
{
    FakeApiKeyStore keys;
    const auto answer = [](std::string bytes) {
        return [bytes](LoopbackServer&, SocketHandle client) { LoopbackServer::Send(client, bytes); };
    };
    LoopbackServer listed(answer(JsonAnswer("200 OK", R"({"object":"list","data":[{"id":"llama3.1:8b"}]})")));
    LoopbackServer unlisted(answer(JsonAnswer("200 OK", R"({"object":"list","data":[{"id":"other"}]})")));

    const SetupStatus passed = Provider(keys, listed.Url("/v1")).CheckSetup({}, SetupSurface::SettingsPage, {});
    const SetupStatus warned = Provider(keys, unlisted.Url("/v1")).CheckSetup({}, SetupSurface::SettingsPage, {});
    const SetupStatus unreachable =
        Provider(keys, "http://127.0.0.1:1/v1").CheckSetup({}, SetupSurface::SettingsPage, {});

    ASSERT_EQ(passed.Steps.size(), 3u);
    EXPECT_EQ(passed.Steps[2].State, SetupStepState::Passed);
    EXPECT_NE(listed.LastRequest().find("GET /v1/models"), std::string::npos) << listed.LastRequest();
    ASSERT_EQ(warned.Steps.size(), 3u);
    EXPECT_EQ(warned.Steps[2].State, SetupStepState::Warning);
    EXPECT_TRUE(warned.Ready());
    ASSERT_EQ(unreachable.Steps.size(), 3u);
    EXPECT_EQ(unreachable.Steps[2].State, SetupStepState::Failed);
    EXPECT_FALSE(unreachable.Ready());
}

// A whole turn against a loopback server: the request goes to <base>/chat/completions with the
// saved key, and the reply streams back.
TEST(OpenAiCompatibleProviderTests, ATurnStreamsFromTheChatCompletionsPathWithTheSavedKey)
{
    FakeApiKeyStore keys;
    keys.Data->Saved = "fixture-endpoint-key";
    LoopbackServer server([](LoopbackServer&, SocketHandle client) {
        LoopbackServer::Send(client, ChunkedHead() + Chunk(kRecordedReply) + "0\r\n\r\n");
    });
    OpenAiCompatibleProvider provider = Provider(keys, server.Url("/v1/"));
    AgentTurnRequest request;
    request.UserText = "Hi";
    RecordingTurnEvents events;
    CancelToken cancel;

    provider.RunTurn(request, events, cancel);

    EXPECT_EQ(events.Result.Outcome, TurnOutcome::Succeeded) << events.Result.Error;
    EXPECT_EQ(events.Result.Text, "Hello, world");
    const std::string sent = server.LastRequest();
    EXPECT_NE(sent.find("POST /v1/chat/completions"), std::string::npos) << sent;
    EXPECT_NE(sent.find("Bearer fixture-endpoint-key"), std::string::npos);
    EXPECT_EQ(keys.Data->LastName, ApiKeyStore::kEndpointSlot.CredentialName);
}

// A model list's owner names the server: TensorFold Bar serves "local-model", owned by "tensorfold".
TEST(OpenAiCompatibleProviderTests, AModelListNamesItsServer)
{
    const LocalServerScan::Server tensorFold = LocalServerScan::ParseModelList(
        "http://127.0.0.1:8080/v1",
        R"({"object":"list","data":[{"id":"local-model","object":"model","owned_by":"tensorfold"}]})");
    EXPECT_EQ(tensorFold.Name, "TensorFold");
    EXPECT_EQ(tensorFold.Models, (std::vector<std::string>{"local-model"}));

    const LocalServerScan::Server lmStudio = LocalServerScan::ParseModelList(
        "http://127.0.0.1:1234/v1", R"({"data":[{"id":"qwen3","owned_by":"organization_owner"}]})");
    EXPECT_EQ(lmStudio.Name, "LM Studio") << "an unknown owner falls back to the port's program";

    EXPECT_EQ(LocalServerScan::ParseModelList("http://127.0.0.1:9999/v1", R"({"data":[{"id":"m"}]})").Name,
              "Server at 127.0.0.1:9999");
    EXPECT_TRUE(LocalServerScan::ParseModelList("http://127.0.0.1:8080/v1", "not json").Models.empty());
}

// The scan keeps the servers that list a model and skips a port nothing answers on.
TEST(OpenAiCompatibleProviderTests, TheScanFindsTheServersThatAnswer)
{
    LoopbackServer server([](LoopbackServer&, SocketHandle client) {
        LoopbackServer::Send(client, JsonAnswer("200 OK", R"({"data":[{"id":"local-model","owned_by":"tensorfold"}]})"));
    });
    const std::string answering = server.Url("/v1");
    const std::string_view candidates[] = {"http://127.0.0.1:1/v1", answering};
    CancelToken cancel;

    const std::vector<LocalServerScan::Server> found = LocalServerScan::Scan(candidates, cancel);

    ASSERT_EQ(found.size(), 1u);
    EXPECT_EQ(found[0].Name, "TensorFold");
    EXPECT_EQ(found[0].BaseUrl, answering);
}
// A server started without TensorFold Bar lists its model by name: a FirstListed provider asks
// for that model, and its check has no Model step.
TEST(OpenAiCompatibleProviderTests, AFirstListedProviderAsksForTheServersFirstModel)
{
    FakeApiKeyStore keys;
    LoopbackServer server([](LoopbackServer& self, SocketHandle client) {
        // The model list for GET /v1/models, the chat stream for the chat request.
        if (self.LastRequest().starts_with("GET "))
            LoopbackServer::Send(client, JsonAnswer("200 OK", R"({"data":[{"id":"Qwen3.8-Flash","owned_by":"tensorfold"}]})"));
        else
            LoopbackServer::Send(client, ChunkedHead() + Chunk(kRecordedReply) + "0\r\n\r\n");
    });
    OpenAiCompatibleProvider provider(keys.Store("the test store", ApiKeyStore::kEndpointSlot), server.Url("/v1"), "",
                                      "tensorfold-local", OpenAiCompatibleProvider::ModelSource::FirstListed);

    const SetupStatus status = provider.CheckSetup({}, SetupSurface::SettingsPage, {});
    ASSERT_EQ(status.Steps.size(), 2u) << "Endpoint and Endpoint answers, no Model step";
    EXPECT_EQ(status.Steps[1].State, SetupStepState::Passed);
    EXPECT_NE(status.Steps[1].Found.find("Qwen3.8-Flash"), std::string::npos) << status.Steps[1].Found;

    AgentTurnRequest request;
    request.UserText = "Hi";
    RecordingTurnEvents events;
    CancelToken cancel;
    provider.RunTurn(request, events, cancel);

    EXPECT_EQ(events.Result.Outcome, TurnOutcome::Succeeded) << events.Result.Error;
    EXPECT_NE(server.LastRequest().find("\"model\":\"Qwen3.8-Flash\""), std::string::npos) << server.LastRequest();
    EXPECT_EQ(provider.Id(), "tensorfold-local");
}

// ReadServedModel names the served model and the efforts its entry lists, and a turn that names an
// effort sends it as reasoning_effort; a turn without one sends none.
TEST(OpenAiCompatibleProviderTests, TheServedModelsEffortsAreReadAndTheChosenOneIsSent)
{
    FakeApiKeyStore keys;
    LoopbackServer server([](LoopbackServer& self, SocketHandle client) {
        if (self.LastRequest().starts_with("GET "))
            LoopbackServer::Send(client, JsonAnswer("200 OK", R"({"data":[{"id":"inkling-2-mini",)"
                                                              R"("reasoning":{"supported_efforts":["high","low"]}}]})"));
        else
            LoopbackServer::Send(client, ChunkedHead() + Chunk(kRecordedReply) + "0\r\n\r\n");
    });
    OpenAiCompatibleProvider provider(keys.Store("the test store", ApiKeyStore::kEndpointSlot), server.Url("/v1"), "",
                                      "tensorfold-local", OpenAiCompatibleProvider::ModelSource::FirstListed);
    CancelToken cancel;

    const OpenAiCompatibleProvider::ServedModel served = provider.ReadServedModel(cancel);
    EXPECT_EQ(served.Name, "inkling-2-mini");
    EXPECT_EQ(served.Efforts, (std::vector<std::string>{"high", "low"}));

    AgentTurnRequest request;
    request.UserText = "Hi";
    request.Effort = "high";
    RecordingTurnEvents events;
    provider.RunTurn(request, events, cancel);
    EXPECT_EQ(events.Result.Outcome, TurnOutcome::Succeeded) << events.Result.Error;
    EXPECT_NE(server.LastRequest().find("\"reasoning_effort\":\"high\""), std::string::npos) << server.LastRequest();

    request.Effort.clear();
    RecordingTurnEvents plain;
    provider.RunTurn(request, plain, cancel);
    EXPECT_EQ(server.LastRequest().find("reasoning_effort"), std::string::npos) << "Default sends no effort";
}

// With no server running, the turn says to start it.
TEST(OpenAiCompatibleProviderTests, AFirstListedProviderWithNoServerRefusesTheTurn)
{
    FakeApiKeyStore keys;
    OpenAiCompatibleProvider provider(keys.Store(), "http://127.0.0.1:1/v1", "", "tensorfold-local",
                                      OpenAiCompatibleProvider::ModelSource::FirstListed);
    AgentTurnRequest request;
    request.UserText = "Hi";
    RecordingTurnEvents events;
    CancelToken cancel;
    provider.RunTurn(request, events, cancel);

    EXPECT_EQ(events.Result.Outcome, TurnOutcome::Failed);
    EXPECT_NE(events.Result.Error.find("Start the server"), std::string::npos) << events.Result.Error;
}

} // namespace GameEngine
