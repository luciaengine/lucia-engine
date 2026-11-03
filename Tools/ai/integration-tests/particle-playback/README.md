# Cozy VFX conversion tools (draft)

These optional, manually invoked tools require a locally licensed Cozy pack.
They are separate from the particle runtime and are not run by the build or editor.
Generated scenes use the particle features in [PR #1513](https://github.com/Lunarsong/GameEngine/pull/1513);
use that branch to evaluate them until those features land on main.
No third-party pack assets are included. The full licensed-pack conversion still
needs validation; the standalone OBJ tests do not establish full conversion parity.

## Particle playback preview

This generates a small native scene for checking continuous fire and leaf landing
without committing third-party textures. Supply a local, licensed copy of
**Cozy Stylized Environment VFX** containing `FXt_Fire_00.png`, `FXt_Fire_01.png`,
`FXt_Fire_02.png`, and `FXt_Leaves.png`.

```sh
python3 Tools/ai/integration-tests/particle-playback/generate_preview.py \
  --source-assets /path/to/CozyStylizedEnvironmentVFX \
  --output build/particle-playback-preview
```

Open `Assets/Scenes/ParticlePlayback.scene` in that generated project with the
current Editor. Rerunning the generator replaces its generated scene/materials.
It never modifies the source pack.

For Scene View, open the camera settings, disable Auto Exposure, and set Fixed
EV100 to **7**. Scene View keeps its own saved exposure; the generated scene
camera also uses EV7. Frame both effects around `(0, 1, 0)` from 12 metres away.

- **Continuous Flame** uses the complete animation (16 frames for Fire00/02, 20 for Fire01) over each particle's
  lifetime. The 0.25-second overlap between generations crossfades their alpha;
  it is an intentional smoothing adjustment, not a byte-for-byte prefab port.
- **Falling / Landed** maps `FX_env_Leaves_03`: a tilted emission rectangle at
  height 5, speed 1 along its normal, downward acceleration of 0.5–0.75, randomized
  starting rotation, horizontal two-octave noise and 45°/s rotational flutter.
  Collision spawns a resting leaf with inherited size/color/rotation; it fades
  near the end of its five-second lifetime. Noise uses Perlin curl with the source quality, damping and octave controls.
  Native seeds and gradient tables can produce different individual trajectories.

Both use the source pack's grayscale-derived alpha and tint-dependent
additive/alpha blend equation. Fixed seeds make runs reproducible. The runtime
suite separately checks flipbook frame coverage, handoff brightness, gradient
channel correlation, and inherited appearance.

## Refresh an existing Unity gallery

`sync_unity_gallery.py` reads the licensed Unity prefabs, resolves variants, and
updates noise, random-gradient inputs, and collision sub-emitter triggers in an
existing native gallery. It requires PyYAML. Each changed scene receives a
`.scene.before-unity-sync` backup, retained on repeated runs.

```sh
python3 sync_unity_gallery.py --source-assets /path/to/CozyStylizedEnvironmentVFX \
  --scenes /path/to/Assets/cozy/Scenes --report /tmp/cozy-sync-report.json
```

The JSON report lists matched scenes, noise settings, unmatched emitters, and
remaining implementation differences. Perlin curl, frequency damping, quality and octave settings are supported.
Unity's internal seeds and gradient tables are not replicated. The script preserves existing converted
settings outside the supported changes; a successful mapping is not a claim of
pixel-identical rendering or a complete audit of every Unity module.

### Wind, beam, and butterfly rendering

The gallery sync also imports the wind trail width range, particle color/size inheritance,
point spacing, tiled UV scale, particle-lifetime fade, trail-length fade, and death behavior.
The two fade controls are triangular envelopes, matching this pack's three-key alpha curves;
they are not a general Unity gradient importer. Wind uses the source edge's +Y launch axis
rotated into +Z, and disables the particle head because Unity uses trails-only rendering.

Butterfly materials enable masked mesh wing deformation with the source speed/range and
mask texture, plus the source two-color wing tint. Particle vertex modifiers can read
material properties and textures. Velocity-facing meshes use simulated displacement so
noise-driven butterflies turn along their flight path. Sunbeam billboard geometry keeps
the renderer pivot and converts the flipped UVs to the native image convention. A Unity 6
`ParticleSystemRenderer.BakeMesh` reference confirms Y=-1..0 and Unity V=0 at Y=0 for
pivot Y=-0.5 with flip Y=1; the native mesh inverts V to keep the bright end at the emitter. Converted assets are backed up
before these renderer corrections; source textures remain external and unchanged.
Reruns leave an already converted mesh unchanged. If a mesh differs from both its
original backup and the expected conversion, sync stops with an error and preserves
both files, so later geometry or UV edits are never silently replaced.

The butterfly vertex stream also supplies ParticleIndex to the flap phase. The native
shader uses per-particle indices and applies the source graph displacement after particle
scaling, along the emitter up axis. This preserves the full flap amplitude for small insects.
Perlin curl uses world-coordinate derivatives and unnormalized lattice gradients. A Unity
500-particle reference at frequencies 0.1, 0.5, and 1 confirms that damping cancels the base
frequency in motion amplitude; omitting the derivative's frequency factor over-amplified snow.

The gallery sync maps constant Unity collision damping to `collisionDamping`.
Full damping with zero bounce holds landed particles in place while lifetime and
visual processors continue. Phase entry releases the contact state. Birth subemitters
with a constant distance emission rate become `distance` events on the parent,
with world-space spacing and interpolated spawn positions along each simulation step.

The gallery sync triangulates its convex OBJ faces offline, including all four
corners of the butterfly's wing and body quads. This asset preparation does not
require changing the engine's OBJ importer. The sync flips the butterfly OBJ's V
coordinates once for native texture sampling; procedural wing color uses Unity's
original UV convention.

Run the standalone conversion tests (no source pack or PyYAML required):

```sh
python3 Tools/ai/integration-tests/particle-playback/test_sync_unity_gallery.py
```
