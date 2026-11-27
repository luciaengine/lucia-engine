const byId = id => document.getElementById(id);
const joinForm = byId('join');
const sessionPanel = byId('session');
const peerPicker = byId('peers');
const messageForm = byId('message');
const chat = byId('chat');
const status = byId('status');

let connection;
let call;
let incoming;
let peers = [];

async function post(state, operation, value) {
  const response = await fetch('/v1/rooms/' + encodeURIComponent(state.room) + '/' + operation, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json', Authorization: 'Bearer ' + state.token },
    body: JSON.stringify(value),
    signal: AbortSignal.timeout(5000),
  });
  const answer = await response.json();
  if (!response.ok) throw Object.assign(new Error(answer.error), { status: response.status });
  return answer;
}

function showPeers(next, localId) {
  peers = next;
  const previous = peerPicker.value;
  peerPicker.replaceChildren();
  for (const peer of next) {
    if (peer.id === localId || !peer.callable) continue;
    peerPicker.add(new Option(peer.name, peer.id));
  }
  if (Array.from(peerPicker.options).some(option => option.value === previous)) peerPicker.value = previous;
  byId('call').disabled = !peerPicker.value || !!call || !!incoming;
}

function sendSignal(state, remoteId, signal) {
  return post(state, 'signal', { session: state.session, to: remoteId, signal });
}

function muteButton(kind, changed) {
  const button = byId(kind === 'audio' ? 'mic' : 'camera');
  button.setAttribute('aria-pressed', String(changed));
  const device = kind === 'audio' ? 'mic' : 'camera';
  button.textContent = changed ? 'Turn ' + device + ' off' : 'Turn ' + device + ' on';
}

function endCall(tellPeer) {
  const old = call;
  call = null;
  if (old) {
    for (const track of old.local.getTracks()) track.stop();
    old.pc.close();
    if (tellPeer && connection) {
      void sendSignal(connection, old.peer, { kind: 'hangup', call: old.id }).catch(() => {});
    }
  }
  byId('local').srcObject = null;
  byId('remote').srcObject = null;
  byId('media').hidden = true;
  muteButton('audio', false);
  muteButton('video', false);
  if (connection) showPeers(peers, connection.id);
}

function clearIncoming() {
  incoming = null;
  byId('incoming').hidden = true;
  if (connection) showPeers(peers, connection.id);
}

function makeCall(state, peer, id) {
  // Host candidates are enough on a shared network. A deployment can supply
  // TURN and STUN through an authenticated endpoint when it needs WAN calls.
  const pc = new RTCPeerConnection();
  const current = { pc, peer, id, candidates: [], outgoingCandidates: [],
    descriptionSent: false, local: new MediaStream() };
  call = current;
  byId('media').hidden = false;
  byId('local').srcObject = current.local;
  pc.ontrack = ({ track }) => {
    const video = byId('remote');
    if (!video.srcObject) video.srcObject = new MediaStream();
    video.srcObject.addTrack(track);
  };
  pc.onicecandidate = ({ candidate }) => {
    if (call !== current || !candidate) return;
    if (!current.descriptionSent) {
      current.outgoingCandidates.push(candidate.toJSON());
      return;
    }
    void sendSignal(state, peer, { call: id, kind: 'candidate', value: candidate.toJSON() })
      .catch(error => { status.textContent = error.message; });
  };
  pc.onconnectionstatechange = () => {
    if (call !== current) return;
    if (pc.connectionState === 'failed') {
      status.textContent = 'The call could not connect.';
      endCall(true);
    }
  };
  showPeers(peers, state.id);
  return current;
}

async function shareDescription(state, current) {
  await sendSignal(state, current.peer,
    { kind: 'description', call: current.id, value: current.pc.localDescription });
  current.descriptionSent = true;
  for (const candidate of current.outgoingCandidates.splice(0)) {
    if (call !== current) return;
    await sendSignal(state, current.peer, { kind: 'candidate', call: current.id, value: candidate });
  }
}

async function setRemote(current, description) {
  await current.pc.setRemoteDescription(description);
  for (const candidate of current.candidates.splice(0)) await current.pc.addIceCandidate(candidate);
}

async function handleSignal(state, signal) {
  if (signal.kind === 'hangup') {
    if (call?.peer === signal.from && call.id === signal.call) endCall(false);
    if (incoming?.from === signal.from && incoming.call === signal.call) clearIncoming();
    return;
  }
  if (signal.kind === 'description' && signal.value?.type === 'offer') {
    if (call && call.peer === signal.from && state.id > signal.from) endCall(true);
    if (call || incoming) {
      await sendSignal(state, signal.from, { call: signal.call, kind: 'hangup' });
      return;
    }
    incoming = { ...signal, candidates: [] };
    const caller = peers.find(peer => peer.id === signal.from);
    byId('caller').textContent = (caller?.name || 'Collaborator') + ' is calling';
    byId('incoming').hidden = false;
    showPeers(peers, state.id);
    return;
  }
  if (incoming?.from === signal.from && incoming.call === signal.call && signal.kind === 'candidate') {
    if (incoming.candidates.length < 64) incoming.candidates.push(signal.value);
    return;
  }
  const current = call;
  if (!current || current.peer !== signal.from || current.id !== signal.call) return;
  if (signal.kind === 'description' && signal.value?.type === 'answer') {
    await setRemote(current, signal.value);
  } else if (signal.kind === 'candidate') {
    if (current.pc.remoteDescription) await current.pc.addIceCandidate(signal.value);
    else if (current.candidates.length < 64) current.candidates.push(signal.value);
  }
}

