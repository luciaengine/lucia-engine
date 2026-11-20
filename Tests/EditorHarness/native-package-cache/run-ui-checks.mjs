import assert from 'node:assert/strict';
import { ipc, PORT } from '../lib/ipc.mjs';

// Start an isolated Editor with EZTree enabled and Packages visible.
// Point GE_PACKAGE_CACHE_DIR at a scratch directory. For "blocked", put a
// regular file named .native in that directory before launching the editor.
const [sessionLabel, cacheState] = process.argv.slice(2);
assert(sessionLabel && (cacheState === 'blocked' || cacheState === 'ready'),
  'Provide the editor session label and cache state (blocked or ready).');
assert.notEqual(PORT, 9999, 'Use a dedicated editor debug port.');

const state = await ipc('get_editor_state');
assert.equal(state.session?.label, sessionLabel, 'The debug port belongs to a different editor.');
const packages = await ipc('get_panel_tree', {
  panelId: 'PackageManager',
  maxDepth: 20,
  maxTextLength: 0,
  includeHidden: true,
});
assert.equal(packages.searchIsComplete, true, JSON.stringify(packages));

function descendants(node) {
  return [node, ...(node.ch ?? []).flatMap(descendants)];
}

const nodes = descendants(packages);
assert(nodes.some(node => node.text === 'eztree'), 'The Packages panel must show EZTree.');
const errors = nodes.filter(node => node.cls?.split(' ').includes('pkg-issue-error'));
for (const moduleName of ['Eztree', 'Eztree.Editor']) {
  const notice = errors.find(node => node.text?.startsWith(`Skipped '${moduleName}' from 'eztree'.`));
  if (cacheState === 'blocked') {
    assert(notice, `Missing cache failure for ${moduleName}.`);
    assert(!notice.hidden && notice.w > 0 && notice.h > 0, `Cache failure for ${moduleName} is hidden.`);
    assert.match(notice.text, /native code cache could not be opened/);
    assert.match(notice.text, /writable, then reopen the project/);
  } else {
    assert.equal(notice, undefined, `Unexpected cache failure for ${moduleName}.`);
  }
}
