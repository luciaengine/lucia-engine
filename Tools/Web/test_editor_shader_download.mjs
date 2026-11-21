import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';

const host = new URL('../../Managed/Scripting.Web/EditorHost/main.mjs', import.meta.url);
const source = await readFile(process.argv[2] ?? host, 'utf8');
const begin = source.indexOf('function createShaderDownload(');
const end = source.indexOf('function createScriptSession(', begin);
assert.ok(begin >= 0 && end > begin);
const create = new Function(source.slice(begin, end) + ';return createShaderDownload;')();
const flush = () => new Promise(resolve => setImmediate(resolve));
let downloads = 0;
let installs = 0;
let complete;
let reject;
const prep = () => {
    downloads++;
    return new Promise((resolve, fail) => {
        complete = resolve;
        reject = fail;
    });
};
const shader = create(prep);
assert.equal(shader.poll(), 0);
assert.equal(shader.poll(), 0);
assert.equal(downloads, 1);
const former = console.error;
console.error = () => {};
try {
    reject(new Error('Injected transfer failure'));
    await flush();
    assert.equal(shader.poll(), -1);
    assert.equal(shader.poll(), 0);
    assert.equal(downloads, 2);
    complete(() => { throw new Error('Injected mount failure'); });
    await flush();
    assert.equal(shader.poll(), 0);
    shader.mountReady();
    assert.equal(shader.poll(), -1);
    assert.equal(shader.poll(), 0);
    assert.equal(downloads, 3);
} finally {
    console.error = former;
}
complete(() => { installs++; });
await flush();
assert.equal(installs, 0);
assert.equal(shader.poll(), 0);
shader.mountReady();
assert.equal(installs, 1);
assert.equal(shader.poll(), 1);
shader.mountReady();
assert.equal(installs, 1);
assert.equal(downloads, 3);

const direct = create(prep);
direct.mount(() => { installs++; });
assert.equal(direct.poll(), 1);
assert.equal(installs, 2);
assert.equal(downloads, 3);
console.log('Deferred mounting, single transfer, retry and direct project passed');
