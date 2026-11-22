import { ipcConfig } from "./config.js";
import { editorCall, editorCallCount, lastEditorEndpoint } from "./editor-call.js";
import type { ToolDef } from "./registry.js";
import { allTools } from "./tools/index.js";

// What an MCP server started for the editor's AI Assistant serves and asks. The
// editor writes both variables into the server's environment when a conversation
// attaches it; a server started without them serves every tool, as before.

/**
 * The tools this server registers: the names in GE_MCP_TOOLS (comma-separated),
 * or every tool when it is unset. Throws on a name the registry does not have, so
 * a mistyped or retired name stops the server at start instead of shrinking the
 * set without a word.
 */
export function servedTools(): readonly ToolDef[] {
  const list = process.env.GE_MCP_TOOLS;
  if (list === undefined) return allTools;
  const names = list.split(",").map(n => n.trim()).filter(n => n.length > 0);
  const known = new Set(allTools.map(t => t.name));
  const unknown = names.filter(n => !known.has(n));
  if (unknown.length > 0) {
    throw new Error(
      `GE_MCP_TOOLS names tools this server does not have: ${unknown.join(", ")}. ` +
      `List the server's tools with \`node mcp/ge.mjs --help\`, or rebuild the McpServer target if the editor is newer.`);
  }
  const wanted = new Set(names);
  return allTools.filter(t => wanted.has(t.name));
}

/**
 * Before a tool that acts on this machine runs (ToolDef.actsOnHost), a server bound
 * to an assistant session asks the editor's gate, as the editor gates the requests
 * it receives itself. Throws the editor's refusal; an unbound server asks nothing.
 */
export async function authorizeHostAction(tool: ToolDef, args: Record<string, unknown>): Promise<void> {
  if (!tool.actsOnHost || !ipcConfig().assistantToken) return;
  await editorCall("assistant_authorize", { tool: tool.name, arguments: args });
}

/** One MCP content block of a tool's answer. */
export type ToolContent = { type: "text"; text: string } | { type: "image"; data: string; mimeType: string };

/**
 * Runs one served tool as the MCP server answers it: the host-action check, then the
 * tool, its output as content blocks (the image first, then the summary or the data),
 * and a refusal or failure as one text block with isError. Every answer that came
 * from an editor names that editor. The MCP server and the assistant bridge both call
 * this, so a tool answers the same through either.
 */
export async function callServedTool(
  tool: ToolDef,
  args: Record<string, unknown>,
): Promise<{ content: ToolContent[]; isError?: true }> {
  // Only when this invocation actually reached an editor: attributing an endpoint to
  // a build or codegen tool would be a claim it never made. A refusal counts as
  // reaching one; an unreachable editor does not.
  const callsBefore = editorCallCount();
  const withEndpoint = (body: string) => {
    const endpoint = editorCallCount() > callsBefore ? lastEditorEndpoint() : null;
    return endpoint ? `${body}\n\n[editor ${endpoint}]` : body;
  };
  try {
    await authorizeHostAction(tool, args);
    const out = await tool.run(args);
    const content: ToolContent[] = [];
    // Image first: an agent reading the response sees the picture before the path.
    // Only an inline image goes in the answer; the others stay in the file the summary names.
    if (out.image?.inline) content.push({ type: "image", data: out.image.base64, mimeType: out.image.mimeType });
    content.push({ type: "text", text: withEndpoint(out.summary ?? JSON.stringify(out.data, null, 2)) });
    return { content };
  } catch (err: any) {
    return { content: [{ type: "text", text: withEndpoint(err?.message ?? String(err)) }], isError: true };
  }
}
