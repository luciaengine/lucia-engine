import { spawn } from "child_process";
import { repoRoot } from "../paths.js";
import { ToolError } from "../registry.js";

// The tool gate: a command named by GE_MCP_TOOL_GATE that runs before every
// build, test run and editor start (a team workflow sets it to its wait script).
// Unset or empty means no gate. Exit 0 lets the tool run; any other exit
// refuses it, echoing the command's output. The engine knows nothing of what
// the command checks.

export const kToolGateVariable = "GE_MCP_TOOL_GATE";

/**
 * Runs the gate command, when one is set, and returns once it exits 0; throws a
 * ToolError naming the command, its exit and its output otherwise.
 */
export async function runToolGate(): Promise<void> {
  const command = process.env[kToolGateVariable]?.trim();
  if (!command) return;
  const { code, output } = await new Promise<{ code: number | null; output: string }>((resolve) => {
    const child = spawn(command, { cwd: repoRoot(), shell: true, stdio: ["ignore", "pipe", "pipe"] });
    let text = "";
    child.stdout.on("data", (d) => { text += d.toString(); });
    child.stderr.on("data", (d) => { text += d.toString(); });
    child.on("error", (e) => resolve({ code: null, output: `${text}${e.message}` }));
    child.on("close", (exit) => resolve({ code: exit, output: text }));
  });
  if (code === 0) return;
  const trimmed = output.trim();
  throw new ToolError(
    `Refused by the tool gate ${kToolGateVariable}="${command}" (exit ${code ?? "none"})`
    + (trimmed ? `:\n${trimmed}` : "."));
}
