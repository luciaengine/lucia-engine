// build_engine, run_tests, launch_editor and editor_full_cycle must not start
// anything while the AB-LOCK timing lock is held.
//
// A timing measurement holds the machine through the lock file; a build, test
// run or editor that starts inside the window voids it. The tools read the lock
// themselves, so an agent that calls them directly cannot slip past the gate.
//
// Each arm runs the CLI against an empty repository root (GAMEENGINE_ROOT) and a
// lock file of its own (AB_LOCK_FILE), so no real build, editor or lock is ever
// touched: past the gate, cmake and ctest fail at once on the missing build
// directory and launch_editor finds no editor, and those failures are the proof
// that the gate let the tool run.
//
// Exit 0 = every arm held; 1 = a failure; 2 = the harness itself could not run.
//
//   node Tests/Mcp/timing-lock-check.mjs

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
const workDir = fs.mkdtempSync(path.join(os.tmpdir(), 'timing-lock-'));

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

function spawnCli(args, extraEnv) {
  return new Promise((resolve, reject) => {
    const remaining = deadline - Date.now();
    if (remaining <= 0) {
      reject(new Error(`ran out of the ${kTotalBudgetMs}ms budget before: ge ${args.join(' ')}`));
      return;
    }
    const env = { ...process.env, ...extraEnv };
    delete env.GE_MCP_TOOL_GATE;
    delete env.AB_LOCK_HOLDER;
    if (extraEnv.AB_LOCK_HOLDER) env.AB_LOCK_HOLDER = extraEnv.AB_LOCK_HOLDER;
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

const lockFile = path.join(workDir, 'AB-LOCK');
const nowSec = Math.floor(Date.now() / 1000);
const heldText = `timing: 4242, window; held at most 30 minutes; owner pid 31337 on lockhost; expires ${nowSec + 1800}`;
const expiredText = `timing: 4242, window; owner pid 31337 on lockhost; expires ${nowSec - 600}`;

// What each tool prints once it is past the gate, with no build or editor to act on.
const kTools = [
  { name: 'build_engine', args: ['build_engine'], ran: /BUILD (OK|FAILED)/ },
  { name: 'run_tests', args: ['run_tests'], ran: /TESTS (PASSED|FAILED)/ },
  { name: 'launch_editor', args: ['launch_editor'], ran: /Editor not found at/ },
  { name: 'editor_full_cycle', args: ['editor_full_cycle'], ran: /BUILD FAILED|Editor exe not found/ },
];

/** Runs `tool` with the lock file holding `text` (absent when null). */
async function runWithLock(tool, text, extraEnv = {}, extraArgs = []) {
  fs.rmSync(lockFile, { force: true });
  if (text !== null) fs.writeFileSync(lockFile, `${text}\n`);
  return spawnCli([...tool.args, ...extraArgs], { ...baseEnv, ...extraEnv });
}

function expectRefused(label, tool, result) {
  check(`${label}: ${tool.name} refuses`, result.code !== 0 && result.output.includes(lockFile)
      && /pid 31337 on lockhost/.test(result.output) && /wait-ab-lock\.sh/.test(result.output),
    `exit ${result.code}; output must name the lock file, its owner and the wait script:\n${result.output.trim()}`);
  check(`${label}: ${tool.name} starts nothing`, !tool.ran.test(result.output),
    `the tool ran past the lock:\n${result.output.trim()}`);
}

function expectRan(label, tool, result) {
  check(`${label}: ${tool.name} runs`, tool.ran.test(result.output) && !result.output.includes('timing lock'),
    `exit ${result.code}; expected ${tool.ran} and no lock refusal:\n${result.output.trim()}`);
}

let baseEnv;
try {
  baseEnv = {
    GAMEENGINE_ROOT: makeEmptyRepoRoot(),
    GE_EDITOR_DEBUG_PORT: String(await freePort()),
    AB_LOCK_FILE: lockFile,
  };
} catch (e) {
  console.error(`timing-lock-check: harness could not start: ${e.message}`);
  process.exit(2);
}

try {
  for (const tool of kTools) {
    expectRefused('timing lock held', tool, await runWithLock(tool, heldText));
    expectRan('no lock file', tool, await runWithLock(tool, null));
  }

  const build = kTools[0];
  const launch = kTools[2];
  for (const tool of [build, launch]) {
    expectRan('lock without "timing"', tool, await runWithLock(tool, 'released 2026-10-13 21:04'));
  }
  expectRan('holder\'s own launch', launch, await runWithLock(launch, heldText, { AB_LOCK_HOLDER: '4242' }));
  expectRefused('another holder\'s id', launch, await runWithLock(launch, heldText, { AB_LOCK_HOLDER: '424' }));
  expectRan('lock past its expiry', launch, await runWithLock(launch, expiredText));

  const waited = await runWithLock(launch, heldText, {}, ['--wait-for-timing-lock-sec', '1']);
  expectRefused('bounded wait', launch, waited);
  check('bounded wait: the refusal reports the wait', /waited 1s/.test(waited.output), waited.output.trim());
} catch (e) {
  console.error(`timing-lock-check: harness failed: ${e.message}`);
  fs.rmSync(workDir, { recursive: true, force: true });
  process.exit(2);
}

fs.rmSync(workDir, { recursive: true, force: true });
if (failures.length > 0) {
  console.error(`timing-lock-check: ${failures.length} failure(s)\n  ${failures.join('\n  ')}`);
  process.exit(1);
}
console.log('timing-lock-check: every tool refused under a timing lock and ran without one');
