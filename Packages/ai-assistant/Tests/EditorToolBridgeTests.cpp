#include "Providers/BackendDetection.h"
#include "Providers/CancelToken.h"
#include "Providers/EditorToolBridge.h"
#include "Providers/OpenAiCompatibleProvider.h"
#include "FakeApiKeyStore.h"
#include "RecordingTurnEvents.h"

#include "LoopbackServer.h"
#include "TestTempDir.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>

#include <nlohmann/json.hpp>

namespace GameEngine
{
namespace
{
// Stands in for mcp/dist/assistant-bridge.js: `list` prints two core tools and one other;
// `call <tool> --text-only` answers with the tool's name, the stdin arguments and the
// assistant token from the environment, so a test sees what reached the bridge.
constexpr const char* kFakeBridge = R"JS(
const [command, name, flag] = process.argv.slice(2);
if (command === "list") {
  console.log(JSON.stringify([
    { name: "get_editor_state", description: "Read the editor state", inputSchema: { type: "object", properties: {} } },
    { name: "get_log", description: "Read the log", inputSchema: { type: "object", properties: { count: { type: "number" } } } },
    { name: "get_render_stats", description: "Read the render stats", inputSchema: { type: "object", properties: {} } },
  ]));
  process.exit(0);
}
let input = "";
process.stdin.on("data", c => input += c);
process.stdin.on("end", () => {
  const text = `ran ${name} ${flag} args=${input} token=${process.env.GE_ASSISTANT_TOKEN}`;
  console.log(JSON.stringify({ content: [{ type: "text", text }], isError: name === "get_log" ? true : undefined }));
});
)JS";

std::optional<std::string> Node()
{
    const auto found = BackendDetection::FindProgram("node", BackendDetection::CurrentInputs());
    return found ? std::optional<std::string>(found->Program.string()) : std::nullopt;
}

// An attachment whose server script sits beside the fake bridge in `folder`.
AgentToolAttachment FakeAttachment(const std::filesystem::path& folder, const std::string& node)
{
    std::ofstream(folder / "assistant-bridge.js") << kFakeBridge;
    std::ofstream(folder / "index.js") << "";
    AgentToolAttachment attachment;
    attachment.ServerName = "editor_assistant";
    attachment.Node = node;
    attachment.ServerScript = (folder / "index.js").string();
    attachment.Environment = {{"GE_ASSISTANT_TOKEN", "fixture-token"}};
    return attachment;
}

std::string Chunk(std::string_view bytes)
{
    std::ostringstream size;
    size << std::hex << bytes.size();
    return size.str() + "\r\n" + std::string(bytes) + "\r\n";
}

std::string Stream(std::string_view events)
{
    return "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n" +
           Chunk(events) + "0\r\n\r\n";
}
} // namespace

