#!/usr/bin/env node
import { z } from "zod";
import { zodToJsonSchema } from "zod-to-json-schema";
import { callServedTool, servedTools } from "./assistant.js";
import { ipc } from "./ipc-singleton.js";

// The editor's tools for an AI Assistant turn on a model API (the custom endpoint,
// TensorFold), where the editor runs the conversation itself instead of a CLI that
// speaks MCP. The editor starts this script once per request, with the same
// environment as the MCP server (GE_EDITOR_DEBUG_PORT, GE_MCP_TOOLS,
// GE_ASSISTANT_TOKEN), so a call binds to the conversation and the editor's gate
// decides it as it decides the MCP server's:
//
//   assistant-bridge list         the served tools as JSON: name, description and the
//                                 JSON Schema of the arguments
//   assistant-bridge call <tool> [--text-only]
//                                 reads the arguments as one JSON object on stdin and
//                                 prints the answer as callServedTool returns it; with
//                                 --text-only an image becomes a line that says one was
//                                 taken (for a model without image input, and so a
//                                 screenshot never makes the answer longer than a line
//                                 the editor reads)
//
// Each prints one JSON document on stdout and exits 0; a usage error exits 2.

async function readStdin(): Promise<string> {
  const chunks: Buffer[] = [];
  for await (const chunk of process.stdin) chunks.push(chunk as Buffer);
  return Buffer.concat(chunks).toString("utf8");
}

function refusal(text: string) {
  return { content: [{ type: "text", text }], isError: true };
}

async function main(): Promise<number> {
  const [command, name, ...flags] = process.argv.slice(2);
  const textOnly = flags.includes("--text-only");
  const tools = servedTools();
  if (command === "list") {
    console.log(JSON.stringify(tools.map(tool => ({
      name: tool.name,
      description: tool.description,
      inputSchema: zodToJsonSchema(z.object(tool.schema), { target: "openApi3" }),
    }))));
    return 0;
  }
  if (command === "call" && name) {
    const tool = tools.find(t => t.name === name);
    if (!tool) {
      console.log(JSON.stringify(refusal(`The assistant has no tool '${name}'. Call one of the listed tools.`)));
      return 0;
    }
    let args: unknown;
    try {
      const text = await readStdin();
      args = text.trim() === "" ? {} : JSON.parse(text);
    } catch (err: any) {
      console.log(JSON.stringify(refusal(`The arguments of ${name} are not JSON: ${err?.message ?? String(err)}`)));
      return 0;
    }
    const parsed = z.object(tool.schema).safeParse(args);
    if (!parsed.success) {
      const issues = parsed.error.issues.map(i => `${i.path.join(".") || "(arguments)"}: ${i.message}`).join("; ");
      console.log(JSON.stringify(refusal(`The arguments of ${name} do not match its schema: ${issues}`)));
      return 0;
    }
    const answer = await callServedTool(tool, parsed.data as Record<string, unknown>);
    if (textOnly)
      answer.content = answer.content.map(block => block.type === "image"
        ? { type: "text", text: `[A ${block.mimeType} image was taken; this connection gets tool results as text only.]` }
        : block);
    console.log(JSON.stringify(answer));
    return 0;
  }
  console.error("Usage: assistant-bridge list | assistant-bridge call <tool> [--text-only] (arguments as JSON on stdin)");
  return 2;
}

main().then(
  code => {
    ipc().disconnect();
    process.exit(code);
  },
  err => {
    console.error(`[assistant-bridge] ${err?.message ?? String(err)}`);
    process.exit(1);
  },
);
