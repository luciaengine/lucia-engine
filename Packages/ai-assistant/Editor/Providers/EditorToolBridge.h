#pragma once

#include "IAgentProvider.h"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace GameEngine
{
class CancelToken;

/// The editor's tools for a turn on a model API, where the editor runs the conversation itself:
/// it runs the staged `mcp/dist/assistant-bridge.js` beside the MCP server with the turn's
/// AgentToolAttachment (Node.js, the server's environment and its session token), once to list
/// the tools and once per call, so every call binds to the conversation and the editor's gate
/// decides it as it decides the MCP server's.
///
/// The model sees kCoreTools with their full schemas and two more: describe_editor_tool (the
/// schema of any other tool, answered from the list) and call_editor_tool (runs any tool by name).
/// A model with a small context keeps room for the conversation; every tool stays reachable.
///
/// An image a tool answers with (take_screenshot with includeImage) reaches the editor as a file,
/// never on the bridge's stdout: ToolImages::Returned reads it into Answer::Images and deletes it.
class EditorToolBridge
{
public:
    /// What the model gets for an image a tool answers with.
    enum class ToolImages : uint8_t
    {
        /// A line that says an image was taken: the model reads text only.
        Described,
        /// The image, in Answer::Images: the model reads images.
        Returned,
    };

    /// What one tool call answered: its text, and its images when ToolImages::Returned.
    struct Answer
    {
        std::string Text;
        bool IsError = false;
        std::vector<PromptFile> Images;
    };

    /// The tools sent with their full schemas: the everyday reads and edits.
    static constexpr std::string_view kCoreTools[] = {
        "get_editor_state", "get_scene_hierarchy", "get_entity_components", "query_ecs", "select_entity",
        "create_entity",    "set_component",       "delete_entity",         "undo",      "redo",
        "get_log",          "take_screenshot",     "get_camera",            "look_at",   "focus_view",
        "open_asset",       "save_scene",          "set_play_mode",         "execute_command",
        "list_creative_workspaces", "open_creative_workspace", "inspect_creative_document", "edit_creative_document"};
    /// The two tools that reach the rest.
    static constexpr std::string_view kDescribeTool = "describe_editor_tool";
    static constexpr std::string_view kCallTool = "call_editor_tool";

    EditorToolBridge(const AgentToolAttachment& attachment, ToolImages images);

    /// Lists the tools (assistant-bridge list) and keeps them for Definitions() and calls. False,
    /// with the reason in `error`, when the bridge does not start or answers no list.
    bool Load(const CancelToken& cancel, std::string& error);

    /// The OpenAI-style `tools` array: kCoreTools that the list has, then the two meta-tools,
    /// whose descriptions name every other tool. Empty before Load().
    nlohmann::json Definitions() const;

    /// Runs the call the model made: a core tool, describe_editor_tool or call_editor_tool, with
    /// `arguments` as the model wrote them (a JSON object as text).
    Answer Call(std::string_view name, std::string_view arguments, const CancelToken& cancel) const;

    /// The tool a call of `name` with `arguments` runs, for the conversation's call row:
    /// call_editor_tool's inner tool, else `name`.
    static std::string RunsTool(std::string_view name, std::string_view arguments);

private:
    Answer RunTool(const std::string& name, const std::string& arguments, const CancelToken& cancel) const;
    const nlohmann::json* FindTool(std::string_view name) const;
    /// Reads the image files an answer names into `answer`, and deletes them.
    void TakeImages(const nlohmann::json& content, Answer& answer) const;

    AgentToolAttachment m_Attachment;
    std::string m_BridgeScript;
    ToolImages m_Images = ToolImages::Described;
    /// Where the bridge writes images for this process (ToolImages::Returned).
    std::filesystem::path m_ImageFolder;
    /// The list's entries (name, description, inputSchema), in its order.
    std::vector<nlohmann::json> m_Tools;
};
} // namespace GameEngine