async function poll(state) {
  try {
    const answer = await post(state, 'sync', {
      session: state.session, sequence: ++state.sequence, cursor: null,
      afterChat: state.afterChat, afterSignal: state.afterSignal, afterEdit: Number.MAX_SAFE_INTEGER,
    });
    if (connection !== state) return;
    showPeers(answer.peers, state.id);
    if (call && !peers.some(peer => peer.id === call.peer)) endCall(false);
    if (incoming && !peers.some(peer => peer.id === incoming.from)) clearIncoming();
    for (const message of answer.chat) {
      const item = document.createElement('li');
      const name = document.createElement('strong');
      name.textContent = message.author;
      item.append(name, ' ' + message.text);
      chat.append(item);
      while (chat.children.length > 128) chat.firstElementChild.remove();
      chat.scrollTop = chat.scrollHeight;
      state.afterChat = message.number;
    }
    for (const signal of answer.signals) {
      try {
        await handleSignal(state, signal);
      } catch (error) {
        status.textContent = error.message;
      }
      state.afterSignal = signal.number;
    }
  } catch (error) {
    if (connection !== state) return;
    status.textContent = error.message;
    if (error.status === 401) await leave(false);
  }
  if (connection === state) setTimeout(() => void poll(state), 250);
}

async function leave(tellRelay) {
  const state = connection;
  connection = null;
  endCall(false);
  clearIncoming();
  chat.replaceChildren();
  joinForm.hidden = false;
  sessionPanel.hidden = true;
  if (tellRelay && state) {
    await post(state, 'leave', { session: state.session }).catch(() => {});
  }
}

joinForm.addEventListener('submit', async event => {
  event.preventDefault();
  const data = new FormData(joinForm);
  const button = joinForm.querySelector('button');
  button.disabled = true;
  try {
    const options = { room: data.get('room'), token: data.get('token') };
    const session = await post(options, 'join', { name: data.get('name'), callable: !!globalThis.RTCPeerConnection });
    connection = { ...options, ...session, sequence: 0, afterChat: 0, afterSignal: 0 };
    joinForm.elements.token.value = '';
    joinForm.hidden = true;
    sessionPanel.hidden = false;
    status.textContent = 'Joined';
    void poll(connection);
  } catch (error) {
    status.textContent = error.message;
  } finally {
    button.disabled = false;
  }
});

messageForm.addEventListener('submit', async event => {
  event.preventDefault();
  if (!connection) return;
  const field = messageForm.elements.text;
  const original = field.value;
  try {
    await post(connection, 'chat', { session: connection.session, text: original });
    if (field.value === original) field.value = '';
  } catch (error) {
    status.textContent = error.message;
  }
});

byId('call').addEventListener('click', async () => {
  const state = connection;
  if (!state || !peerPicker.value || call || incoming) return;
  try {
    const current = makeCall(state, peerPicker.value, crypto.randomUUID());
    current.pc.addTransceiver('audio', { direction: 'sendrecv' });
    current.pc.addTransceiver('video', { direction: 'sendrecv' });
    await current.pc.setLocalDescription(await current.pc.createOffer());
    await shareDescription(state, current);
  } catch (error) {
    status.textContent = error.message;
    endCall(false);
  }
});

byId('accept').addEventListener('click', async () => {
  const state = connection;
  const offer = incoming;
  if (!state || !offer || call) return;
  clearIncoming();
  try {
    const current = makeCall(state, offer.from, offer.call);
    current.candidates = offer.candidates;
    await setRemote(current, offer.value);
    for (const transceiver of current.pc.getTransceivers()) transceiver.direction = 'sendrecv';
    await current.pc.setLocalDescription(await current.pc.createAnswer());
    await shareDescription(state, current);
  } catch (error) {
    status.textContent = error.message;
    endCall(true);
  }
});

byId('decline').addEventListener('click', () => {
  const offer = incoming;
  clearIncoming();
  if (offer && connection) {
    void sendSignal(connection, offer.from, { kind: 'hangup', call: offer.call }).catch(() => {});
  }
});

async function changeDevice(kind) {
  const current = call;
  const button = byId(kind === 'audio' ? 'mic' : 'camera');
  if (!current) return;
  const sender = current.pc.getTransceivers().find(item => item.receiver.track.kind === kind)?.sender;
  if (!sender) return;
  button.disabled = true;
  try {
    if (sender.track) {
      const previous = sender.track;
      await sender.replaceTrack(null);
      previous.stop();
      current.local.removeTrack(previous);
      muteButton(kind, false);
      return;
    }
    const stream = await navigator.mediaDevices.getUserMedia(kind === 'audio' ? { audio: true } : { video: true });
    const track = stream.getTracks()[0];
    if (current !== call) {
      track.stop();
      return;
    }
    try {
      await sender.replaceTrack(track);
    } catch (error) {
      track.stop();
      throw error;
    }
    current.local.addTrack(track);
    muteButton(kind, true);
  } catch (error) {
    status.textContent = error.message;
  } finally {
    button.disabled = false;
  }
}

byId('mic').addEventListener('click', () => void changeDevice('audio'));
byId('camera').addEventListener('click', () => void changeDevice('video'));
byId('hangup').addEventListener('click', () => endCall(true));
byId('leave').addEventListener('click', () => void leave(true));
addEventListener('pagehide', () => void leave(false));
