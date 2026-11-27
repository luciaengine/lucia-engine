import { createServer } from 'node:http';
import { randomBytes, timingSafeEqual } from 'node:crypto';
import { appendFileSync, mkdirSync, readFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { pathToFileURL } from 'node:url';

const companion = new Map([
  ['/', ['text/html; charset=utf-8', new URL('./room.html', import.meta.url)]],
  ['/room.js', ['text/javascript; charset=utf-8', new URL('./room.js', import.meta.url)]],
  ['/room.css', ['text/css; charset=utf-8', new URL('./room.css', import.meta.url)]],
]);

const maxPeers = 12;
const maxChanges = 8192;
const maxChat = 128;
const requestLimit = 96 * 1024;
const sessionTimeoutMs = 20_000;
const presenceTimeoutMs = 2_000;
const roomPattern = /^[a-zA-Z0-9_-]{1,80}$/;
const identifierPattern = /^[a-zA-Z0-9_.:-]{1,160}$/;

class RequestError extends Error {
  constructor(status, message) {
    super(message);
    this.status = status;
  }
}

function requireValid(condition, message) {
  if (!condition) throw new RequestError(400, message);
}

function authMatches(received, expected) {
  const supplied = Buffer.from(received || '');
  const secret = Buffer.from(expected);
  return supplied.length === secret.length && timingSafeEqual(supplied, secret);
}

function boundedString(value, limit) {
  return typeof value === 'string' && value.length > 0 && value.length <= limit;
}

async function readJson(request) {
  if (Number(request.headers['content-length']) > requestLimit) {
    throw new RequestError(413, 'Request too large');
  }
  const chunks = [];
  let bytes = 0;
  for await (const chunk of request) {
    bytes += chunk.length;
    if (bytes > requestLimit) throw new RequestError(413, 'Request too large');
    chunks.push(chunk);
  }
  try {
    const data = JSON.parse(Buffer.concat(chunks).toString('utf8'));
    requireValid(data && typeof data === 'object' && !Array.isArray(data), 'Expected an object');
    return data;
  } catch {
    throw new RequestError(400, 'Invalid JSON object');
  }
}

function cursorFrom(data) {
  if (data === null || data === undefined) return null;
  requireValid(boundedString(data.scene, 300) && identifierPattern.test(data.view || '') &&
    [data.x, data.y].every(n => typeof n === 'number' && Number.isFinite(n) && n >= 0 && n <= 1),
  'Invalid cursor');
  return { scene: data.scene, view: data.view, x: data.x, y: data.y };
}

function changeFrom(data) {
  requireValid(data && ['property', 'materialColor', 'script'].includes(data.kind) &&
    identifierPattern.test(data.target || '') && identifierPattern.test(data.field || '') &&
    Number.isSafeInteger(data.expected) && data.expected >= 0, 'Invalid change target or revision');
  if (data.kind === 'materialColor') {
    requireValid(Array.isArray(data.value) && data.value.length === 4 &&
      data.value.every(n => typeof n === 'number' && Number.isFinite(n) && n >= 0),
    'Expected a linear RGBA value');
  } else if (data.kind === 'script') {
    requireValid(typeof data.value === 'string' && data.value.length <= 48_000, 'Invalid script text');
  } else {
    requireValid(data.value !== undefined && JSON.stringify(data.value).length <= 8192, 'Invalid property value');
  }
  return { kind: data.kind, target: data.target, field: data.field, value: data.value, expected: data.expected };
}

function keyOf(change) {
  return [change.kind, change.target, change.field].join('/');
}

function roomState(directory, name) {
  const file = directory ? resolve(directory, name + '.jsonl') : null;
  const room = { file, peers: new Map(), chat: [], nextChat: 1, changes: [], revisions: new Map() };
  if (!file) return room;
  try {
    for (const line of readFileSync(file, 'utf8').split('\n')) {
      if (!line) continue;
      const change = JSON.parse(line);
      room.changes.push(change);
      room.revisions.set(keyOf(change), change.revision);
    }
  } catch (error) {
    if (error.code !== 'ENOENT') throw error;
  }
  return room;
}

function expire(room, now) {
  for (const [session, peer] of room.peers) {
    if (now - peer.lastSeen > sessionTimeoutMs) room.peers.delete(session);
  }
}

function makePeer(room, data, now) {
  const name = typeof data.name === 'string' ? data.name.trim() : '';
  requireValid(boundedString(name, 48) && !/[\u0000-\u001f\u007f]/u.test(name), 'Display name required');
  if (room.peers.size >= maxPeers) throw new RequestError(429, 'Room is full');
  const session = randomBytes(24).toString('base64url');
  const peer = { id: randomBytes(12).toString('base64url'), name, cursor: null,
    sequence: -1, lastSeen: now, cursorAt: 0, callable: data.callable === true, messages: [], nextSignal: 1 };
  room.peers.set(session, peer);
  return { session, id: peer.id };
}

function listPeers(room, self, now) {
  return Array.from(room.peers.values(), peer => ({
    id: peer.id,
    name: peer.name,
    callable: peer.callable,
    cursor: peer !== self && now - peer.cursorAt <= presenceTimeoutMs ? peer.cursor : null,
  }));
}

function recordChange(room, peer, data) {
  const input = changeFrom(data);
  const key = keyOf(input);
  const revision = room.revisions.get(key) || 0;
  if (input.expected !== revision) {
    const last = room.changes.findLast(change => keyOf(change) === key);
    throw Object.assign(new RequestError(409, 'The field changed since you opened it'), { current: last });
  }
  if (room.changes.length >= maxChanges) throw new RequestError(507, 'The room change journal is full');
  const change = { ...input, revision: revision + 1, number: room.changes.length + 1, author: peer.id };
  if (room.file) appendFileSync(room.file, JSON.stringify(change) + '\n', { mode: 0o600, flush: true });
  room.changes.push(change);
  room.revisions.set(key, change.revision);
  return change;
}

export function createCollaborationRelay({ token, roomName = 'scene', directory, clock = Date.now } = {}) {
  if (typeof token !== 'string' || token.length < 24) throw new Error('Set a room token of at least 24 characters');
  if (directory) mkdirSync(directory, { recursive: true, mode: 0o700 });
  if (!roomPattern.test(roomName)) throw new Error('Invalid relay room name');
  let room;
  const server = createServer({ requestTimeout: 8_000, headersTimeout: 4_000 }, async (request, response) => {
    const send = (status, body) => {
      response.writeHead(status, { 'Content-Type': 'application/json', 'Cache-Control': 'no-store' });
      response.end(JSON.stringify(body));
    };
    try {
      const asset = request.method === 'GET' ? companion.get(request.url) : undefined;
      if (asset) {
        response.writeHead(200, { 'Content-Type': asset[0], 'Cache-Control': 'no-store',
          'Content-Security-Policy': "default-src 'self'; media-src 'self' blob:; frame-ancestors 'self'",
          'X-Content-Type-Options': 'nosniff' });
        return response.end(readFileSync(asset[1]));
      }
      if (request.method !== 'POST') throw new RequestError(405, 'Use POST');
      if (!authMatches(request.headers.authorization, 'Bearer ' + token)) throw new RequestError(401, 'Invalid room token');
      const [, api, roomsSegment, requestedRoom, operation, ...rest] = (request.url || '').split('/');
      if (api !== 'v1' || roomsSegment !== 'rooms' || rest.length || requestedRoom !== roomName ||
          !['join', 'sync', 'leave', 'chat', 'signal', 'edit'].includes(operation)) {
        throw new RequestError(404, 'Unknown room endpoint');
      }
      const data = await readJson(request);
      const now = clock();
      if (!room) {
        if (operation !== 'join') throw new RequestError(401, 'Join the room again');
        room = roomState(directory, roomName);
      }
      expire(room, now);
      if (operation === 'join') return send(200, makePeer(room, data, now));
      const peer = room.peers.get(data.session);
      if (!peer) throw new RequestError(401, 'Join the room again');
      peer.lastSeen = now;
      if (operation === 'leave') {
        room.peers.delete(data.session);
        return send(200, {});
      }
      if (operation === 'sync') {
        requireValid(Number.isSafeInteger(data.sequence) && data.sequence >= 0, 'Invalid presence sequence');
        if (data.sequence > peer.sequence) {
          peer.cursor = cursorFrom(data.cursor);
          peer.cursorAt = now;
          peer.sequence = data.sequence;
        }
        if (data.cursorOnly === true) return send(200, { peers: listPeers(room, peer, now) });
        const afterChat = Number.isSafeInteger(data.afterChat) ? data.afterChat : 0;
        const afterEdit = Number.isSafeInteger(data.afterEdit) ? data.afterEdit : 0;
        const afterSignal = Number.isSafeInteger(data.afterSignal) ? data.afterSignal : 0;
        peer.messages = peer.messages.filter(signal => signal.number > afterSignal);
        const signals = peer.messages.slice(0, 8);
        return send(200, { peers: listPeers(room, peer, now), signals,
          chat: room.chat.filter(line => line.number > afterChat),
          edits: room.changes.slice(Math.max(0, afterEdit), Math.max(0, afterEdit) + 16) });
      }
      if (operation === 'chat') {
        const text = typeof data.text === 'string' ? data.text.trim() : '';
        requireValid(boundedString(text, 2000), 'Message is empty or too long');
        const message = { author: peer.name, text, number: room.nextChat++ };
        room.chat.push(message);
        if (room.chat.length > maxChat) room.chat.shift();
        return send(200, message);
      }
      if (operation === 'signal') {
        const target = Array.from(room.peers.values()).find(member => member.id === data.to && member !== peer);
        requireValid(target?.callable && peer.callable && data.signal && ['description', 'candidate', 'hangup'].includes(data.signal.kind) &&
          identifierPattern.test(data.signal.call || '') &&
          JSON.stringify(data.signal).length <= 24_000, 'Invalid call signal');
        if (target.messages.length >= 64) throw new RequestError(429, 'Peer is not receiving signals');
        target.messages.push({ ...data.signal, from: peer.id, number: target.nextSignal++ });
        return send(200, {});
      }
      return send(200, recordChange(room, peer, data));
    } catch (error) {
      if (!(error instanceof RequestError)) {
        console.error(error.message);
        return send(500, { error: 'Room state is unavailable' });
      }
      return send(error.status, { error: error.message, current: error.current });
    }
  });
  server.maxConnections = 128;
  return server;
}

if (process.argv[1] && import.meta.url === pathToFileURL(resolve(process.argv[1])).href) {
  const port = Number(process.env.LUCIA_COLLAB_PORT || 8746);
  const host = process.env.LUCIA_COLLAB_HOST || '127.0.0.1';
  const relay = createCollaborationRelay({
    token: process.env.LUCIA_COLLAB_TOKEN,
    roomName: process.env.LUCIA_COLLAB_ROOM,
    directory: process.env.LUCIA_COLLAB_STATE,
  });
  relay.listen(port, host, () => console.log(`Lucia collaboration relay listening on ${host}:${port}`));
}
