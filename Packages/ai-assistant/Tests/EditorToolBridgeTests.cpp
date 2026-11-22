#include "Providers/BackendDetection.h"
#include "Providers/CancelToken.h"
#include "Providers/EditorToolBridge.h"
#include "Providers/OpenAiCompatibleProvider.h"
#include "FakeApiKeyStore.h"
#include "RecordingTurnEvents.h"

#include "LoopbackServer.h"
#include "Platform/Process.h"
#include "TestTempDir.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace GameEngine
{
namespace
{
// Stands in for mcp/dist/assistant-bridge.js: `list` prints three core tools and one other;
// `call <tool> <flag>` answers with the tool's name, its first flag, the stdin arguments and the
// assistant token from the environment, so a test sees what reached the bridge. With
// --image-dir, take_screenshot writes a 1x1 PNG into the folder and names it in an image_file
// block, and also names a file outside the folder, which the editor must leave alone.
constexpr const char* kFakeBridge = R"JS(
const fs = require("fs"), path = require("path");
const [command, name, flag, folder] = process.argv.slice(2);
const png = "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAIAAACQd1PeAAAADElEQVR4nGP4z8AAAAMBAQDJ/pLvAAAAAElFTkSuQmCC";
if (command === "list") {
  console.log(JSON.stringify([
    { name: "get_editor_state", description: "Read the editor state", inputSchema: { type: "object", properties: {} } },
    { name: "take_screenshot", description: "Capture the editor", inputSchema: { type: "object", properties: {} } },
    { name: "get_log", description: "Read the log", inputSchema: { type: "object", properties: { count: { type: "number" } } } },
    { name: "get_render_stats", description: "Read the render stats", inputSchema: { type: "object", properties: {} } },
  ]));
  process.exit(0);
}
let input = "";
process.stdin.on("data", c => input += c);
process.stdin.on("end", () => {
  const text = `ran ${name} ${flag} args=${input} token=${process.env.GE_ASSISTANT_TOKEN}`;
  const content = [{ type: "text", text }];
  if (name === "take_screenshot" && flag === "--image-dir") {
    fs.mkdirSync(folder, { recursive: true });
    const inside = path.join(folder, "shot.png"), outside = path.join(path.dirname(folder), "outside.png");
    fs.writeFileSync(inside, Buffer.from(png, "base64"));
    fs.writeFileSync(outside, Buffer.from(png, "base64"));
    content.unshift({ type: "image_file", path: inside, mimeType: "image/png" },
                    { type: "image_file", path: outside, mimeType: "image/png" });
  }
  console.log(JSON.stringify({ content, isError: name === "get_log" ? true : undefined }));
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

std::string JsonAnswer(std::string_view body)
{
    return "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) +
           "\r\nConnection: close\r\n\r\n" + std::string(body);
}

constexpr const char* kCallScreenshot =
    "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"c1\",\"type\":\"function\","
    "\"function\":{\"name\":\"take_screenshot\",\"arguments\":\"{}\"}}]},\"finish_reason\":\"tool_calls\"}]}\n\n"
    "data: [DONE]\n\n";
constexpr const char* kAnswerDone =
    "data: {\"choices\":[{\"delta\":{\"content\":\"Done.\"},\"finish_reason\":\"stop\"}]}\n\ndata: [DONE]\n\n";

// A model server whose model list gives `entry` for m1, and which asks for take_screenshot once.
// `chats` collects every chat request.
LoopbackServer ScreenshotModel(std::string entry, std::vector<std::string>& chats)
{
    return LoopbackServer([entry = std::move(entry), &chats](LoopbackServer& self, SocketHandle client) {
        const std::string request = self.LastRequest();
        if (request.starts_with("GET "))
        {
            LoopbackServer::Send(client, JsonAnswer(R"({"data":[)" + entry + "]}"));
            return;
        }
        chats.push_back(request);
        LoopbackServer::Send(client, Stream(chats.size() == 1 ? kCallScreenshot : kAnswerDone));
    });
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
    EditorToolBridge bridge(FakeAttachment(temp.Path(), *node), EditorToolBridge::ToolImages::Described);
    CancelToken cancel;
    std::string error;
    ASSERT_TRUE(bridge.Load(cancel, error)) << error;

    const nlohmann::json definitions = bridge.Definitions();
    std::vector<std::string> names;
    for (const nlohmann::json& definition : definitions)
        names.push_back(definition["function"]["name"]);
    EXPECT_EQ(names, (std::vector<std::string>{"get_editor_state", "take_screenshot", "get_log", "describe_editor_tool",
                                               "call_editor_tool"}));
    EXPECT_NE(definitions[3]["function"]["description"].get<std::string>().find("get_render_stats"), std::string::npos);

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

// ToolImages::Returned reads the image file the bridge writes in its folder and deletes it; a file
// outside the folder is neither read nor deleted. Described asks the bridge for text only.
TEST(EditorToolBridgeTests, ReturnedImagesAreReadFromTheBridgesFolderAndDeleted)
{
    const std::optional<std::string> node = Node();
    if (!node)
        GTEST_SKIP() << "Node.js is not installed";
    const TestUtils::ScopedTempDir temp{TestUtils::MakeUniqueTempDirectory("EditorToolImages")};
    const AgentToolAttachment attachment = FakeAttachment(temp.Path(), *node);
    CancelToken cancel;
    std::string error;

    EditorToolBridge returned(attachment, EditorToolBridge::ToolImages::Returned);
    ASSERT_TRUE(returned.Load(cancel, error)) << error;
    const EditorToolBridge::Answer shot = returned.Call("take_screenshot", "{}", cancel);
    ASSERT_FALSE(shot.IsError) << shot.Text;
    ASSERT_EQ(shot.Images.size(), 1u) << shot.Text;
    EXPECT_EQ(shot.Images[0].MediaType, "image/png");
    EXPECT_TRUE(shot.Images[0].Bytes.starts_with("\x89PNG"));
    const std::filesystem::path folder = std::filesystem::path(shot.Images[0].Path).parent_path();
    EXPECT_FALSE(std::filesystem::exists(shot.Images[0].Path)) << "the image file is deleted once read";
    const std::filesystem::path outside = folder.parent_path() / "outside.png";
    EXPECT_TRUE(std::filesystem::exists(outside)) << "a file outside the bridge's folder is left alone";
    std::filesystem::remove(outside);

    EditorToolBridge described(attachment, EditorToolBridge::ToolImages::Described);
    ASSERT_TRUE(described.Load(cancel, error)) << error;
    const EditorToolBridge::Answer line = described.Call("take_screenshot", "{}", cancel);
    EXPECT_TRUE(line.Images.empty());
    EXPECT_EQ(line.Text, "ran take_screenshot --text-only args={} token=fixture-token");
}

// A model whose list entry names image input gets the screenshot back as an image_url part in a
// user message after the tool answer, and is told how to ask for it.
TEST(EditorToolBridgeTests, AModelThatReadsImagesGetsTheToolsImage)
{
    const std::optional<std::string> node = Node();
    if (!node)
        GTEST_SKIP() << "Node.js is not installed";
    const TestUtils::ScopedTempDir temp{TestUtils::MakeUniqueTempDirectory("EditorToolVision")};
    const AgentToolAttachment attachment = FakeAttachment(temp.Path(), *node);
    std::vector<std::string> chats;
    LoopbackServer model = ScreenshotModel(R"({"id":"m1","input_types":["text","image"]})", chats);
    FakeApiKeyStore keys;
    OpenAiCompatibleProvider provider(keys.Store("the test store", ApiKeyStore::kEndpointSlot), model.Url("/v1"), "m1");
    AgentTurnRequest request;
    request.UserText = "Look at the scene.";
    request.Tools = &attachment;
    RecordingTurnEvents events;
    CancelToken cancel;

    provider.RunTurn(request, events, cancel);

    ASSERT_EQ(events.Result.Outcome, TurnOutcome::Succeeded) << events.Result.Error;
    ASSERT_EQ(chats.size(), 2u);
    EXPECT_NE(chats[0].find("includeImage set to true"), std::string::npos) << chats[0];
    const std::string& second = chats[1];
    EXPECT_NE(second.find("ran take_screenshot --image-dir"), std::string::npos) << second;
    EXPECT_NE(second.find("The images the tool calls returned, in order: take_screenshot (c1)."), std::string::npos);
    EXPECT_NE(second.find("data:image/png;base64,iVBORw0KGgo"), std::string::npos) << "the screenshot is sent";
    EXPECT_LT(second.find("\"role\":\"tool\""), second.find("data:image/png")) << "the image follows the tool answer";
    std::filesystem::remove(std::filesystem::temp_directory_path() / "gameengine_mcp" /
                            ("pid-" + std::to_string(Platform::GetCurrentProcessId())) / "outside.png");
}

// A model whose list says it reads text only gets a line for the screenshot, no image, and a turn
// whose prompt carries images is refused before any chat request.
TEST(EditorToolBridgeTests, AModelThatReadsTextOnlyGetsALineAndRefusesPromptImages)
{
    const std::optional<std::string> node = Node();
    if (!node)
        GTEST_SKIP() << "Node.js is not installed";
    const TestUtils::ScopedTempDir temp{TestUtils::MakeUniqueTempDirectory("EditorToolText")};
    const AgentToolAttachment attachment = FakeAttachment(temp.Path(), *node);
    std::vector<std::string> chats;
    LoopbackServer model = ScreenshotModel(R"({"id":"m1","input_types":["text"],"capabilities":{"vision":false}})", chats);
    FakeApiKeyStore keys;
    OpenAiCompatibleProvider provider(keys.Store("the test store", ApiKeyStore::kEndpointSlot), model.Url("/v1"), "m1");
    AgentTurnRequest request;
    request.UserText = "Look at the scene.";
    request.Tools = &attachment;
    RecordingTurnEvents events;
    CancelToken cancel;

    provider.RunTurn(request, events, cancel);

    ASSERT_EQ(events.Result.Outcome, TurnOutcome::Succeeded) << events.Result.Error;
    ASSERT_EQ(chats.size(), 2u);
    EXPECT_NE(chats[0].find("reads text only"), std::string::npos) << chats[0];
    EXPECT_NE(chats[1].find("ran take_screenshot --text-only"), std::string::npos) << chats[1];
    EXPECT_EQ(chats[1].find("image_url"), std::string::npos) << chats[1];

    const PromptFile image{.Name = "shot.png", .Kind = PromptFileKind::Image, .MediaType = "image/png", .Bytes = "\x89PNG"};
    request.Images = std::span<const PromptFile>(&image, 1);
    RecordingTurnEvents refused;
    provider.RunTurn(request, refused, cancel);
    EXPECT_EQ(refused.Result.Outcome, TurnOutcome::Failed);
    EXPECT_NE(refused.Result.Error.find("reads text only"), std::string::npos) << refused.Result.Error;
    EXPECT_EQ(chats.size(), 2u) << "no chat request is made";
}
} // namespace GameEngine
