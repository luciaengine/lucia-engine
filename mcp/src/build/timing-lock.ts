import { execFileSync } from "child_process";
import * as fs from "fs";
import * as path from "path";
import { z } from "zod";
import { repoRoot } from "../paths.js";
import { ToolError } from "../registry.js";

// The timing lock (AB-LOCK) is the team's gate for ms/fps measurements: while
// the file exists and its text says "timing", no build, test run or editor may
// start on this machine. The workbench scripts take, release and expire it;
// this module only reads it, so the engine tools honour the lock without
// depending on the workbench. Location, as the workbench resolves it:
//   - AB_LOCK_FILE, when set;
//   - Windows: C:\Dev\AB-LOCK;
//   - macOS and Linux: AB-LOCK in the directory that holds the primary
//     checkout, beside the worktrees.
// The holder's own launches pass when AB_LOCK_HOLDER names the id written after
// "timing:". A lock past its "expires <epoch seconds>" plus the grace period
// belongs to a dead or overrunning holder and does not block; removing it is
// left to the workbench scripts.

const kExpiryGraceSeconds = 120;
const kPollMs = 5000;

/** The `waitForTimingLockSec` argument every tool that starts a build, test run or editor takes. */
export const kTimingLockWaitSchema = z.coerce.number().optional().describe(
  "Seconds to wait while the AB-LOCK timing lock is held before refusing (default: 0, refuse at once)");

function primaryCheckoutParent(root: string): string {
  try {
    const commonDir = execFileSync("git", ["-C", root, "rev-parse", "--path-format=absolute", "--git-common-dir"],
      { encoding: "utf8", timeout: 5000, stdio: ["ignore", "pipe", "ignore"] }).trim();
    return path.dirname(path.dirname(commonDir));
  } catch {
    return path.dirname(root);
  }
}

export function timingLockPath(): string {
  if (process.env.AB_LOCK_FILE) return process.env.AB_LOCK_FILE;
  if (process.platform === "win32") return "C:\\Dev\\AB-LOCK";
  return path.join(primaryCheckoutParent(repoRoot()), "AB-LOCK");
}

function escapeRegExp(text: string): string {
  return text.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
}

/** The lock's text when it blocks a new start, else null. */
function blockingLockText(lockPath: string): string | null {
  let text: string;
  try {
    text = fs.readFileSync(lockPath, "utf8").trim();
  } catch {
    return null;
  }
  if (!/timing/i.test(text)) return null;
  const expires = text.match(/expires (\d{9,})/);
  if (expires && Date.now() / 1000 > Number(expires[1]) + kExpiryGraceSeconds) return null;
  const holder = process.env.AB_LOCK_HOLDER;
  if (holder && new RegExp(`timing: *#?${escapeRegExp(holder)}([^0-9]|$)`, "i").test(text)) return null;
  return text;
}

function refusal(lockPath: string, text: string, waitedSec: number): ToolError {
  const owner = text.match(/owner pid (\d+) on ([^;\s]+)/);
  const workbench = process.env.GE_WORKBENCH;
  const waitScript = workbench
    ? `bash ${path.join(workbench, "agent", "wait-ab-lock.sh")}`
    : "the workbench's agent/wait-ab-lock.sh";
  return new ToolError(
    `The timing lock ${lockPath} is held${owner ? ` by pid ${owner[1]} on ${owner[2]}` : ""}`
    + `${waitedSec > 0 ? ` (waited ${waitedSec}s)` : ""}: "${text}". No build, test run or editor may start `
    + `while a timing measurement runs. Retry after the lock is gone: pass waitForTimingLockSec, or run `
    + `${waitScript} before this command. The holder's own launches set AB_LOCK_HOLDER to its id.`);
}

/**
 * Returns when no timing lock blocks a new build, test run or editor start;
 * polls for up to `waitSec` seconds while one does, then throws a ToolError
 * naming the lock file, its owner and the fix.
 */
export async function passTimingLock(waitSec = 0): Promise<void> {
  const lockPath = timingLockPath();
  const deadline = Date.now() + waitSec * 1000;
  const start = Date.now();
  for (;;) {
    const text = blockingLockText(lockPath);
    if (text === null) return;
    if (Date.now() >= deadline) throw refusal(lockPath, text, Math.round((Date.now() - start) / 1000));
    await new Promise(r => setTimeout(r, Math.min(kPollMs, Math.max(deadline - Date.now(), 0))));
  }
}
