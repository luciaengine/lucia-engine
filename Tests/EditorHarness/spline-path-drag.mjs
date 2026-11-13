#!/usr/bin/env node
// Run against an isolated Editor with a disposable project, in Edit, with a scene whose ground at
// the world origin is flat (terrain relief adds knots between the corners):
//   GE_EDITOR_DEBUG_PORT=<port> node Tests/EditorHarness/spline-path-drag.mjs [--out=<folder>]
// One simulate_mouse_drag with a path draws a regular octagon with the spline tool and ends on its start.
// Asserts the stroke makes one new spline that is closed with one knot per corner. The spline is read
// back from a scene saved under --out (default: the temp folder); the save makes that file the active
// scene. Deletes the spline afterwards. Drives the editor only through its debug port.
import assert from 'node:assert/strict';
import os from 'node:os';
import path from 'node:path';
import fs from 'node:fs';
import { ipc, sleep, PORT } from './lib/ipc.mjs';

const kCorners = 8;
const kStepsPerSide = 12;

async function call(method, params = {}) {
    const result = await ipc(method, params);
    assert.ok(!result?.error, `${method}: ${JSON.stringify(result)}`);
    return result;
}

function find(node, accept) {
    if (!node) return null;
    if (accept(node)) return node;
    for (const child of node.ch ?? node.children ?? []) {
        const found = find(child, accept);
        if (found) return found;
    }
    return null;
}

async function splineIds() {
    const hierarchy = await call('get_scene_hierarchy');
    return (hierarchy.entities ?? []).filter(e => e.name === 'Spline').map(e => e.id);
}

// The corners of a regular octagon around the view centre, starting and ending on the first corner.
function octagon(view) {
    const cx = view.x + view.w / 2, cy = view.y + view.h / 2;
    const radius = Math.min(view.w, view.h) * 0.3;
    const corners = [];
    for (let i = 0; i <= kCorners; ++i) {
        const angle = (2 * Math.PI * (i % kCorners)) / kCorners;
        corners.push({ x: cx + radius * Math.cos(angle), y: cy + radius * Math.sin(angle) });
    }
    return corners;
}

// Spline.closed and Spline.pointCount of the saved entity whose block carries `tag`.
function savedSpline(sceneText, tag) {
    const block = sceneText.split('[entity ').find(b => b.startsWith(`id="${tag}"]`));
    assert.ok(block, `the saved scene holds entity ${tag}`);
    const closed = /Spline\.closed = (true|false)/.exec(block);
    const count = /Spline\.pointCount = (\d+)/.exec(block);
    assert.ok(closed && count, `entity ${tag} saved a spline`);
    return { closed: closed[1] === 'true', knots: Number(count[1]) };
}

async function main() {
    const outArg = process.argv.find(a => a.startsWith('--out='));
    const outDir = outArg ? outArg.slice('--out='.length) : os.tmpdir();
    const scenePath = path.join(outDir, 'spline-path-drag.scene').replaceAll('\\', '/');

    const tree = await call('get_ui_tree', { maxDepth: 30 });
    const view = find(tree.root ?? tree, n => n.id === 'SceneViewViewport' && n.w > 200 && n.h > 200);
    assert.ok(view, 'the scene view viewport has a layout box');

    await call('look_at', { target: [0, 0, 0], distance: 60 });
    await call('set_camera', { pitchDeg: -89, yawDeg: 0 });
    await sleep(500);
    await call('set_scene_tool', { tool: 'spline' });

    const before = await splineIds();
    const [start, ...corners] = octagon(view);
    await call('move_pointer', start);
    const drag = await call('simulate_mouse_drag', {
        startX: start.x, startY: start.y, path: corners, steps: kCorners * kStepsPerSide,
    });
    await sleep(1000);
    const created = (await splineIds()).filter(id => !before.includes(id));
    let entityId;
    try {
        assert.equal(created.length, 1, `one new spline: ${JSON.stringify(created)}`);
        entityId = created[0];
        // An entity has the scene tag of its saved block after a save.
        await call('save_scene', { path: scenePath });
        const components = await call('get_entity_components', { entityId });
        const tag = components?.components?.SceneEntityTag?.value;
        assert.ok(tag, `spline ${entityId} has a scene tag`);
        const spline = savedSpline(fs.readFileSync(scenePath, 'utf8'), tag);
        assert.ok(spline.closed, `the stroke closed the loop: ${JSON.stringify(spline)}`);
        assert.equal(spline.knots, kCorners, `one knot per corner: ${JSON.stringify(spline)}`);
        console.log(`PASS spline-path-drag: spline ${entityId} closed with ${spline.knots} knots after ` +
                    `${drag.steps} moves; editor on port ${PORT}`);
    } finally {
        for (const id of created)
            await ipc('delete_entity', { entityId: id });
        await ipc('set_scene_tool', { tool: 'selection' });
    }
}

main().catch((error) => {
    console.error(`FAIL spline-path-drag: ${error.message}`);
    process.exit(1);
});
