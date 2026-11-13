import { execFile, spawn } from "child_process";
import * as fs from "fs";
import * as net from "net";
import * as path from "path";
import { promisify } from "util";

const execFileAsync = promisify(execFile);

// System32 tool by absolute path — the MCP host's PATH is not guaranteed.
export function system32(tool: string): string {
  return path.join(process.env.SystemRoot ?? "C:\\Windows", "System32", tool);
}

// True if something is listening on the local TCP port. A connect probe is
// locale-invariant and needs no subprocess, unlike parsing netstat output.
export function isPortListening(port: number): Promise<boolean> {
  return new Promise((resolve) => {
    const probe = net.createConnection({ port, host: "127.0.0.1" });
    probe.setTimeout(1000);
    probe.on("connect", () => { probe.destroy(); resolve(true); });
    probe.on("timeout", () => { probe.destroy(); resolve(true); }); // listening but slow to accept
    probe.on("error", () => resolve(false));
  });
}

// Find the PID listening on a local TCP port, or null if it can't be resolved
// (tool missing, or localized netstat output that doesn't match) — callers
// must NOT treat null as "port free"; use isPortListening for that.
export async function findPidListeningOnPort(port: number): Promise<number | null> {
  try {
    if (process.platform === "win32") {
      const { stdout } = await execFileAsync(system32("netstat.exe"), ["-ano", "-p", "TCP"],
        { timeout: 5000, maxBuffer: 10 * 1024 * 1024 });
      for (const line of stdout.split("\n")) {
        // "LISTENING" is not localized-proof; on a non-English Windows this
        // can fail to match — callers handle null by refusing, not killing.
        const m = line.match(/TCP\s+\S+:(\d+)\s+\S+\s+LISTENING\s+(\d+)/i);
        if (m && Number(m[1]) === port) return Number(m[2]);
      }
    } else {
      const { stdout } = await execFileAsync("lsof", ["-ti", `tcp:${port}`, "-sTCP:LISTEN"], { timeout: 5000 });
      const pid = parseInt(stdout.trim().split("\n")[0], 10);
      if (!isNaN(pid)) return pid;
    }
  } catch { /* unresolvable */ }
  return null;
}

// True if the PID belongs to an Editor process (never kill anything else).
export async function isEditorProcess(pid: number): Promise<boolean> {
  try {
    if (process.platform === "win32") {
      const { stdout } = await execFileAsync(system32("tasklist.exe"), ["/FI", `PID eq ${pid}`, "/FO", "CSV", "/NH"], { timeout: 5000 });
      return /"Editor\.exe"/i.test(stdout);
    }
    const { stdout } = await execFileAsync("ps", ["-p", String(pid), "-o", "comm="], { timeout: 5000 });
    return path.basename(stdout.trim()) === "Editor";
  } catch {
    return false;
  }
}

export function isProcessAlive(pid: number): boolean {
  try { process.kill(pid, 0); return true; } catch { return false; }
}

export async function killPid(pid: number): Promise<void> {
  if (process.platform === "win32") {
    await execFileAsync(system32("taskkill.exe"), ["/PID", String(pid), "/F"], { timeout: 5000 });
  } else {
    process.kill(pid, "SIGKILL");
  }
}

export async function pollUntil(predicate: () => Promise<boolean>, timeoutMs: number, intervalMs = 250): Promise<boolean> {
  const start = Date.now();
  while (Date.now() - start < timeoutMs) {
    if (await predicate()) return true;
    await new Promise(r => setTimeout(r, intervalMs));
  }
  return predicate();
}

// This process's environment with `variables` set over it. Windows names are case
// insensitive, so a registered PATH replaces the inherited Path rather than sitting
// beside it.
function withVariables(variables: Record<string, string>, clearGtestEnvironment: boolean): NodeJS.ProcessEnv {
  const env: NodeJS.ProcessEnv = { ...process.env };
  for (const [name, value] of Object.entries(variables)) {
    if (process.platform === "win32") {
      for (const existing of Object.keys(env)) if (existing.toLowerCase() === name.toLowerCase()) delete env[existing];
    }
    env[name] = value;
  }
  if (clearGtestEnvironment) {
    for (const name of Object.keys(env)) if (name.toUpperCase().startsWith("GTEST_")) delete env[name];
  }
  return env;
}

export interface LoggedRun {
  exitCode: number | null;
  timedOut: boolean;
  durationMs: number;
  cleanupUncertain: boolean;
}

/**
 * Streams a direct child's output to a fresh log. On timeout only that child is
 * signalled; descendant cleanup is unverified and must be checked by the owner.
 */
export function runToLog(command: string, args: string[],
  options: { cwd: string; logPath: string; timeoutMs: number; env?: Record<string, string>; clearGtestEnvironment?: boolean }): Promise<LoggedRun> {
  fs.mkdirSync(path.dirname(options.logPath), { recursive: true });
  const log = fs.openSync(options.logPath, "wx");
  const start = Date.now();
  return new Promise((resolve) => {
    let timedOut = false;
    let finished = false;
    let timer: ReturnType<typeof setTimeout> | undefined;
    const finish = (exitCode: number | null, cleanupUncertain: boolean) => {
      if (finished) return;
      finished = true;
      if (timer) clearTimeout(timer);
      fs.closeSync(log);
      resolve({ exitCode, timedOut, cleanupUncertain, durationMs: Date.now() - start });
    };
    let child: ReturnType<typeof spawn>;
    try {
      child = spawn(command, args, {
        cwd: options.cwd,
        env: withVariables(options.env ?? {}, options.clearGtestEnvironment ?? false),
        stdio: ["ignore", log, log],
        windowsHide: true,
      });
    } catch (error) {
      fs.writeSync(log, `\n${String(error)}\n`);
      finish(null, false);
      return;
    }
    timer = setTimeout(() => {
      if (finished) return;
      timedOut = true;
      fs.writeSync(log, "\nTimed out: direct-child termination requested; descendant cleanup is unverified.\n");
      // No PID search, process-name match or later signal against a recycled PID.
      if (child.exitCode === null && child.signalCode === null) child.kill();
    }, options.timeoutMs);
    child.on("error", (error) => {
      if (finished) return;
      fs.writeSync(log, `\n${error.message}\n`);
      finish(null, child.pid !== undefined);
    });
    child.on("close", (code) => finish(code, timedOut));
  });
}

/**
 * A file-name-safe local timestamp with milliseconds and this process's id, so two
 * runs of the same tool keep separate logs; names sort in time order within a process.
 */
let runSequence = 0;

export function runStamp(): string {
  const now = new Date();
  const pad = (value: number, width = 2) => String(value).padStart(width, "0");
  return `${now.getFullYear()}${pad(now.getMonth() + 1)}${pad(now.getDate())}-${pad(now.getHours())}${pad(now.getMinutes())}${pad(now.getSeconds())}-${pad(now.getMilliseconds(), 3)}-${process.pid}-${++runSequence}`;
}
