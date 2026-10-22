// Operator diagnostic. --resolve pins a public DNS answer for this process only;
// TLS SNI/certificate validation still uses the real hostname. No OS DNS changes.
import https from 'node:https';
import http from 'node:http';
import { createHmac } from 'node:crypto';
import { once } from 'node:events';
import { performance } from 'node:perf_hooks';
import assert from 'node:assert/strict';
import { WebSocket } from 'ws';
import { VERSION } from './server.mjs';

const endpoint = process.argv[2] ?? 'https://lobby.japer.me';
const pinned = process.argv[3] === '--resolve' ? process.argv[4] : null;
const lookup = pinned ? (host, options, done) => {
  if (options?.all) done(null, [{ address: pinned, family: 4 }]);
  else done(null, pinned, 4);
} : undefined;
async function api(path, body) {
  return new Promise((resolve, reject) => {
    const url = new URL(path, endpoint);
    const request = (url.protocol === 'https:' ? https : http).request(url, {
      lookup, method: body ? 'POST' : 'GET', headers: {
        'Content-Type': 'application/json', 'X-Nidhogg-Version': VERSION,
      }, timeout: 10000,
    }, response => {
      let data = '';
      response.on('data', chunk => { data += chunk; if (data.length > 1024 * 1024) request.destroy(new Error('Oversized response')); });
      response.on('end', () => {
        try { assert.equal(response.statusCode, 200, `HTTP ${response.statusCode}`); resolve(JSON.parse(data)); }
        catch (error) { reject(error); }
      });
    });
    request.on('error', reject); request.on('timeout', () => request.destroy(new Error('Request timeout')));
    request.end(body ? JSON.stringify(body) : undefined);
  });
}
let host, guest; const sockets = [];
const auth = p => ({ roomId: p.roomId, authToken: p.authToken });
try {
  const health = await api('/health'); assert.equal(health.protocol, VERSION);
  host = await api('/api/rooms/create', { roomName: 'Deployment check', hostName: 'Probe host',
    region: 'all', stageIndex: 0, isPublic: false, inputDelayFrames: 3 });
  guest = await api('/api/rooms/join_code', { roomCode: host.roomCode, playerName: 'Probe guest' });
  for (const p of [host, guest]) await api('/api/rooms/ready', { ...auth(p), ready: true });
  Object.assign(host, await api('/api/rooms/start', auth(host)));
  Object.assign(guest, await api('/api/rooms/heartbeat', auth(guest)));
  for (const p of [host, guest]) {
    const url = new URL('/relay?roomId=' + p.roomId, endpoint); url.protocol = url.protocol === 'https:' ? 'wss:' : 'ws:';
    const ws = new WebSocket(url, { lookup, handshakeTimeout: 10000,
      headers: { 'X-Nidhogg-Version': VERSION, 'X-Nidhogg-Token': p.authToken } });
    sockets.push(ws); await once(ws, 'open');
  }
  const packet = Buffer.alloc(48); packet.writeUInt32LE(0x504e484e);
  packet[4] = 5; packet[5] = 1; packet.writeBigUInt64LE(BigInt(host.sessionId), 8);
  packet.writeBigUInt64LE(1n, 16); packet.writeBigUInt64LE(123n, 24);
  createHmac('sha256', Buffer.from(host.sessionKey, 'hex').subarray(16)).update(packet.subarray(0, 32)).digest().copy(packet, 32, 0, 16);
  const arrival = once(sockets[1], 'message');
  const start = performance.now(); sockets[0].send(packet);
  const received = await Promise.race([arrival, new Promise((_, reject) => setTimeout(() => reject(new Error('Relay timeout')), 10000).unref())]);
  assert.deepEqual(received[0], packet);
  console.log(JSON.stringify({ endpoint, tlsHostnameValidated: endpoint.startsWith('https://'), resolver: pinned ? 'pinned public DNS answer; diagnostic only' : 'system DNS',
    health: 'PASS', privateRoom: 'PASS', readyStart: 'PASS', binaryWebSocketRelay: 'PASS', forwardMs: Math.round(performance.now() - start) }));
} finally {
  for (const socket of sockets) socket.terminate();
  if (host) await api('/api/rooms/leave', auth(host)).catch(() => {});
}
