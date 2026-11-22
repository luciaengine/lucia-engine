#!/usr/bin/env node
import { randomUUID } from "node:crypto";
import { mkdirSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { z } from "zod";
import { zodToJsonSchema } from "zod-to-json-schema";
import { callServedTool, servedTools, type ToolContent } from "./assistant.js";
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
//   assistant-bridge call <tool> [--text-only | --image-dir <folder>]
//                                 reads the arguments as one JSON object on stdin and
//                                 prints the answer as callServedTool returns it. An
//                                 image never travels on stdout, so the answer stays
//                                 shorter than a line the editor reads: --text-only
//                                 makes it a line that says one was taken (a model
//                                 without image input); --image-dir writes it to a new
//                                 file in <folder> and makes it an image_file block
//                                 with the file's path, which the editor reads and
//                                 deletes
//
// Each prints one JSON document on stdout and exits 0; a usage error exits 2.

async function readStdin(): Promise<string> {
  const chunks: Buffer[] = [];
  for await (const chunk of process.stdin) chunks.push(chunk as Buffer);
  return Buffer.concat(chunks).toString("utf8");
}

// Writes one JSON document and resolves once it has left the process: stdout to a pipe is
// written asynchronously, and exiting before the write ends cuts a long answer short.
function print(value: unknown): Promise<void> {
  return new Promise(resolve => process.stdout.write(JSON.stringify(value) + "\n", () => resolve()));
}

function refusal(text: string) {
  return { content: [{ type: "text", text }], isError: true };
}

type BridgeContent = ToolContent | { type: "image_file"; path: string; mimeType: string };

// An image block as the editor reads it: a text line, a file, or (with neither flag) as it is.
function imageBlock(block: ToolContent, textOnly: boolean, imageDir: string | undefined): BridgeContent {
  if (block.type !== "image") return block;
  if (textOnly)
    return { type: "text", text: `[A ${block.mimeType} image was taken; this connection gets tool results as text only.]` };
  if (!imageDir) return block;
  mkdirSync(imageDir, { recursive: true });
  const path = join(imageDir, `${randomUUID()}.${block.mimeType === "image/png" ? "png" : "img"}`);
  writeFileSync(path, Buffer.from(block.data, "base64"));
  return { type: "image_file", path, mimeType: block.mimeType };
}

async function main(): Promise<number> {
  const [command, name, ...flags] = process.argv.slice(2);
  const textOnly = flags.includes("--text-only");
  const imageDirFlag = flags.indexOf("--image-dir");
  const imageDir = imageDirFlag >= 0 ? flags[imageDirFlag + 1] : undefined;
  const tools = servedTools();
  if (command === "list") {
    await print(tools.map(tool => ({
      name: tool.name,
      description: tool.description,
      inputSchema: zodToJsonSchema(z.object(tool.schema), { target: "openApi3" }),
    })));
    return 0;
  }
  if (command === "call" && name) {
    const tool = tools.find(t => t.name === name);
    if (!tool) {
      await print(refusal(`The assistant has no tool '${name}'. Call one of the listed tools.`));
      return 0;
    }
    let args: unknown;
    try {
      const text = await readStdin();
      args = text.trim() === "" ? {} : JSON.parse(text);
    } catch (err: any) {
      await print(refusal(`The arguments of ${name} are not JSON: ${err?.message ?? String(err)}`));
      return 0;
    }
    const parsed = z.object(tool.schema).safeParse(args);
    if (!parsed.success) {
      const issues = parsed.error.issues.map(i => `${i.path.join(".") || "(arguments)"}: ${i.message}`).join("; ");
      await print(refusal(`The arguments of ${name} do not match its schema: ${issues}`));
      return 0;
    }
    const answer = await callServedTool(tool, parsed.data as Record<string, unknown>);
    await print({ ...answer, content: answer.content.map(block => imageBlock(block, textOnly, imageDir)) });
    return 0;
  }
  console.error("Usage: assistant-bridge list | assistant-bridge call <tool> [--text-only | --image-dir <folder>] (arguments as JSON on stdin)");
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
