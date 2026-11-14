import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { ipc, sleep } from '../lib/ipc.mjs';

assert(process.env.GE_EDITOR_DEBUG_PORT && process.env.GE_EDITOR_DEBUG_PORT !== '9999',
  'Use a dedicated test editor port');
const projectArg = process.argv.indexOf('--project');
assert(projectArg >= 0, 'Provide --project /absolute/path/to/a/test-copy');
const project = fs.realpathSync(process.argv[projectArg + 1]);
const preview = process.argv.includes('--preview');
assert(project !== fs.realpathSync(path.dirname(fileURLToPath(import.meta.url))), 'Use a copy of this fixture');
async function call(method, params = {}) {
  const result = await ipc(method, params);
  assert(!result?.error, `${method}: ${JSON.stringify(result)}`);
  return result;
}
const state = await call('get_editor_state');
assert(!state.sceneDegraded && !state.sceneLoadError, 'Fixture scene did not load cleanly');
assert(state.scenePath && fs.realpathSync(state.scenePath).startsWith(project + path.sep),
  'Open the copied fixture in the dedicated editor first');
const nodes = [];
function walk(node) { nodes.push(node); for (const child of node.children ?? []) walk(child); }
for (const node of (await call('get_scene_hierarchy')).entities) walk(node);
function id(name) { const n = nodes.find(n => n.name === name); assert(n, name); return n.id; }
const request = {
  receivers: [id('Bake Floor')], occluders: [id('Bake Blocker'), id('Bake Emitter')],
  lightEntity: id('Bake Sun'), resolution: preview ? 64 : 1024, giSamples: preview ? 64 : 256, bounces: 2,
  denoise: !preview,
  environmentRadiance: [0.02, 0.02, 0.02], shadowMode: 'Only', apply: true,
};
const started = await call('bake_lightmaps', request);
assert.equal(started.status, 'baking');
let result;
const deadline = Date.now() + 600000;
do {
  await sleep(250);
  result = await call('get_lightmap_bake');
} while (result.status === 'baking' && Date.now() < deadline);
assert.equal(result.status, 'complete', JSON.stringify(result));
assert.equal(result.applied, true, JSON.stringify(result));
assert(['cpu', 'gpu_ray_query', 'gpu_and_cpu'].includes(result.acceleration));
assert(result.tracedRays > 0 && result.gpuTracedRays >= 0 && result.gpuTracedRays <= result.tracedRays);
if (process.argv.includes('--require-gpu')) {
  assert.equal(result.acceleration, 'gpu_ray_query', 'GPU acceleration was required for this run');
  assert.equal(result.gpuTracedRays, result.tracedRays);
}
assert.equal(result.receivers.length, 1);
const row = result.receivers[0];
for (const key of ['shadow', 'indirect', 'material']) assert(fs.existsSync(row[key]), key);
const material = JSON.parse(fs.readFileSync(row.material));
assert.equal(material.surfaceShader, 'Surfaces/shadow_lightmap_pbr.glsl');
assert.equal(material.properties.userVec0[3], 1);
assert(material.textures.indirectLightmapMap.guid);
assert(material.textures.shadowLightmapMap.guid);
await call('focus_view', { view: 'game' });
await sleep(3000);
const settleDeadline = Date.now() + 120000;
// View-specific shader variants are requested after extraction starts. Allow
// the cold compile wave to settle as well as waiting for texture decodes.
const earliestCapture = Date.now() + 20000;
let settled;
let extraction;
do {
  settled = await call('get_editor_state');
  extraction = (await call('get_render_stats')).renderServices?.extraction;
  if (settled.pendingMaterialTextureBinds === 0 && extraction?.pendingPipeline === 0 &&
      extraction?.pendingMaterialLoad === 0 && Date.now() >= earliestCapture) break;
  await sleep(500);
} while (Date.now() < settleDeadline);
assert.equal(settled.pendingMaterialTextureBinds, 0, 'Material textures did not finish loading');
assert.equal(extraction?.pendingPipeline, 0, 'Material pipelines did not finish loading');
const shot = await call('take_screenshot', { target: 'window' });
assert.equal(shot.pendingMaterialTextureBinds ?? 0, 0, 'Screenshot contains pending textures');
assert(shot.pngBase64, 'No screenshot');
const output = path.join(project, 'results'); fs.mkdirSync(output, { recursive: true });
fs.writeFileSync(path.join(output, 'baked.png'), Buffer.from(shot.pngBase64, 'base64'));
fs.writeFileSync(path.join(output, 'bake-result.json'), JSON.stringify(result, null, 2));
console.log(JSON.stringify(result, null, 2));
