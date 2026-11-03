// build_engine, run_tests, launch_editor and editor_full_cycle must run the
// command named by GE_MCP_TOOL_GATE before they start anything, refuse when it
// exits non-zero (echoing its output), and run when it exits 0 or is unset.
//
// A team workflow points the variable at its own wait script, so an agent that
// calls the tools directly cannot slip past the workflow's gate; the engine
// knows nothing of what the command checks.
//
// Each arm runs the CLI against an empty repository root (GAMEENGINE_ROOT), so
// no real build or editor is ever touched: past the gate, cmake and ctest fail
// at once on the missing build directory and launch_editor finds no editor, and
// those failures are the proof that the gate let the tool run. For the same
// reason editor_full_cycle's second gate call, between its build and the editor
// start, is not reached here.
//
// Exit 0 = every arm held; 1 = a failure; 2 = the harness itself could not run.
//
//   node Tests/Mcp/tool-gate-check.mjs

import { spawn } from 'node:child_process';
import fs from 'node:fs';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const repoRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..', '..');
const geEntry = path.join(repoRoot, 'mcp', 'ge.mjs');

// One budget for the whole run; the first spawn may install and compile mcp/.
const kTotalBudgetMs = 540_000;
const deadline = Date.now() + kTotalBudgetMs;

const failures = [];
const workDir = fs.mkdtempSync(path.join(os.tmpdir(), 'tool-gate-'));

function check(label, condition, detail) {
  if (!condition) failures.push(`${label}\n    ${detail}`);
}

/** A directory the CLI accepts as a repository root, with no build in it. */
function makeEmptyRepoRoot() {
  const root = path.join(workDir, 'repo');
  fs.mkdirSync(path.join(root, 'Engine'), { recursive: true });
  fs.mkdirSync(path.join(root, 'Apps'), { recursive: true });
  fs.writeFileSync(path.join(root, 'CMakePresets.json'), '{}');
  return root;
}

/** A local port nothing listens on, so the debug-port step finds no editor. */
function freePort() {
  return new Promise((resolve, reject) => {
    const server = net.createServer();
    server.on('error', reject);
    server.listen(0, '127.0.0.1', () => {
      const { port } = server.address();
      server.close(() => resolve(port));
    });
  });
}

/** A gate command: node running a script that prints `line` and exits `code`. */
function makeGate(name, line, code) {
  const script = path.join(workDir, `${name}.mjs`);
  fs.writeFileSync(script, `console.log(${JSON.stringify(line)});\nprocess.exit(${code});\n`);
  return `"${process.execPath}" "${script}"`;
}

function spawnCli(args, gate) {
  return new Promise((resolve, reject) => {
    const remaining = deadline - Date.now();
    if (remaining <= 0) {
      reject(new Error(`ran out of the ${kTotalBudgetMs}ms budget before: ge ${args.join(' ')}`));
      return;
    }
    const env = { ...process.env, ...baseEnv };
    delete env.GE_MCP_TOOL_GATE;
    if (gate !== null) env.GE_MCP_TOOL_GATE = gate;
    const child = spawn(process.execPath, [geEntry, ...args], {
      cwd: repoRoot,
      env,
      stdio: ['ignore', 'pipe', 'pipe'],
    });
    let output = '';
    child.stdout.on('data', (d) => { output += d.toString(); });
    child.stderr.on('data', (d) => { output += d.toString(); });
    const timer = setTimeout(() => {
      child.kill();
      reject(new Error(`timed out: ge ${args.join(' ')}`));
    }, remaining);
    child.on('error', (e) => { clearTimeout(timer); reject(e); });
    child.on('close', (code) => { clearTimeout(timer); resolve({ code, output }); });
  });
}

// What each tool prints once it is past the gate, with no build or editor to act on.
const kTools = [
  { name: 'build_engine', args: ['build_engine'], ran: /BUILD (OK|FAILED)/ },
  { name: 'run_tests', args: ['run_tests'], ran: /TESTS (PASSED|FAILED)/ },
  { name: 'launch_editor', args: ['launch_editor'], ran: /Editor not found at/ },
  { name: 'editor_full_cycle', args: ['editor_full_cycle'], ran: /BUILD FAILED|Editor exe not found/ },
];

const kRefusalLine = 'gate-check: a timing measurement holds this machine';

function expectRefused(label, tool, result) {
  check(`${label}: ${tool.name} refuses`, result.code !== 0 && result.output.includes(kRefusalLine)
      && /GE_MCP_TOOL_GATE/.test(result.output) && /exit 1/.test(result.output),
    `exit ${result.code}; output must name the gate, its exit and echo its output:\n${result.output.trim()}`);
  check(`${label}: ${tool.name} starts nothing`, !tool.ran.test(result.output),
    `the tool ran past the gate:\n${result.output.trim()}`);
}

function expectRan(label, tool, result) {
  check(`${label}: ${tool.name} runs`, tool.ran.test(result.output) && !result.output.includes('tool gate'),
    `exit ${result.code}; expected ${tool.ran} and no gate refusal:\n${result.output.trim()}`);
}

let baseEnv;
let refusingGate;
let passingGate;
try {
  baseEnv = {
    GAMEENGINE_ROOT: makeEmptyRepoRoot(),
    GE_EDITOR_DEBUG_PORT: String(await freePort()),
    AB_LOCK_FILE: path.join(workDir, "absent-AB-LOCK"),
  };
  refusingGate = makeGate('refuse', kRefusalLine, 1);
  passingGate = makeGate('pass', 'gate-check: free', 0);
} catch (e) {
  console.error(`tool-gate-check: harness could not start: ${e.message}`);
  process.exit(2);
}

try {
  for (const tool of kTools) {
    expectRefused('gate exits 1', tool, await spawnCli(tool.args, refusingGate));
    expectRan('gate exits 0', tool, await spawnCli(tool.args, passingGate));
    expectRan('no gate set', tool, await spawnCli(tool.args, null));
  }
} catch (e) {
  console.error(`tool-gate-check: harness failed: ${e.message}`);
  fs.rmSync(workDir, { recursive: true, force: true });
  process.exit(2);
}

fs.rmSync(workDir, { recursive: true, force: true });
if (failures.length > 0) {
  console.error(`tool-gate-check: ${failures.length} failure(s)\n  ${failures.join('\n  ')}`);
  process.exit(1);
}
console.log('tool-gate-check: every tool refused when the gate failed and ran when it passed or was unset');
