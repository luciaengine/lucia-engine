---
name: change-scrutiny
description: The pass every change runs on itself before its pull request opens — reuse (the engine's types, containers, utilities, controls, the job system), placement (the owner file and module), names, the public surface (whether a symbol should exist, be public, or be an existing API changed instead), the style pass (the whole of CodingStyle.md read against the diff), and the UI/UX pass (whether a user understands and can operate every surface the change adds or alters). Produces the "Reuse and surface" table the pull request body carries. Use after the code compiles and the suites are green, before `gh pr create`; use again at every fold that adds a symbol, a file or a user-facing surface.
---

# Change scrutiny

A change is reviewed twice: once by its author with this pass, once by the reviewer with the review rules. The author's pass is the cheaper one, so it runs first and its table goes into the pull request body under "Reuse and surface". The rules it enforces are `CodingStyle.md` (all of it: the Mathematics Types, Existing First and Public Surface sections carry the reuse and surface questions, section 5 below walks the rest) and `CLAUDE.md`'s engineering values, with section 6 asking whether a user understands what the change shows them; this skill is the method, not a second rule set.

## Scope, judgment, and finding severity

Review the requested behavior and the architecture needed to support it. Keep cleanup proportional to the change. A small correctness fix does not require renaming or restructuring the whole function it touches. Fix conventions in added or materially changed code; include nearby cleanup only when it makes the fix clear or removes a cause of the defect. Record unrelated improvements as follow-ups. A larger necessary refactor needs an explicit explanation of why the requested behavior cannot work without it.

Use the reuse, placement, API, and concurrency rules below as preferred designs. Evaluate ownership, semantics, lifetime, dependencies, and measured cost before applying them. A justified exception names the alternative considered, the constraint that prevents using it, and the resulting tradeoff in the table. An exception does not waive a correctness requirement, a verification gate, or an explicit user constraint. Cite any applicable repository rule rather than silently treating it as optional.

Classify each finding by its consequence:

| Class | Evidence required | Disposition |
|---|---|---|
| Correctness blocker | A concrete failing scenario, violated invariant, or missing required verification, with the affected path and supporting evidence | Resolve before readiness; state unverified behavior explicitly |
| Maintainability finding | A specific ownership, duplication, API-contract, or maintenance problem and a scoped remedy | Fix material problems in scope; explain a justified exception or record a separate follow-up |
| Style suggestion | A readability or consistency improvement with no demonstrated behavioral or architectural harm | Keep local and proportional; a preference alone does not block readiness |

An explicit mandatory repository convention remains a requirement even when its finding is classified as style. Name that rule and any justified exception separately from the finding's impact. Do not present a stylistic preference as a correctness defect. Order findings by consequence, not by checklist order. Each finding names the location, problem, evidence, and smallest useful remedy. A clean checklist is not proof of correctness.

## 1. Inventory the change

Use the freshly fetched intended target base for the three-dot diff. The commands below use `origin/main` only as an example, not as an assumed target. From the diff and its stat, list:

- every file added, with its owner (the module and the class it holds);
- every type, function, constant and setting added or made public (a symbol in a public include folder, a `public:` member, a `*ScriptApi.h` declaration, a setting registered, a component field reflected);
- every piece of behavior written from scratch (a loop over strings, a container, a math expression, a thread, a drawing routine, a UI assembly, a hash);
- every surface a user sees or operates that the change adds or alters, in the editor or the Player: a panel, an inspector section, a setting, a dialog, a menu, a notice, a control, a line of user-facing text.

## 2. Reuse: for each piece written from scratch, find the engine's piece

Search before concluding it does not exist; the search is the evidence, named in the table.

| What the change does | Where the engine already does it |
|---|---|
| a position, direction, scale, color, quaternion, matrix, bounding box | `Engine/Modules/Mathematics` (`Vector2/3/4`, `Quaternion`, `Matrix4x4`, `Geometry.h`), `Types/Color.h` (`Color`, `ColorLinear`) |
| lerp, clamp to 0..1, smoothstep, remap, easing, noise | `Mathematics/Interpolation.h` (`Lerp`, `Clamp01`, `SmoothStep`), `Mathematics`, `Noise` |
| split, trim, join, case fold, prefix tests, number and byte formatting, parsing, path joining | `Types/StringUtils.h`, `Types/FormatNumber.h`, `Types/ParseNumber.h`, `Types/PathUtils.h`, `Types/NearestName.h` |
| a map, a set, a handle table, a view over a run, a hash | `Types/FlatMap.h` (and `FlatSet` when it lands), `Foundation/GenerationalVector`, `std::span`, `Types/Fnv1a.h` |
| parallel or deferred work, a worker, a poll, a future | `Engine/Modules/JobSystem` (a job, a batch, a continuation; a pool build like `HeightFieldShapeBuilder`) |
| a log line | `Logger` |
| a pool or arena, a frame allocator | `Engine/Modules/Memory` |
| a render pass, a GPU resource, a barrier | the render graph (`Engine/Source/Engine/Rendering` nodes; the pass declares its resources) |
| a drawn handle, a gizmo, a line in the Scene View | `Apps/Editor/Include/SceneView/SceneViewGizmos.h` |
| an inspector row, a section, a property editor | `Apps/Editor/Include/UI/InspectorSection.h`, the inspector registry |
| a control | `Engine/Modules/UI/Include/UI/Controls`, the module's own `UI/Controls` folder |
| a settings page, a post effect, a dockable, a search item | the registries (`Editor::EditorSettingsRegistry` and the others) |

The search: list `Engine/Modules/`, then use `rg "<verb or noun>" Engine/Modules/<Candidate>/Include` for the function, and the test map for the suite that covers it. Prefer a suitable existing piece. Extend it when the new behavior belongs to its contract; do not broaden a shared API or add a dependency solely to avoid a small local implementation with different semantics. Put shared functionality with its owning module and caller-specific behavior with its caller. Explain deliberate separation or duplication in the table, including the alternative and its concrete cost.

## 3. Placement and names

- Prefer one class per file, named for the class, and shared free functions beside their kin in the owning module's utility header. File-local helpers can stay in the implementation file. Judge a split by responsibility, coupling, and discoverability; record a justified exception rather than creating files solely to satisfy a count. Keep feature implementation out of hubs (`EditorApplication`, `EditorContext`, `SettingsPanel`, `InspectorPanel`, `HierarchyPanel`, `SceneViewPanel`, `RenderServices`, `UIManager`, `UIElement`, `core.css`).
- The file lives with its owner: the module that owns the concept, under the folder its siblings use (`UI/Controls`, `UI/Inspectors`, `Systems`, `Shaders/<Area>`), and inside the feature's own folder when the feature has more than one file (`Panels/ProjectPicker/`, not loose in `Panels/`); a change that adds a second file to a feature creates the folder and moves the first file with it. The table names the folder of every added file.
- Names say what the thing does for the caller, in full words, PascalCase for types and functions, `m_PascalCase` for private members, `kPascalCase` for constants; a name that needs a comment to be understood is the wrong name.

## 4. The public surface, one question set per symbol

Ask these of every symbol the inventory marked public, and write the answer in the table:

1. **Who needs it, outside the module, today?** Name the caller in this change. No caller: the symbol is private, or deleted. "A future caller" is no caller.
2. **Does an existing API already carry this?** Prefer extending it when ownership and semantics match. A distinct synchronous, asynchronous, or overloaded entry point needs a real caller and a clear difference in contract, lifetime, or cost. Do not force different contracts into one API to reduce the symbol count.
3. **Is a new symbol the right end state, or does a refactor remove the need?** Consider whether a flag, special case, or pass-through exposes an ownership problem. Prefer a scoped refactor when it simplifies the requested behavior; explain larger necessary changes and keep unrelated redesign separate.
4. **Is it a game-facing API?** Then it ships with its user documentation and a test, judged by what a game developer needs, not by who calls it inside the engine.
5. **Is the name the caller's word for it?** The verb for a function, the noun for a type; the engine's own terms where it has one.
6. **What is its narrowest reach?** Private by default; a `.cpp`-local or an internal header before a public include; a public include only for the consumer named in question 1.

A public symbol is a promise the engine keeps for every future caller; the smallest promise that serves the named caller is the right one.

## 5. The style pass: the whole of `CodingStyle.md` against the diff

Read the diff against every section of the style guide, not from memory. The ones that bite:

- **Names.** Functions and types PascalCase; private members `m_PascalCase`; public data members of a plain aggregate PascalCase; locals and parameters camelCase; file-scope statics `s_PascalCase`; constants `kPascalCase` at every scope, a named constant inside a function included. Full words: no `idx`, `tmp`, `cnt`, `mgr`, `ptr`. A boolean reads as a condition (`IsVisible`, `HasParent`). The verb matches the cost and the contract: `Get` returns what is there without side effects, `Find` may fail, `Compute` and `Build` cost something, `Try` returns whether it did.
- **Files and headers.** One class per file, the file named for it; forward declarations in the header, includes in the `.cpp`; include order own header, module headers, standard library, third-party; quotes for project headers, angle brackets for external ones; `public`, `protected`, `private` in that order; no `using namespace` and no global-scope symbol in a header; everything inside the project's namespaces.
- **Lambdas.** A short adapter handed to an API. One that grows past a handful of lines, captures `[&]` across unrelated state, is assigned to a local and called once, or is needed twice becomes a named function in the owning file; no function built from nested lambdas.
- **Enums, literals and types.** `enum class`, with an explicit underlying type where layout matters; fixed-width integers where layout matters; `std::chrono` for time, `std::filesystem` for paths; `nullptr`; no magic number: a `k` constant whose name or comment states the unit.
- **Control flow and errors.** Early returns over nesting; a hot path straight-line; no exceptions on a hot path, validation and early returns instead; the project's assert macro, never with a side effect inside it.
- **Concurrency.** State thread affinity and ownership on a public API. Prefer the job system (`Query::Parallel`, `JobCounter`) for work it can schedule safely. A dedicated thread needs a concrete reason, such as an externally imposed affinity or a blocking lifetime unsuitable for workers, plus shutdown, synchronization, and lifetime evidence. Prefer immutability, then a named synchronization primitive.
- **Templates and copies.** A public template constrained by a concept or a `requires` clause; a large template body in an `.inl`; move operations for a heavy type, copy semantics deleted or defined on purpose.
- **Logging and comments.** Nothing verbose on a hot path, warnings and errors only, no enter and exit lines; Doxygen on public APIs; a comment states the invariant, the unit, the convention or the trap as it is now, never history, never what a value used to be, never a word to a reviewer, no author or date stamp; no commented-out code, no vague TODO. Every comment the diff adds or touches is one to three lines of Simplified Technical English (the `writing-simplified-technical-english` skill) and says what the code is, not what changed: a comment that narrates a change, a former value, a removed path or the review that asked for it is deleted; a comment over three lines is cut to its invariant.
- **Tests.** A test name states the behavior; the file sized like its neighbors; scratch checks not committed.
- **Cleanup in scope.** Apply conventions to added and materially changed code. Leave unrelated names and structure in an existing function alone when changing them would obscure the requested fix. Record worthwhile wider cleanup separately; touching a function does not by itself justify migrating all of it.

Before writing the style summary, run these searches over existing touched source files and inspect each match in context. The searches generate candidates, not findings. Distinguish added or materially changed code from pre-existing code and record relevant matches and their disposition. Guard an empty file list, omit deleted paths, and use a path-safe equivalent when filenames contain spaces. These examples assume ordinary paths and a verified `origin/main`; substitute the freshly fetched target base when it differs.

```
git diff --name-only origin/main...HEAD -- '*.cpp' '*.h' '*.py' '*.cs' > /tmp/touched.txt
grep -n -E '\b(ln|rel|m|n|rx|idx|tmp|cnt|mgr|ptr|buf|str|val|res|ctx|obj|elem|i|j|k)\b\s*[=;,)]' $(cat /tmp/touched.txt)     # short names (locals in a loop body of three lines are the only allowed exception: say which)
grep -n -E '\bfloat\s+\w+\[(3|4|9|16)\]|std::array<float,\s*(3|4|9|16)>' $(cat /tmp/touched.txt)              # raw arrays where Vector3, Vector4, Matrix4x4, ColorLinear fit
grep -n -E '^#include' $(cat /tmp/touched.txt)                                                                     # the order: own header, module headers, standard library, third-party
grep -n -E '\b(std::thread|std::async|std::jthread|CreateThread|_beginthread)\b' $(cat /tmp/touched.txt)            # a thread of the change's own
grep -n -E '\[&\]' $(cat /tmp/touched.txt)                                                                        # a lambda capturing everything: a named function instead
grep -n -E '\bTODO\b|\bFIXME\b' $(cat /tmp/touched.txt)                                                          # vague TODOs
grep -n -i -E "^\s*(//|\*|#).*\b($(paste -sd'|' Tools/CleanupGate/history_words.txt))\b" $(cat /tmp/touched.txt)   # change narration in comments: the word list of the cleanup gate's history_comment
```

Resolve confirmed findings according to their severity and applicable repository requirements. A short name, raw array, or lambda capture is not a defect solely because it matches a search. Check meaning, layout constraints, capture lifetime, and readability. Report justified exceptions, pre-existing out-of-scope matches, and optional suggestions accurately. The "Reuse and surface" section ends with a summary such as `Style pass: names, raw arrays, include order, threads, lambdas, comments checked over N files; required fixes resolved; exceptions and follow-ups listed`. An inaccurate summary is corrected with evidence and assessed by its consequence, rather than automatically outranking a behavioral defect.

## 6. The UI/UX pass: every surface a user sees or operates

Run it on every change whose inventory lists a user-facing surface. Answer each line for each surface, in writing, against the change as built; a line answered from memory of the design is not answered. Open the surface in an isolated editor or Player, or read its capture, while you answer.

- **Understood at first sight.** A user with no explanation knows what the control does and what each option means. The label is a name a user would say, not an engine-internal noun (not "observer" where the meaning is "seen by the eye or a camera"). The unit and the range show where they matter.
- **The control fits the choice.** A choice among presets is a dropdown, with a thumbnail per preset when they look different; a mode with two values is a toggle or two radio choices; a number with a useful range is a slider with a number box; a list of many is searchable. Four buttons are not a preset picker.
- **Defaults that work.** The default is what most users want on day one. A dial whose non-default values are all worse is deleted. A setting nobody would reach for in a week does not ship.
- **Does what a user expects.** Copy gives feedback. Right-click on an item opens a menu with the actions a user expects there. Double-click does the obvious thing, and the tooltip says what it does. Escape cancels. A long item has a detail view. Undo covers every edit.
- **Consistent with the editor.** The same words, icons, row heights, fonts and spacing as the neighboring panels: the editor's text styles and palette, no new font or color for one panel. A new control is a class in the UI module (or the owning module's UI folder) with a `.uxml` layout and CSS, never a one-off; the typed C++ builder `UI::Element<T>` builds only trees made at run time from data (a tree that is the same every time is a `.uxml` file).
- **Shown, not described.** A capture of each changed surface at rest and in use (the menu open, the notice visible, the long item selected), with the pose or state on the record line, taken against the editor UI checklist in `GameEngine-Workbench/agent/evidence-checklists.md`. The visual critics judge the captures for intuitiveness and look, not only for pixels. A surface the owner asked for is shown to her before it merges.
- **Text.** American spelling, plain words, no other engine's names, and the thing as it is now.

