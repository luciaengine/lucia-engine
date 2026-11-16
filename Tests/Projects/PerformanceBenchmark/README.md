# Performance benchmark project

The fixed scene set of the engine's performance benchmark: one scene for each kind of game the engine serves. `Tools/Benchmarks/Scenes/generate_scenes.py` writes every file here. Do not edit them by hand: change the generator, run it and commit its output.

- `benchmark.json`: the resolution, the frame counts, and each scene's camera poses, environment and required passes.
- `Assets/Scenes/`: `Forest`, `OpenWorld`, `Crowd`, `City`, `Interior`, `Sprites2D`.
- `Assets/Materials/`, `Assets/UI/`: the materials and the 2D scene's UI.

`Tools/Benchmarks/Scenes/benchmark.py run` copies this project to a work folder and adds the content that is not committed: the city's cluster meshes, the crowd's model and the forest's render pipeline. `docs/development/performance-benchmark.mdx` says how to run and compare.

All content is procedural or generated. The crowd's `Fox.glb` comes from `Tests/EditorHarness/rg2-gate1-content/Assets/Models/Fox/`, with its license and provenance there.
