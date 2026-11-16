# MeshClusterCook

Cook a supported static, opaque triangle model into a self-contained coarse
`.gecluster` model plus immutable fine pages. Run the staged tool without an editor:

```text
MeshClusterCook --input Models/rock.glb --output Models/rock.gecluster
```

The coarse model has one mesh for each source mesh. Each cluster group is a
contiguous index range of its coarse mesh, and each group has one fine page that
replaces that range. The coarse mesh gets an ordinary generated LOD chain for
distance, so far away one instance is one draw of a few triangles. Each generated
level adds the coarse error to its own, so it switches at the same screen error
as an ordinary model's chain.

The report includes vertex and triangle totals, the number of reduced coarse
groups, and the estimated maximum coarse error in model units, so a successful
cook can be checked for useful reduction.

Static node placements are preserved, including multiple nodes that share a
mesh. The output contains geometry and materials; animation, skinning and morph
targets are refused.

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
mesh. When a view wants a group's fine page, that instance splits: it gets one
runtime-only child for each group, which draws the group's coarse range or its
fine page, and the instance's own coarse mesh stops drawing. The split ends when
no view wants a fine page for 30 frames. A change to the instance's renderer ends
the split, and the next frame splits it again from the new state. Fine pages use
the existing asset IO service; pending or failed reads keep the coarse range.

The prototype supports one cluster world, static opaque standard-PBR materials,
and ordinary raster views. It checks every active view and freezes selection
before extraction. Non-cluster preview worlds can share the renderer. Enabling
GI, reflection capture or a second cluster world returns to coarse mode
until restart. HLOD runs beside cluster LOD: a member that HLOD replaces with its
cluster proxy keeps its coarse cover, and its fine page retires. Destroying or clearing the cluster world also selects coarse mode
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
when its projected error is above that budget. Under the cap, groups refine in order of
largest projected error first, until the next group does not fit. Every instance counts its
coarse triangles first, so a cap below the coarse total refines nothing. Forced LOD 0 and LOD
selection off ignore the cap. Any mix of coarse and fine groups is crack-free, because the
cook locks every border that groups share.

Fine storage owns at most 32 MiB of GPU buffer capacity, including unused and
retiring ranges. At most four reads remain in flight; each preparation admits at most two fine pages.
The 64 MiB page-work reservation charges decoded/read/upload arrays; it is not
a process-RSS limit. These bounds exclude the pinned coarse model, driver heaps
allocator overhead and device-deferred native frees. Monitors expose `ClusterLOD.TrackedPages`, `ClusterLOD.ResidentPages`,
`ClusterLOD.SplitInstances`, `ClusterLOD.PendingReads`, `ClusterLOD.ChargedPageWorkBytes` and
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
