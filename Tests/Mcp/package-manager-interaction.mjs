// Run against an isolated editor started with --session-label package-manager-review.
// Set GE_PACKAGE_PROJECT to its copied fixture and GE_PACKAGE_EVIDENCE to an external evidence directory.
// GE_EDITOR_DEBUG_PORT selects the isolated port.
import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { mkdirSync, writeFileSync, copyFileSync, realpathSync } from 'node:fs';
import { join, isAbsolute } from 'node:path';

const port = Number(process.env.GE_EDITOR_DEBUG_PORT);
assert(port > 0 && port !== 9999 && port !== 9998, 'Use an isolated editor port');
const evidence = process.env.GE_PACKAGE_EVIDENCE;
assert(evidence && isAbsolute(evidence), 'Set an absolute external evidence directory');
const cli = fileURLToPath(new URL('../../mcp/ge.mjs', import.meta.url));
const records = [];
function call(name, args = {}) {
    const parameters = { ...args };
    const result = JSON.parse(execFileSync(process.execPath,
        [cli, name, JSON.stringify(parameters), '--port', String(port), '--json'], { encoding: 'utf8' }));
    assert(result.ok, `${name}: ${JSON.stringify(result)}`);
    records.push({ name, parameters, data: result.data });
    return result.data;
}
function nodes(root) {
    return [root, ...(root.ch ?? []).flatMap(nodes)];
}
function panel() {
    return nodes(call('get_panel_tree', { panelId: 'PackageManager', maxDepth: 20 }));
}
function eventually(read, matches) {
    const deadline = Date.now() + 5000;
    do {
        const value = read();
        if (matches(value)) return value;
    } while (Date.now() < deadline);
    assert.fail('Editor state did not settle within five seconds');
}
function selectedCount() {
    return panel().filter(node => node.cls?.split(' ').includes('pkg-row') && node.cls.split(' ').includes('selected')).length;
}
function click(elementId) { call('click_element', { elementId }); }
function key(key, mods) { call('send_key', { key, ...(mods ? { mods } : {}) }); }
function detailContains(text) {
    eventually(panel, tree => {
        const detail = tree.find(node => node.id === 'PackageDetails');
        return detail && nodes(detail).some(node => node.text?.includes(text));
    });
}

function statusContains(text) {
    eventually(panel, tree => tree.some(node => node.cls?.split(' ').includes('pkg-action-status') && node.text?.includes(text)));
}

assert.equal(call('get_editor_state').session.label, 'package-manager-review');
const project = process.env.GE_PACKAGE_PROJECT;
assert(project && isAbsolute(project), 'Set GE_PACKAGE_PROJECT to the copied fixture project');
assert.equal(realpathSync(call('get_build_settings').workspaceRoot), realpathSync(project));
click('TopToolbarPackages');
const tree = eventually(panel, nodes => nodes.some(node => node.id === 'PackageList' && node.h > 0));
const packageCount = Number(tree.find(node => node.cls?.split(' ').includes('pkg-status')).text.match(/^(\d+)/)[1]);
assert(tree.some(node => node.id === 'PackageList' && node.h > 0));
const search = tree.find(node => node.cls?.split(' ').includes('pkg-search'));
assert(search && search.h > 0 && search.h <= 28, 'Search must stay compact');
const body = tree.find(node => node.cls?.split(' ').includes('pkg-body'));
assert(search.y >= body.y + body.h + 5.5, 'Review fixture must retain bottom search placement');
click('PackageList');
key('Home');
eventually(selectedCount, count => count === 1);
key('Down', 'shift');
eventually(selectedCount, count => count === 2);
detailContains('2 packages selected');
detailContains('Enable selected');
detailContains('Disable selected');
click('PackageDisableSelected');
statusContains('Disabled 2 package(s)');
detailContains('2 packages selected');
click('PackageEnableSelected');
statusContains('Enabled 2 package(s)');
detailContains('2 packages selected');
click('PackageList');
key('A', process.platform === 'darwin' ? 'super' : 'control');
detailContains(`${packageCount} packages selected`);
key('Home');
key('Down');
eventually(selectedCount, count => count === 1);
detailContains('chromatic-aberration');
const state = call('get_editor_state');
assert.equal(state.pendingMaterialTextureBinds, 0);
const capture = call('take_screenshot', { target: 'panel', panelId: 'PackageManager' });
assert.equal(capture.method, 'rendergraph');
mkdirSync(evidence, { recursive: true });
copyFileSync(capture.filePath, join(evidence, 'verified-package-panel.png'));
writeFileSync(join(evidence, 'package-interaction.json'), JSON.stringify(records, null, 2));
console.log('PASS: compact search, arrows, range/select-all, bulk disable/enable, detail and rendergraph capture');