// The core tools go in whole; every other tool is named in describe_editor_tool and reached
// through call_editor_tool.
TEST(EditorToolBridgeTests, CoreToolsGoInWholeAndTheRestThroughTheMetaTools)
{
    const std::optional<std::string> node = Node();
    if (!node)
        GTEST_SKIP() << "Node.js is not installed";
    const TestUtils::ScopedTempDir temp{TestUtils::MakeUniqueTempDirectory("EditorToolBridge")};
    EditorToolBridge bridge(FakeAttachment(temp.Path(), *node));
    CancelToken cancel;
    std::string error;
    ASSERT_TRUE(bridge.Load(cancel, error)) << error;

    const nlohmann::json definitions = bridge.Definitions();
    std::vector<std::string> names;
    for (const nlohmann::json& definition : definitions)
        names.push_back(definition["function"]["name"]);
    EXPECT_EQ(names, (std::vector<std::string>{"get_editor_state", "get_log", "describe_editor_tool", "call_editor_tool"}));
    EXPECT_NE(definitions[2]["function"]["description"].get<std::string>().find("get_render_stats"), std::string::npos);

    const EditorToolBridge::Answer core = bridge.Call("get_editor_state", "{}", cancel);
    EXPECT_FALSE(core.IsError);
    EXPECT_EQ(core.Text, "ran get_editor_state --text-only args={} token=fixture-token");

    const EditorToolBridge::Answer other =
        bridge.Call("call_editor_tool", R"({"tool":"get_render_stats","arguments":{"frames":3}})", cancel);
    EXPECT_EQ(other.Text, R"(ran get_render_stats --text-only args={"frames":3} token=fixture-token)");
    EXPECT_EQ(EditorToolBridge::RunsTool("call_editor_tool", R"({"tool":"get_render_stats"})"), "get_render_stats");

    EXPECT_NE(bridge.Call("describe_editor_tool", R"({"tool":"get_render_stats"})", cancel).Text.find("Read the render stats"),
              std::string::npos);
    EXPECT_TRUE(bridge.Call("get_log", R"({"count":5})", cancel).IsError) << "the bridge's isError reaches the answer";
    EXPECT_TRUE(bridge.Call("get_render_stats", "{}", cancel).IsError) << "a non-core tool only through call_editor_tool";
    EXPECT_TRUE(bridge.Call("call_editor_tool", R"({"tool":"no_such_tool"})", cancel).IsError);
}

// A whole tool turn: the model asks for get_editor_state, the bridge runs it, the answer goes back
// as a tool message, and the model's second reply ends the turn.
TEST(EditorToolBridgeTests, AToolTurnRunsTheCallAndSendsTheAnswerBack)
{
    const std::optional<std::string> node = Node();
    if (!node)
        GTEST_SKIP() << "Node.js is not installed";
    const TestUtils::ScopedTempDir temp{TestUtils::MakeUniqueTempDirectory("EditorToolTurn")};
    const AgentToolAttachment attachment = FakeAttachment(temp.Path(), *node);
    LoopbackServer model([](LoopbackServer& self, SocketHandle client) {
        if (self.LastRequest().find("\"role\":\"tool\"") == std::string::npos)
            LoopbackServer::Send(client, Stream(
                "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"c1\",\"type\":\"function\","
                "\"function\":{\"name\":\"get_editor_state\",\"arguments\":\"\"}}]}}]}\n\n"
                "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"function\":{\"arguments\":\"{}\"}}]},"
                "\"finish_reason\":\"tool_calls\"}]}\n\n"
                "data: [DONE]\n\n"));
        else
            LoopbackServer::Send(client, Stream(
                "data: {\"choices\":[{\"delta\":{\"content\":\"The scene is open.\"},\"finish_reason\":\"stop\"}]}\n\n"
                "data: [DONE]\n\n"));
    });
    FakeApiKeyStore keys;
    OpenAiCompatibleProvider provider(keys.Store("the test store", ApiKeyStore::kEndpointSlot), model.Url("/v1"), "m1");
    AgentTurnRequest request;
    request.UserText = "What is open?";
    request.Tools = &attachment;
    RecordingTurnEvents events;
    CancelToken cancel;

    provider.RunTurn(request, events, cancel);

    EXPECT_EQ(events.Result.Outcome, TurnOutcome::Succeeded) << events.Result.Error;
    EXPECT_EQ(events.Result.Text, "The scene is open.");
    EXPECT_EQ(events.Result.ModelRoundTrips, 2u);
    const std::vector<std::string> expected = {"tool:get_editor_state", "delta:The scene is open.",
                                               "finished:The scene is open."};
    EXPECT_EQ(events.Log, expected);
    const std::string second = model.LastRequest();
    EXPECT_NE(second.find("\"tool_call_id\":\"c1\""), std::string::npos) << second;
    EXPECT_NE(second.find("ran get_editor_state --text-only args={} token=fixture-token"), std::string::npos);
    EXPECT_NE(second.find("\"tools\":["), std::string::npos) << "every request offers the tools";
}
} // namespace GameEngine
