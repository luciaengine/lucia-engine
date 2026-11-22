#include "EditorToolBridge.h"

#include "CancelToken.h"
#include "JsonFields.h"
#include "PromptFiles.h"

#include "Engine/Build/CancellableShellProcess.h"
#include "Platform/Process.h"
#include "Types/PathUtils.h"

#include <algorithm>
#include <filesystem>
#include <system_error>
#include <utility>

#include <nlohmann/json.hpp>

namespace GameEngine
{
namespace
{
constexpr const char* kBridgeFile = "assistant-bridge.js";

// One folder per editor process; each image file has a unique name, so turns can share it.
std::filesystem::path ImageFolder()
{
    std::error_code error;
    const std::filesystem::path base = std::filesystem::temp_directory_path(error);
    if (error || base.empty())
        return {};
    return base / "gameengine_mcp" / ("pid-" + std::to_string(Platform::GetCurrentProcessId())) / "tool-images";
}

bool IsCore(std::string_view name)
{
    return std::find(std::begin(EditorToolBridge::kCoreTools), std::end(EditorToolBridge::kCoreTools), name) !=
           std::end(EditorToolBridge::kCoreTools);
}

nlohmann::json Function(std::string_view name, std::string_view description, nlohmann::json parameters)
{
    return {{"type", "function"},
            {"function", {{"name", name}, {"description", description}, {"parameters", std::move(parameters)}}}};
}

// The text of an MCP answer's content blocks, joined by blank lines.
std::string ContentText(const nlohmann::json& answer)
{
    std::string text;
    const auto content = answer.find("content");
    if (content == answer.end() || !content->is_array())
        return text;
    for (const nlohmann::json& block : *content)
        if (block.is_object() && JsonFields::String(block, "type") == "text")
            text += (text.empty() ? "" : "\n\n") + JsonFields::String(block, "text");
    return text;
}
} // namespace

EditorToolBridge::EditorToolBridge(const AgentToolAttachment& attachment, ToolImages images)
    : m_Attachment(attachment),
      m_BridgeScript((std::filesystem::path(attachment.ServerScript).parent_path() / kBridgeFile).string()),
      m_ImageFolder(images == ToolImages::Returned ? ImageFolder() : std::filesystem::path())
{
    // Without a temporary folder an image cannot leave the bridge, so the model gets the line.
    m_Images = m_ImageFolder.empty() ? ToolImages::Described : images;
}

bool EditorToolBridge::Load(const CancelToken& cancel, std::string& error)
{
    ProcessLaunch launch;
    launch.Executable = m_Attachment.Node;
    launch.Arguments = {m_BridgeScript, "list"};
    for (const auto& [name, value] : m_Attachment.Environment)
        launch.Environment.push_back({name, value});
    std::string listed;
    CancellableShellProcess process;
    const ShellProcessResult run = process.Run(
        launch, [&cancel] { return cancel.IsCancelled(); }, [&listed](const std::string& line) { listed += line; });
    const nlohmann::json tools = nlohmann::json::parse(listed, nullptr, false);
    if (run.exitCode != 0 || !tools.is_array())
    {
        error = "The editor's tools could not be listed (Node.js at " + m_Attachment.Node + ", exit code " +
                std::to_string(run.exitCode) + (run.stderrTail.empty() ? std::string() : ": " + run.stderrTail) +
                "). Install Node.js 20 or later, or set Node executable in Settings > AI Assistant > Model and "
                "options, and build the McpServer target.";
        return false;
    }
    m_Tools.clear();
    for (const nlohmann::json& tool : tools)
        if (tool.is_object() && !JsonFields::String(tool, "name").empty())
            m_Tools.push_back(tool);
    return true;
}

nlohmann::json EditorToolBridge::Definitions() const
{
    nlohmann::json definitions = nlohmann::json::array();
    if (m_Tools.empty())
        return definitions;
    std::string others;
    for (const nlohmann::json& tool : m_Tools)
    {
        const std::string name = JsonFields::String(tool, "name");
        if (IsCore(name))
        {
            const auto schema = tool.find("inputSchema");
            definitions.push_back(Function(name, JsonFields::String(tool, "description"),
                                           schema != tool.end() && schema->is_object()
                                               ? *schema
                                               : nlohmann::json{{"type", "object"}, {"properties", nlohmann::json::object()}}));
        }
        else
        {
            others += (others.empty() ? "" : ", ") + name;
        }
    }
    definitions.push_back(Function(
        kDescribeTool,
        "Returns the description and JSON Schema of one of the editor's other tools, before you call it with "
        "call_editor_tool. The other tools: " + others + ".",
        {{"type", "object"},
         {"properties", {{"tool", {{"type", "string"}, {"description", "The tool's name."}}}}},
         {"required", {"tool"}}}));
    definitions.push_back(Function(
        kCallTool,
        "Runs one of the editor's other tools (listed in describe_editor_tool) with its arguments.",
        {{"type", "object"},
         {"properties",
          {{"tool", {{"type", "string"}, {"description", "The tool's name."}}},
           {"arguments", {{"type", "object"}, {"description", "The tool's arguments, as its schema describes."}}}}},
         {"required", {"tool"}}}));
    return definitions;
}

EditorToolBridge::Answer EditorToolBridge::Call(std::string_view name, std::string_view arguments,
                                                const CancelToken& cancel) const
{
    const nlohmann::json parsed = nlohmann::json::parse(arguments.empty() ? std::string_view("{}") : arguments,
                                                        nullptr, false);
    if (!parsed.is_object())
        return {.Text = "The arguments of " + std::string(name) + " are not a JSON object.", .IsError = true};
    if (name == kDescribeTool)
    {
        const std::string tool = JsonFields::String(parsed, "tool");
        const nlohmann::json* found = FindTool(tool);
        if (!found)
            return {.Text = "The editor has no tool '" + tool + "'.", .IsError = true};
        return {.Text = found->dump()};
    }
    if (name == kCallTool)
    {
        const std::string tool = JsonFields::String(parsed, "tool");
        if (!FindTool(tool))
            return {.Text = "The editor has no tool '" + tool + "'.", .IsError = true};
        const auto inner = parsed.find("arguments");
        return RunTool(tool, inner != parsed.end() && inner->is_object() ? inner->dump() : "{}", cancel);
    }
    if (!IsCore(name) || !FindTool(name))
        return {.Text = "The editor has no tool '" + std::string(name) + "'; use call_editor_tool for the others.",
                .IsError = true};
    return RunTool(std::string(name), parsed.dump(), cancel);
}

std::string EditorToolBridge::RunsTool(std::string_view name, std::string_view arguments)
{
    if (name != kCallTool)
        return std::string(name);
    const nlohmann::json parsed = nlohmann::json::parse(arguments, nullptr, false);
    const std::string tool = parsed.is_object() ? JsonFields::String(parsed, "tool") : std::string();
    return tool.empty() ? std::string(name) : tool;
}

EditorToolBridge::Answer EditorToolBridge::RunTool(const std::string& name, const std::string& arguments,
                                                   const CancelToken& cancel) const
{
    ProcessLaunch launch;
    launch.Executable = m_Attachment.Node;
    launch.Arguments = {m_BridgeScript, "call", name};
    if (m_Images == ToolImages::Returned)
        launch.Arguments.insert(launch.Arguments.end(), {"--image-dir", PathUtf8(m_ImageFolder)});
    else
        launch.Arguments.push_back("--text-only");
    for (const auto& [variable, value] : m_Attachment.Environment)
        launch.Environment.push_back({variable, value});
    launch.StdinPayload = arguments;
    std::string printed;
    CancellableShellProcess process;
    const ShellProcessResult run = process.Run(
        launch, [&cancel] { return cancel.IsCancelled(); }, [&printed](const std::string& line) { printed += line; });
    if (run.cancelled)
        return {.Text = "Stopped.", .IsError = true};
    const nlohmann::json answer = nlohmann::json::parse(printed, nullptr, false);
    if (run.exitCode != 0 || !answer.is_object())
        return {.Text = "The tool " + name + " did not answer (exit code " + std::to_string(run.exitCode) +
                        (run.stderrTail.empty() ? std::string() : ": " + run.stderrTail) + ").",
                .IsError = true};
    const auto isError = answer.find("isError");
    Answer result{.Text = ContentText(answer),
                  .IsError = isError != answer.end() && isError->is_boolean() && isError->get<bool>()};
    if (const auto content = answer.find("content"); content != answer.end() && content->is_array())
        TakeImages(*content, result);
    return result;
}

void EditorToolBridge::TakeImages(const nlohmann::json& content, Answer& answer) const
{
    for (const nlohmann::json& block : content)
    {
        if (!block.is_object() || JsonFields::String(block, "type") != "image_file")
            continue;
        const std::filesystem::path path = PathFromUtf8(JsonFields::String(block, "path"));
        // Only a file the bridge wrote in this process's folder is read and deleted.
        if (m_Images != ToolImages::Returned || path.parent_path() != m_ImageFolder)
            continue;
        PromptFileLoad load = PromptFiles::Load(path);
        std::error_code error;
        std::filesystem::remove(path, error);
        if (load.File && load.File->Kind == PromptFileKind::Image)
            answer.Images.push_back(std::move(*load.File));
        else
            answer.Text += (answer.Text.empty() ? "" : "\n\n") + std::string("[The tool's image could not be read: ") +
                           (load.Refusal.empty() ? "it is not an image." : load.Refusal) + "]";
    }
}

const nlohmann::json* EditorToolBridge::FindTool(std::string_view name) const
{
    for (const nlohmann::json& tool : m_Tools)
        if (JsonFields::String(tool, "name") == name)
            return &tool;
    return nullptr;
}
} // namespace GameEngine
