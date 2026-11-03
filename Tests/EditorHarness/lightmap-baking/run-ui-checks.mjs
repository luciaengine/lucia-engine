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
assert(project !== fs.realpathSync(path.dirname(fileURLToPath(import.meta.url))), 'Use a copy of this fixture');
const preview = process.argv.includes('--preview');
async function call(method, params = {}) {
  const result = await ipc(method, params);
  assert(!result?.error, `${method}: ${JSON.stringify(result)}`);
  return result;
}
async function until(predicate, message, timeout = 30000) {
  const end = Date.now() + timeout;
  while (Date.now() < end) { const value = await predicate(); if (value) return value; await sleep(200); }
  assert.fail(message);
}
function nodes(root) { return [root, ...(root.ch ?? []).flatMap(nodes)]; }
async function tree(id) { return call('get_ui_tree', { rootId: id, maxDepth: 12, includeHidden: true }); }
async function text(id) { return nodes(await tree(id)).map(n => n.text ?? '').join(' '); }
async function click(id) { await call('click_element', { elementId: id }); await sleep(250); }
async function revealSetting(id) {
  await call('set_scroll', { elementId: 'LightmapSettingsScroll', y: 0 });
  await sleep(250);
  const viewport = await tree('LightmapSettingsScroll');
  const field = await tree(id);
  if (field.y + field.h > viewport.y + viewport.h) {
    await call('set_scroll', { elementId: 'LightmapSettingsScroll', y: field.y - viewport.y });
    await sleep(250);
  }
}
async function openPanel() {
  const menu = await call('open_context_menu', { elementId: 'TopToolbarPanelMenu', button: 'left' });
  const entry = menu.items.find(item => item.path?.endsWith('Lighting Bake'));
  assert(entry, 'Lighting Bake is missing from the Panels menu');
  await call('invoke_menu_item', { commandId: entry.commandId });
  await until(async () => (await tree('LightmapBake')).w > 0, 'Baking panel did not open');
}
async function choose(id, label) {
  await revealSetting(id);
  await click(id);
  const option = nodes(await tree(`${id}:items`)).find(n => n.text === label);
  assert(option?.w > 0 && option?.h > 0, `No visible option ${label}`);
  await call('click_element', { x: option.x + option.w / 2, y: option.y + option.h / 2 });
  await sleep(250);
}
async function screenshot(name, params) {
  const shot = await call('take_screenshot', params);
  assert(shot.pngBase64, 'No screenshot');
  fs.mkdirSync(path.join(project, 'results'), { recursive: true });
  fs.writeFileSync(path.join(project, 'results', name), Buffer.from(shot.pngBase64, 'base64'));
}

const state = await call('get_editor_state');
assert(!state.sceneDirty, 'Use the saved, unbaked fixture so auto-apply can be detected');
assert(state.scenePath && fs.realpathSync(state.scenePath).startsWith(project + path.sep), 'Open the copied fixture first');
const entities = [];
function walk(entity) { entities.push(entity); for (const c of entity.children ?? []) walk(c); }
for (const entity of (await call('get_scene_hierarchy')).entities) walk(entity);
const floor = entities.find(e => e.name === 'Bake Floor'); assert(floor);

