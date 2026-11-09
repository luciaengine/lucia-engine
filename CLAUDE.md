# CLAUDE.md

@AGENTS.md

Guidance beyond the shared entry point imported above. `CONTRIBUTING.md` is the short form of the rules for people; this file wins where they disagree. Area conventions load on demand from `.claude/rules/` when you open matching files. General team workflow guidance, test maps, evidence checklists, and maintained designs belong in this repository under `docs/development/`, with reusable scripts under `Tools/`. Only restricted console code and documentation belong in the second, private repository. General contributions use this repository and its documented dependencies.

## Engineering values

The bar for every change: high performance, high code quality, clean architecture, high usability. Aim for the best end state for the engine, not the fastest thing that works.

- **Fix causes, not symptoms.** When a bug exposes a design flaw, the design is the bug. Prefer the scoped refactor that leaves the engine simpler; if the right fix is bigger than the ask, say so and propose it.
- **No legacy by default.** There are no users or released projects to preserve. For every API, saved format and removal: update all current callers and committed assets, delete the obsolete path, keep only the supported implementation. No backward compatibility, saved-data migrations, deprecated aliases, fallback readers, or checks whose only purpose is detecting removed legacy data; ordinary validation of supported data stays. Preserve compatibility only when explicitly requested for a concrete consumer.
- **Performance is a feature.** Hot paths get data-oriented designs (SoA, contiguous iteration, flat maps over node-based containers). Performance claims are measured before and after, in Release; "should be neutral" is a hypothesis.
- **Code lives where it belongs.** Before adding to a file, ask whether that file owns the responsibility; extract a controller, service or system with a clear owner instead of appending to whatever file is open. Feature code never lands in a hub (`EditorApplication`, `EditorContext`, `SettingsPanel`, `InspectorPanel`, `HierarchyPanel`, `SceneViewPanel`, `RenderServices`, `UIManager`, `UIElement`, `core.css`); the feature pulls what it needs through the app's API and the hub never learns its name. A feature with more than one file (a panel with its modals, rows, controls and helpers) lives in a folder named for the feature (`Panels/ProjectPicker/`, `SceneView/Gizmos/`); a new file joins its feature's folder or starts one, never a flat folder of unrelated features.
- **Register, do not accrete.** Extensible surfaces take entries by registration from the owning module: settings categories through `Editor::EditorSettingsRegistry`, inspectors, post effects, dockables, and search items through their registries. Migrate a hand-built surface when the change materially alters that surface and the migration supports the requested behavior. Keep unrelated migrations separate.
- **Know what exists before you write.** Search for a suitable engine piece before adding math, noise, hashing, containers, string or time utilities, drawing helpers, or UI controls. Look in `Engine/Modules/`, engine services under `Engine/Include/` and `Engine/Source/`, and editor shared pieces under `Apps/Editor/Include/`. Reuse or extend a piece when its ownership and contract fit. Keep caller-specific behavior local when sharing would create the wrong dependency or broaden an unrelated contract. The `change-scrutiny` skill defines the evidence required for deliberate separation or duplication.
- **Usability counts as quality.** APIs that are hard to misuse, defaults that work, error messages that state the fix, editor UX that does not fight the user. Few settings with high-quality defaults: a dial whose non-default values are all worse is deleted, not shipped. Anything a user sees or operates passes the `change-scrutiny` skill's UI/UX pass before its pull request opens.
- **One engine, many kinds of games.** The engine serves first-person and third-person games, role-playing and open-world games, strategy, flight and space simulation, and their cutscenes at close-up quality. A game built on it is a customer and a source of test content, never the specification: design a system for the range, expose the difference as presets and tuning a game chooses, and do not cap quality or scope at what one game's camera needs. A measurement taken in one game's scene says so and names the scenes that would represent the others.

## Debugging

Get facts from a debugger, a capture or the live editor before theorizing, and confirm the instrument is reachable before reporting what it said. Method: the `root-cause-debugging` skill; layered "is X enabled" questions: `trace-the-chain`.

