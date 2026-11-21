# MeshClusterCook

Cook a static triangle model with opaque or alpha-cutout materials into a self-contained coarse
`.gecluster` model plus immutable fine pages. Run the staged tool without an editor:

```text
MeshClusterCook --input Models/rock.glb --output Models/rock.gecluster
```

The cook builds a cluster DAG for each source mesh, as meshoptimizer's
`clusterlod.h` example does: it splits the mesh into clusters, groups
neighboring clusters, locks the borders that groups share, simplifies each group
to about half, splits the result into clusters again, and repeats until one
cluster is left or no group reduces. A group's error is at least the error of
every group under it, and its sphere encloses theirs, so the projected error
never decreases toward the root. A group that does not reduce stays as it is
and draws at every coarser level.

The coarse model has one mesh for each source mesh. Its LOD0 is the first DAG
level with at most a quarter of the source triangles, unless that level's error
is over 2 % of the mesh extent: then LOD0 is the coarsest level under that
error. A mesh that simplifies badly (sharp, boxy shapes) keeps its source as
LOD0 and draws as an ordinary model with a generated chain. The levels under it are
fine pages: one page for the clusters of one group that one finer group made.
The cook stops the DAG at LOD0. Farther away, the coarse mesh gets an ordinary
generated LOD chain: a whole-mesh simplification needs fewer triangles than the
coarser DAG levels, whose group borders stay locked. Each generated level adds
the error of LOD0 to its own, so it switches at the same screen error as an
ordinary model's chain.

The report includes the DAG groups and units, the LOD0 triangles and the
streamed triangles, so a successful cook can be checked for useful reduction.

Static node placements are preserved, including multiple nodes that share a
mesh. The output contains geometry and materials; animation, skinning and morph
targets are refused. Non-manifold sources cook: an edge of more than two
triangles, a duplicate face, inconsistent winding and a non-manifold vertex are
locked, so every level keeps them.

Alpha-cutout materials keep their cutoff, sidedness, and texture references.
The cook uses UV-aware reductions for those meshes. Vertex streams used to
store leaf pivots require the regular mesh path.

Embedded textures are copied into the coarse model. External textures require
their published asset identities:

```text
MeshClusterCook --input Assets/Models/rock.gltf --output Assets/Models/rock.gecluster --assets Assets
```

`--assets` reads `Assets/.assetmanifest` as an immutable mount. Publish the
project/package asset manifest first. The tool does not scan or rewrite identities. The output must be a new file in an existing
directory. Unsupported topology, animation, vertex streams, materials or resource
limits produce a refusal.

Fine pages are compressed without loss: each page holds its vertices and its
indices in the meshoptimizer codecs, about 30 % of the raw size for a dense
model. The streaming worker decodes and validates a page in about 0.1 ms. A page that
does not decode is refused, and its coarse range keeps drawing.

Keep the generated model and its sibling `.cluster` directory together when
moving authoring data. Package cooking gathers the fine store under the packaged
asset root and retains texture dependency edges. Missing fine pages still permit
the complete coarse model to load. A failed cook can leave an unused immutable
fine store; it does not replace an existing coarse model through this CLI.

The generator's error is an estimate in mesh-local units, not a certified pixel
error or a shading guarantee. This tool alone does not enable runtime fine-page
selection or establish a memory/performance improvement.


## Runtime fine pages

Set `GE_CLUSTER_LOD=1` before starting the Editor or Player. Fine pages run on
Vulkan and Metal devices with buffer device addresses; on any other device fine
mode stays disabled. Fine pages work with async compute on.
Ray tracing reads the coarse model: scene acceleration structures skip fine pages.
Import the generated `.gecluster` model normally: one entity draws each coarse
mesh and its LOD chain. When a view wants a group under LOD0 to refine, that
instance splits: runtime-only children draw the units of its cut, and the
instance's own mesh stops drawing. The cut comes from the top of the DAG down: a
unit draws when its group's projected error is over the pixel budget and the
error of the group that made it is not. A group refines only when every unit it
needs can draw, and coarsens only when the units it made can draw, so a pending
or failed read keeps the coarser units. Each cut is crack-free. The split ends
when no view wants a streamed unit for 30 frames. A change to the instance's
renderer ends the split, and the next frame splits it again from the new state.
Fine pages use the existing asset IO service.

The prototype supports one cluster world, static opaque standard-PBR materials,
and ordinary raster views. It checks every active view and freezes selection
before extraction. Non-cluster preview worlds can share the renderer. Enabling
GI, reflection capture or a second cluster world returns to coarse mode
until restart. HLOD runs beside cluster LOD: a member that HLOD replaces with its
cluster proxy ends its split, and its fine pages retire. Destroying or clearing the cluster world also selects coarse mode
until restart. Skinning, morphing, custom vertex/surface programs, transmission,
blend materials and sector-tagged instances use coarse geometry. CPU picking
uses the coarse model. Re-cooking a different cluster incarnation requires a
restart; live replacement of its fine directory is refused.

### Triangle budget

A game can cap the triangles that all cluster instances of the world draw:

```cpp
renderServices.EnsureFeature<GameEngine::Engine::Renderer::ClusterLODFeature>().SetTriangleBudget(2'000'000);
```

Set `GE_CLUSTER_LOD_TRIANGLE_BUDGET=<triangles>` to set the same cap at startup without code.
The cap is off by default (0). The pixel error budget stays the floor: a group refines only
when its projected error is above that budget. Under the cap, the groups of every split
instance refine from the root down in order of largest projected error, and a group that does
not fit stays coarse. Unsplit instances count their LOD0 triangles first. Forced LOD 0 and LOD
selection off ignore the cap. Every cut is crack-free, because the cook locks every border
that groups share.

Fine storage owns at most 32 MiB of GPU buffer capacity, including unused and
retiring ranges. At most eight reads remain in flight; each preparation admits at most eight fine pages.
The 64 MiB page-work reservation charges decoded/read/upload arrays until the GPU entry draws; it is not
a process-RSS limit. These bounds exclude the pinned coarse model, driver heaps
allocator overhead and device-deferred native frees. Monitors expose `ClusterLOD.TrackedPages`, `ClusterLOD.ResidentPages`,
`ClusterLOD.SplitInstances`, `ClusterLOD.PartChanges` (parts that started or stopped drawing in the last frame; a stable cut shows 0), `ClusterLOD.PendingReads`, `ClusterLOD.ChargedPageWorkBytes` and
`ClusterLOD.FineOwnedBufferBytes`. `ClusterLOD.DecodedElementBytes` counts
live decoded vertex/index elements through their final shared owner, excluding
container capacity and metadata. These are residency diagnostics, not a claim
of a measured speedup or peak VRAM reduction.

Fine handles and their buffers belong to the sealed extraction path. Managed
fine buffers cannot be submitted as raw contributor geometry. Retiring entries
close registry draw lookup before waiting for device-owned graphics completion;
expired graph recordings cannot be replayed. Direct device-address access,
manual GPUScene writes and lower-level command recording bypass this contract
and are unsupported with fine mode.