await call('select_entity', { entityId: floor.id });
await openPanel();
const panel = await tree('LightmapSettingsScroll');
assert(JSON.stringify(panel).includes('inspector-section'), 'Panel does not use native inspector sections');
const caption = nodes(panel).find(n => n.text === 'Resolution');
const statusLabel = await tree('LightmapStatus');
assert(caption && Math.abs(caption.x - statusLabel.x) <= 2, 'Bottom status does not align with inspector content');
const bakeButton = await tree('LightmapBake');
assert(Math.abs(bakeButton.x - statusLabel.x) <= 2, 'Bake action does not align with bottom status');
assert(!nodes(panel).some(n => n.id === 'LightmapSaveScene'), 'Panel duplicates the editor Save action');
assert.match(await text('LightmapResolution'), /1024/);
await choose('LightmapResolution', '8192 × 8192');
assert.match(await text('LightmapResolution'), /8192/);
await choose('LightmapResolution', '1024 × 1024');
await call('set_scroll', { elementId: 'LightmapSettingsScroll', y: 0 });
await sleep(250);
await click('LightmapAddSceneReceivers');
assert.match(await text('LightmapReceivers'), /3 meshes/);
assert.doesNotMatch(await text('LightmapReceivers'), /No Lightmap UVs/);
assert.match(await text('LightmapStatus'), /Skipped 1 unsupported/);
for (const name of ['Bake Floor', 'Bake Blocker', 'Bake Emitter']) assert.match(await text('LightmapReceivers'), new RegExp(name));
await click('LightmapStatus');
await screenshot('baking-panel-settings.png', { target: 'panel', panelId: 'LightmapBaking' });
await click('LightmapClearReceivers');
await click('LightmapAddReceivers');
assert.match(await text('LightmapReceivers'), /Bake Floor/);
assert.match(await text('LightmapOccluders'), /4 meshes/);
await click('LightmapBake');
const running = await until(async () => {
  const result = await call('get_lightmap_bake');
  return result.status === 'baking' && result.progress > 0 ? result : false;
}, 'The UI did not start a bake with progress');
assert.equal(running.totalReceivers, 1);
await click('LightmapCancel');
await until(async () => (await call('get_lightmap_bake')).status === 'cancelled', 'Cancel did not stop the bake');
assert.equal((await call('get_editor_state')).sceneDirty, false);
assert(!fs.existsSync(path.join(project, 'Assets', 'Lightmaps', running.jobId)), 'Cancelled bake left generated assets');
await until(async () => /cancelled/i.test(await text('LightmapStatus')), 'Panel did not display cancellation');

if (preview) {
  await choose('LightmapResolution', '64 × 64 · preview');
  await revealSetting('LightmapSamples');
  await call('input_text', { elementId: 'LightmapSamples', text: '64', clear: true, commit: true });
}
await choose('LightmapShadowMode', 'Baked only');
await click('LightmapBake');
// Hide the panel: editor completion must import/apply without a panel update
// or a get_lightmap_bake poll consuming the worker result.
await click('tab:Inspector');
await until(async () => (await call('get_editor_state')).sceneDirty, 'Background bake did not apply while the panel was hidden', 600000);
const result = await call('get_lightmap_bake');
assert.equal(result.status, 'complete', JSON.stringify(result));
assert.equal(result.applied, true, JSON.stringify(result));
assert(['cpu', 'gpu_ray_query', 'gpu_and_cpu'].includes(result.acceleration));
assert(result.tracedRays > 0 && result.gpuTracedRays >= 0 && result.gpuTracedRays <= result.tracedRays);
if (process.argv.includes('--require-gpu')) {
  assert.equal(result.acceleration, 'gpu_ray_query', 'GPU acceleration was required for this run');
  assert.equal(result.gpuTracedRays, result.tracedRays);
}
assert.equal(result.width, preview ? 64 : 1024);
assert.equal(result.indirectSamples, preview ? 64 : 256);
assert.equal(result.denoised, true);
assert.equal(result.receivers.length, 1);
for (const key of ['shadow', 'indirect', 'material']) assert(fs.existsSync(result.receivers[0][key]), key);
const material = JSON.parse(fs.readFileSync(result.receivers[0].material));
assert.equal(material.properties.userVec0[3], 1);
assert.equal(material.properties.userVec0[0], 3);
assert(material.textures.indirectLightmapMap.guid);

await openPanel();
await until(async () => /Baked materials applied/.test(await text('LightmapStatus')), 'Panel did not display successful assignment');
await call('set_scroll', { elementId: 'LightmapSettingsScroll', y: 0 });
// Use the editor's existing Save toolbar action.
await click('TopToolbarLeft1');
await until(async () => !(await call('get_editor_state')).sceneDirty, 'Save scene did not persist the bake');
assert(fs.readFileSync(state.scenePath, 'utf8').includes(result.receivers[0].materialGuid), 'Scene file does not reference the baked material');
await openPanel();
fs.mkdirSync(path.join(project, 'results'), { recursive: true });
fs.writeFileSync(path.join(project, 'results', 'ui-bake-result.json'), JSON.stringify(result, null, 2));
await click('LightmapStatus');
await screenshot('baking-panel.png', { target: 'panel', panelId: 'LightmapBaking' });
await call('focus_view', { view: 'game' });
await until(async () => (await call('get_editor_state')).pendingMaterialTextureBinds === 0, 'Baked textures did not load', 120000);
await sleep(20000);
await screenshot('baking-ui-window.png', { target: 'window' });
console.log(JSON.stringify({ passed: true, quality: preview ? 'preview' : 'default', result }, null, 2));