- Native crashes and hangs: cdb from the Windows SDK Debugging Tools (usually `C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\`). Snapshot a running process without stopping it: `cdb -pv -p <pid> -c "~*k; qd"`. Reproduce under the debugger: `cdb -g -G <exe> <args>`, then `!analyze -v`, `~*k`, `.frame N` and `dv /t`. Post-mortem: `cdb -z <dump> -c "!analyze -v; ~*k; q"`. PDBs sit beside the staged binaries. Several editors are often alive: verify PID and path before attaching, and never kill an editor you did not launch.
- Memory bugs: the `vs2026-x64-local-asan` preset (`macos-arm64-local-asan` on macOS), Debug configuration, ASan runtime DLLs staged automatically on Windows, `GE_ENABLE_ASAN=1` defined so perf-sensitive tests can relax thresholds. Data races: `macos-arm64-local-tsan` (ThreadSanitizer, Debug, tests on).
- GPU: RenderDoc and the Vulkan instruments, in `.claude/rules/rendering.md` and the `renderdoc-gpu-debug` skill.
- Live editor: the MCP tools (`mcp__gameengine__*`, setup in `docs/getting-started/mcp-setup.mdx`) and the CLI `node mcp/ge.mjs <tool>` (`--help` lists all; `--port` or `GE_EDITOR_DEBUG_PORT` targets a non-default editor; `ge raw <method> '{...}'` reaches methods with no tool). A `get_log` reply ends with a `next` line: paste it for the next call. The CLI's `create_component` writes into the CLI's own repository instead of the project (#5202): do not use it until that is fixed. Adding a `RegisterHandler` without exposing it as a tool or listing a reason in `mcp/ipc-method-coverage.json` fails `EditorIpcMethodDrift`. A backgrounded or occluded editor window may render black or throttled. Follow AGENTS.md’s desktop-session authorization rules; use an authorized validation session or report the affected visual check as not verified.

## Architecture in one screen

```
Engine/Include, Engine/Source   engine-level services           Apps/Editor    the editor (C++, CSS/XML UI)
Engine/Modules/<Name>           one library per subsystem       Apps/Player    the runtime
Engine/ECSModules               shared ECS components           Managed/       C# (CoreBridge, HotReload, Scripting.ABI, CompileServerHost)
Engine/Tests, Tests/            engine-level and integration    docs/          user documentation and development guidance
```

Modules are layered: Foundation (`Logger`, `GenerationalVector`, `Types`), core services (`AssetCore`, `JobSystem`, `Memory`, `Platform`, `FileWatcher`, `Mathematics`), content (`AssetDatabase`, `AssetRuntime`, `Audio`, `Input`, `Text`), world (`ECS`, `Physics`, `PhysicsECS`, `Rendering`, `UI`, `Scheduler`), integration (`Scripting`, `Graph`, the VCS modules). Assets are GUID-identified with pluggable parsers, a dependency graph for targeted hot reload and async streaming. `Engine` is a shared library: the Editor, the Player and `GameEngine.Native.dll` share one `Engine.dll` and its engine singleton; whoever calls `EngineCore::Initialize` first owns the lifecycle (`Engine/Source/Scripting/DllEngineBootstrap.h`).

Convention traps that invert habits from other engines, always called out when in play: left-handed, +Z forward, +Y up; reverse-Z with a D32 depth buffer, clear 0.0 and `GreaterOrEqual`; negative shadow bias; negative viewport height; descriptor indexing and image-layout transitions. The full list is in `.claude/rules/rendering.md`.

## Code style

`CodingStyle.md` is canonical for naming and layout conventions. The `change-scrutiny` skill defines the review method, finding severity, proportional cleanup, and evidence required for design exceptions. Its documented exception process applies to the preferred designs summarized here; an exception does not waive correctness, verification, or an explicit user constraint. Apply conventions to added and materially changed code, and report unrelated cleanup separately. The main design preferences are:

- Existing first: prefer the engine’s mathematics and color types, string and path utilities, containers, and job-system facilities when their contracts fit. Keep shared functionality with its owner, static UI layouts in `.uxml` with CSS, and runtime-generated UI trees in the typed builder. Boundary layouts, dedicated threads, or different semantics need a concrete rationale and appropriate lifetime, layout, or performance evidence in the Reuse and surface table. Assess actual consequences rather than treating every search match as a defect.
- Minimal API surface: private by default, with a named production consumer or a documented game/script use case. Prefer an existing API when its contract fits. A separate entry point or overload needs a meaningful difference in semantics, lifetime, or cost, not a speculative future caller. Game-facing APIs ship with user documentation and a test; judge them by what a game developer needs.
- No speculative abstraction: no interface, virtual dispatch, template or registry for a consumer that does not exist; the second concrete use tells you where the seam belongs. This is not licence to skip design: decomposition, naming and single responsibility are structure, expected for one consumer.
- Named functions over lambdas: a lambda is a short adapter handed to an API; anything longer, captured across a screen, defined then called once, or needed twice becomes a named function in the owning file.
- Prefer one primary class per file where practical. Split a file when its responsibilities warrant it, and justify exceptions by ownership, coupling, and discoverability. Keep added code free of dead code, commented-out code, vague TODOs, and magic numbers. A small fix does not require renaming or restructuring the whole function; include nearby cleanup only when it clarifies the fix or removes its cause.
- Headers: forward declarations in headers, includes in the .cpp; order own header, module headers, stdlib, third-party.
- Comments are short and describe the code as it is now: the invariant, the unit or coordinate convention, the trap and the rule, in one to three lines of Simplified Technical English (the `writing-simplified-technical-english` skill: one meaning per word, active voice, short sentences, a term of art only when it is the engine's own and there is no plain word for it). Never narrate history or a change, never say what a value used to be or why a line moved, never address reviewers, no author or date stamps; a comment that explains a change instead of the code is deleted, and a long comment is cut to its invariant. Doxygen on public APIs.

## Landing a change

Commit verified work to a feature branch early; the target default branch is strict. Use AGENTS.md as the authoritative build, test, integration, and readiness policy. Select live checks by the affected behavior: Player for Player paths, editor for editor paths, and focused render captures where output can change. A compile and unit tests alone do not establish rendering correctness. Verify build exit status and fresh binaries, and record platform or environment limitations explicitly. Required checks that cannot run remain unverified and block readiness; preserve the work and report the concrete limitation.

Review: direction fit comes before correctness: say whether the change is right for the engine at all, in the subsystem that owns the concept, in the shape the design agreed, and state that judgement separately from the findings. Security, input parsing, threading, lifetime, GPU resources and user-visible quality get the full adversarial review (`adversarial-review` skill: probes, mutants, a live check where owed); mechanical classes get the same skill at the depth the change needs. Record review verdicts and evidence in the pull request, review the final head, and coordinate platform validation and exclusive timing windows with the relevant maintainers.

## Coordination

Coordinate implementation, review, and merge work through the pull request and maintained guidance in this repository. State each task’s owner, branch, validation requirements, and timing reservations. Do not require private console access or an external workflow repository for general engine contributions.

## Working discipline

- The brief is a hypothesis: verify the premise (reproduce the defect, trace the cited paths at their current state) before implementing; if evidence contradicts it, stop and report rather than improvise.
- Comments are claims; verify against code and fix any comment your change falsifies.
- "Is X enabled" and "did we fix Y" are chains, not values: enumerate every layer with file:line, date it with `git log`, present the table before the verdict. A reversed answer means the chain was incomplete; enumerate, don't re-sample.
- Tier every claim: verified, inferred, assumed. Other agents' reports, the owner's claims and your own under challenge are all hypotheses: re-verify, never retract on weaker evidence than you asserted on, never accept an unchecked correction. Absence of evidence is not evidence of absence.
- Relays are verbatim: quote a decision and its optionality; carry the tier of a lane's finding ("lane X reports Y, unverified by me") or verify first. Ownership, provenance and version claims (whose process, whose worktree, which file version) are checked in one command before they are stated, above all when they would justify not acting; the session-start git snapshot is not a check.
- Never reason from timestamps; verify by exit code, fresh files and running the result. Missing test scaffolding is work, not an excuse. Debug to root cause: reproduce, capture the failing state before rebuilding, verify the instrument, attribute before fixing, rule out the environment, change one variable at a time, prove the fix before/after under the same measurement.
- A fold brief lists its applicable checks and rationale from AGENTS.md before the lane starts. Review and checks use the same final head. Integrate and validate the freshly fetched target base according to AGENTS.md; do not introduce a separate merge schedule or rebuild exception here. Repeat checks only when new changes, failures, or unresolved concerns justify them.
- Finish the whole task. A turn that ends with a plan, a question the request already answers, or "the build will wake me" leaves the work undone: wait in session for builds and suites, do the next step, and stop only when blocked on input only the owner can give or before a destructive action. Keep a checklist (`STATUS.md` in the evidence directory) that survives compaction.
- A script other processes may be running is never edited in place: write the new file and move it over the old one. Evidence another team needs is attached to the pull request, never cited only by a path on one machine; restricted console evidence stays in the private console repository.

## Reporting

End every long run under three headings: **Blocked on me** (decisions or input only the owner can give), **Changed** (files, commits, pushes), **Found** (defects, gaps or risks that were not the point of the task). Omit an empty heading. When compacting, preserve verbatim: the owner's decisions in their wording, live lane ids and worktree paths, branch heads and SHAs, process ids of anything still running, and every open item.

## Working with the owner

The owner is a senior graphics engineer (Unity URP/HDRP and SRP internals, UE5 engine-side rendering, and extensive hands-on work in this engine's Vulkan renderer, terrain and ECS). Skip the basics, lead with conclusions and evidence, be direct about trade-offs and about verified versus assumed. A one-sentence Unity or UE cross-reference is the fastest bridge for a genuinely new concept; anchor, don't lecture. Ask only for visual or quality judgement, product direction, or an editor-level GPU run; everything else is decided and split autonomously.

## Documentation

`docs/` is for users of the engine (usage, authored formats, concepts). Maintained designs, decisions, investigations, plans, and workflow guidance live under `docs/development/` in this repository. Generated measurements and per-change review records go in the pull request or its attachments. Restricted console documentation stays in the private console repository. The format and the one-canonical-file rule load from `.claude/rules/documentation.md` when you touch `docs/`.
