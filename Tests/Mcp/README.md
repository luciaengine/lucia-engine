# MCP and CLI gates

Node test scripts that guard the agent tool surface — the MCP server and the `ge`
CLI in `mcp/`, and the editor debug-server handlers they talk to. They read
sources as text or drive the CLI against a faked editor, so none of them needs a
built editor, a GPU, or a specific vendor of graphics hardware.

Each is registered as a ctest in the root `CMakeLists.txt` under `BUILD_TESTING`,
and each is runnable on its own with `node Tests/Mcp/<script>` from the repo root.
Exit 0 means the gate held, 1 means it caught something, 2 means the gate itself
could not run.

| Script | ctest name | What it fails on |
|---|---|---|
| `ipc-method-drift.mjs` | `EditorIpcMethodDrift` | A `RegisterHandler()` in `Apps/Editor/Source/DebugServer` that is neither exposed as a tool in `mcp/src/tools` nor listed in `mcp/ipc-method-coverage.json` with a reason for staying raw-only. Also catches stale and redundant entries in that list. |
| `mcp-editor-endpoint-check.mjs` | `McpEditorEndpointReport` | An answer that does not name which editor produced it. Several editors run at once, so a call that lands on the wrong one returns plausible data; every answer, including a refusal, must report the endpoint taken from the live socket. |
| `rgp-capture-check.mjs` | `McpRgpCaptureGate` | The Radeon GPU Profiler flow attempting a capture on non-AMD hardware, refusing without naming what it found, or reporting a stale `.rgp` file as a fresh capture. |
| `tool-gate-check.mjs` | `McpToolGate` | A lifecycle tool starting after the optional `GE_MCP_TOOL_GATE` command refuses it, or refusing when that command succeeds or is unset. The existing timing lock also remains enforced. |
| `timing-lock-check.mjs` | `McpTimingLockGate` | `build_engine`, `run_tests`, `launch_editor` or `editor_full_cycle` starting while the `AB-LOCK` file holds a timing measurement, refusing without naming the lock file, its owner and the wait script, or refusing when the lock is absent, not a timing lock, expired or held by the caller. |
| `send-key-digits-check.mjs` | `McpSendKeyDigits` | `ge send_key --key 8` sending the raw key code 8 instead of the digit key, a raw key code from 32 to 348 no longer sent as given, or `--key 12` or `--key 349` sent to the editor instead of refused with the fix. |
| `batch-stdin-check.mjs` | `McpBatchStdin` | `ge batch < requests.ndjson` not ending with its own exit code (0 when the editor answers every request, 3 when no editor listens), or `ge batch --help` connecting to an editor instead of printing the batch help. |

`ipc-method-drift.mjs` also runs in CI before anything is built, and takes
`--list` to print every debug-server method with how it is reached.

The harness gates spawn the CLI through `mcp/ge.mjs`, which rebuilds
`mcp/dist` when it is stale — that first build is why their ctest timeout is
generous.
