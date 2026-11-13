import test from 'node:test';
import assert from 'node:assert/strict';
import { once } from 'node:events';
import { createHmac } from 'node:crypto';
import { WebSocket } from 'ws';
import { createLobby, VERSION } from './server.mjs';

test('rooms, authorization, isolation, relay packets, expiry and cleanup', async () => {
  let time = Date.now();
  const lobby = createLobby({ now: () => time });
  lobby.server.listen(0, '127.0.0.1'); await once(lobby.server, 'listening');
  const base = `http://127.0.0.1:${lobby.server.address().port}`;
  const api = async (path, body, version = VERSION) => {
    const r = await fetch(base + path, { method: body ? 'POST' : 'GET',
      headers: { 'X-Nidhogg-Version': version, 'Content-Type': 'application/json' },
      ...(body ? { body: JSON.stringify(body) } : {}) });
    return [r.status, await r.json()];
  };
  const create = isPublic => api('/api/rooms/create', { roomName: 'Arena', hostName: 'host', region: 'all', stageIndex: 0, isPublic });
  const wsConnect = async p => {
    const ws = new WebSocket(base.replace('http:', 'ws:') + '/relay?roomId=' + p.roomId,
      { headers: { 'X-Nidhogg-Version': VERSION, 'X-Nidhogg-Token': p.authToken } });
    await once(ws, 'open'); return ws;
  };
  try {
    assert.equal((await api('/api/rooms', undefined, 'old'))[0], 426);
    assert.equal((await api('/api/rooms/create', { stageIndex: 99 }))[0], 400);
    const [status, host] = await create(false); assert.equal(status, 200);
    assert.equal((await api('/api/rooms/start', { roomId: host.roomId, authToken: host.authToken }))[0], 409);
    assert.equal((await api('/api/rooms'))[1].rooms.length, 0);
    assert.equal((await api('/api/rooms/join', { roomId: host.roomId }))[0], 404);
    const [joined, guest] = await api('/api/rooms/join_code', { roomCode: host.roomCode, playerName: 'guest' });
    assert.equal(joined, 200); assert.equal(guest.sessionKey, host.sessionKey);
    const auth = p => ({ roomId: p.roomId, authToken: p.authToken });
    assert.equal((await api('/api/rooms/join_code', { roomCode: host.roomCode }))[0], 409);
    assert.equal((await api('/api/rooms/start', auth(guest)))[0], 403);
    assert.equal((await api('/api/rooms/heartbeat', { ...auth(host), authToken: 'forged' }))[0], 403);
    assert.equal(guest.hostReady, false); assert.equal(guest.guestReady, false);
    assert.equal((await api('/api/rooms/start', { ...auth(host), stageIndex: 99 }))[0], 400);
    assert.equal((await api('/api/rooms/start', { ...auth(host), stageIndex: 1.5 }))[0], 400);
    const beforeStartKey = host.sessionKey;
    const started = (await api('/api/rooms/start', { ...auth(host), stageIndex: 2 }))[1];
    assert.equal(started.stageIndex, 2);
    assert.equal(started.status, 'in_match'); assert.notEqual(started.sessionKey, beforeStartKey);
    assert.equal((await api('/api/rooms/start', { ...auth(host), stageIndex: 2 }))[1].sessionKey, started.sessionKey);
    assert.equal((await api('/api/rooms/start', { ...auth(host), stageIndex: 3 }))[0], 409);
    Object.assign(host, started);
    Object.assign(guest, (await api('/api/rooms/heartbeat', auth(guest)))[1]);
    const a = await wsConnect(host), b = await wsConnect(guest);
    function packet(role, nonce = 1n) {
      const data = Buffer.alloc(48); data.writeUInt32LE(0x504e484e); data[4] = 5; data[5] = role;
      data.writeBigUInt64LE(BigInt(host.sessionId), 8); data.writeBigUInt64LE(nonce, 16);
      data.writeBigUInt64LE(123n, 24);
      createHmac('sha256', Buffer.from(host.sessionKey, 'hex').subarray(16)).update(data.subarray(0, -16)).digest().copy(data, 32, 0, 16);
      return data;
    }
    let received = once(b, 'message'); const data = packet(1); a.send(data);
    assert.deepEqual((await received)[0], data);
    let count = 0; b.on('message', () => ++count);
    a.send(data); a.send(packet(2, 2n)); // replay and forged sender role
    await new Promise(r => setTimeout(r, 50)); assert.equal(count, 0);
    const corrupt = packet(1, 2n); corrupt[24] ^= 1;
    const closed = once(a, 'close'); a.send(corrupt); assert.equal((await closed)[0], 1008);
    b.terminate();
    assert.equal((await api('/api/rooms/leave', auth(guest)))[0], 200);
    assert.equal(lobby.rooms.size, 0);
    const [, pub] = await create(true);
    const list = (await api('/api/rooms?available=1'))[1].rooms;
    assert.equal(list.length, 1);
    for (const key of ['roomCode', 'authToken', 'sessionKey', 'sessionId']) assert.equal(key in list[0], false);
    time += 21000; lobby.sweep(); assert.equal(lobby.rooms.size, 0);
    assert.equal((await api('/api/rooms/heartbeat', auth(pub)))[0], 404);
  } finally { await lobby.close(); }
});
