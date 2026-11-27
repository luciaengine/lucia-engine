import assert from 'node:assert/strict';
import { mkdtemp, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { once } from 'node:events';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import test from 'node:test';
import { createCollaborationRelay } from './relay.mjs';
import { readRoomInvite } from './invite.mjs';

const token = 'integration-test-invite-key';

async function useRelay(config, action) {
  const server = createCollaborationRelay({ token, ...config });
  server.listen(0, '127.0.0.1');
  await once(server, 'listening');
  try {
    const base = 'http://127.0.0.1:' + server.address().port;
    await action(async (endpoint, body, authorization = token) => {
      const response = await fetch(base + '/v1/rooms/' + endpoint, {
        method: 'POST',
        headers: { Authorization: 'Bearer ' + authorization, 'Content-Type': 'application/json' },
        body: JSON.stringify(body),
      });
      return { code: response.status, body: await response.json() };
    }, base);
  } finally {
    await new Promise((resolve, reject) => server.close(error => error ? reject(error) : resolve()));
  }
}

test('presence lists two authenticated collaborators and drops outdated cursor samples', async () => {
  let now = 10000;
  await useRelay({ clock: () => now }, async send => {
    assert.equal((await send('scene/join', { name: 'Unknown' }, 'wrong')).code, 401);
    assert.equal((await send('another-room/join', { name: 'Unknown' })).code, 404);
    const alice = (await send('scene/join', { name: 'Alice' })).body;
    const bob = (await send('scene/join', { name: 'Bob' })).body;
    const cursor = { scene: 'Assets/Main.scene', view: 'perspective', x: .22, y: .83 };
    assert.equal((await send('scene/sync', { session: alice.session, sequence: 2, cursor })).code, 200);
    await send('scene/sync', { session: alice.session, sequence: 1, cursor: { ...cursor, x: 1 } });
    const fromBob = sequence => send('scene/sync', { session: bob.session, sequence, cursorOnly: true });
    const { body } = await fromBob(1);
    assert.deepEqual(body.peers.find(peer => peer.id === alice.id).cursor, cursor);
    assert.equal(body.peers.find(peer => peer.id === bob.id).cursor, null);
    now += 2100;
    assert.equal((await fromBob(2)).body.peers.find(peer => peer.id === alice.id).cursor, null);
    now += 21000;
    assert.equal((await fromBob(3)).code, 401);
  });
});

test('chat and call signals are delivered to authenticated peers without dropping an unacknowledged signal', async () => {
  await useRelay({}, async (send, base) => {
    const page = await fetch(base);
    assert.equal(page.status, 200);
    assert.match(await page.text(), /Invite token/);
    const alice = (await send('scene/join', { name: 'Alice', callable: true })).body;
    const bob = (await send('scene/join', { name: 'Bob', callable: true })).body;
    await send('scene/chat', { session: alice.session, text: '<Hello>' });
    const result = await send('scene/signal', { session: alice.session, to: bob.id,
      signal: { kind: 'description', call: 'test-call', value: { type: 'offer', sdp: 'test' }, from: 'fake' } });
    assert.equal(result.code, 200);
    const sync = (sequence, afterSignal) =>
      send('scene/sync', { session: bob.session, sequence, afterSignal });
    const { body } = await sync(1, 0);
    assert.deepEqual(body.chat, [{ author: 'Alice', text: '<Hello>', number: 1 }]);
    assert.equal(body.signals[0].from, alice.id);
    assert.equal((await sync(2, 0)).body.signals.length, 1);
    assert.equal((await sync(3, body.signals[0].number)).body.signals.length, 0);
  });
});

test('field-level revisions refuse conflicting writes and survive a relay restart', async () => {
  const directory = await mkdtemp(join(tmpdir(), 'lucia-collab-test-'));
  const edit = { kind: 'materialColor', target: 'material-guid', field: 'baseColor', value: [1, .5, 0, 1], expected: 0 };
  try {
    await useRelay({ directory }, async send => {
      const alice = (await send('scene/join', { name: 'Alice' })).body;
      const bob = (await send('scene/join', { name: 'Bob' })).body;
      const first = await send('scene/edit', { ...edit, session: alice.session });
      assert.equal(first.body.revision, 1);
      const conflict = await send('scene/edit', { ...edit, session: bob.session, value: [0, 0, 0, 1] });
      assert.equal(conflict.code, 409);
      assert.deepEqual(conflict.body.current.value, edit.value);
    });
    await useRelay({ directory }, async send => {
      const editor = (await send('scene/join', { name: 'Alice' })).body;
      const results = await send('scene/sync', { session: editor.session, sequence: 1 });
      assert.equal(results.body.edits.length, 1);
      assert.deepEqual(results.body.edits[0].value, edit.value);
      const next = await send('scene/edit', { ...edit, expected: 1, session: editor.session });
      assert.equal(next.body.revision, 2);
    });
  } finally {
    await rm(directory, { recursive: true, force: true });
  }
});


test('the command-line relay closes cleanly on SIGTERM', { timeout: 5000 }, async () => {
  const child = spawn(process.execPath, [fileURLToPath(new URL('./relay.mjs', import.meta.url))], {
    env: { ...process.env, LUCIA_COLLAB_TOKEN: token, LUCIA_COLLAB_PORT: '0',
      LUCIA_COLLAB_HOST: '127.0.0.1', LUCIA_COLLAB_STATE: '' },
    stdio: ['ignore', 'pipe', 'inherit'],
  });
  try {
    const [output] = await once(child.stdout, 'data');
    assert.match(output.toString(), /relay listening/);
    const exited = once(child, 'exit');
    child.kill('SIGTERM');
    assert.deepEqual(await exited, [0, null]);
  } finally {
    if (child.exitCode === null && child.signalCode === null) child.kill('SIGKILL');
  }
});


test('an invite fills the room and preserves reserved characters in the token', () => {
  const key = 'invite-token+with&reserved=#characters';
  assert.deepEqual(readRoomInvite('#room=studio&token=' + encodeURIComponent(key)), { room: 'studio', token: key });
  assert.equal(readRoomInvite('#room=studio&room=other&token=' + encodeURIComponent(key)), null);
  assert.equal(readRoomInvite('#room=../studio&token=' + encodeURIComponent(key)), null);
  assert.equal(readRoomInvite('#room=studio&token=short'), null);
});