Classify UI/UX findings by their effect on the user's task. Resolve broken interactions and required verification before readiness; distinguish those from optional visual refinements. The "Reuse and surface" section ends with a second line after the style summary: `UI/UX pass: first sight, control fit, defaults, expected behavior, consistency, captures, text checked over N surfaces; required fixes resolved; exceptions and follow-ups listed`, or the outstanding requirements and their reason. A change with no user-facing surface writes `UI/UX pass: no user-facing surface`. A reviewer checks the same evidence and assesses inaccurate claims by their consequence.

## 7. Duplicates and the gate

- Investigate duplicate implementations for matching contracts and ownership. Reuse or extend a suitable engine piece. If separation is justified, record the constraint and tradeoff in the table. Report unrelated duplication found nearby as a follow-up rather than expanding this change automatically.
- Where the cleanup gate test exists (`CleanupGateTests`, `Tools/CleanupGate/`), run it; a count above the baseline is this change's to fix before the pull request opens.

## 8. The table in the pull request body

```
## Reuse and surface
| Added | Kind | Engine piece used or why not | Public? Caller outside the module | Verdict |
|---|---|---|---|---|
| Foo::Bar() | function | Types/StringUtils.h TrimWhitespace | no (module-internal) | kept |
| BazCurve | type | Mathematics/Interpolation.h lacked Wrap: extended there (commit …) | yes: the Day Cycle window | kept, documented, tested |
| float heights[16] | field | boundary: the GPU upload struct pins its layout (comment names it) | no | kept, allowlisted |
| DeviceEventWorker | dedicated thread | JobSystem considered; device API requires a blocking thread with fixed affinity; shutdown and lifetime verified | no (module-internal) | justified exception: reserves one thread |
```

A change with nothing public and nothing written from scratch says so in one line, followed by the style pass line and the UI/UX pass line. Keep rows focused on decisions a reviewer needs to assess; group symbols that share the same owner, caller, and rationale. List unresolved findings with their class, evidence, and disposition. A reviewer checks this table against the diff and reports inaccurate claims at the severity their consequences warrant.
