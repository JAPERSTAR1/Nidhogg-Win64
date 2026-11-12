import http from 'node:http';
import { randomBytes, createHmac, timingSafeEqual } from 'node:crypto';
import { WebSocketServer, WebSocket } from 'ws';
import { pathToFileURL } from 'node:url';
import { isIP } from 'node:net';
import { createUdpRelay } from './udp-relay.mjs';

export const VERSION = 'nidhogg-win64-2.1';
const hex = n => randomBytes(n).toString('hex');
class ApiError extends Error { constructor(status, message) { super(message); this.status = status; } }
const requireThat = (condition, status, message) => { if (!condition) throw new ApiError(status, message); };
function name(value, fallback) {
  requireThat(value === undefined || typeof value === 'string', 400, 'Expected text');
  const s = (value ?? fallback).trim();
  requireThat(s.length >= 1 && s.length <= 48 && !/[\x00-\x1f{}"\\]/.test(s), 400, 'Text must contain 1-48 printable characters');
  return s;
}

export function createLobby({ ttlMs = 20000, maxRooms = 128, now = Date.now,
  udpPort = null, udpBind = '0.0.0.0', relayPublicIp = '' } = {}) {
  const rooms = new Map(), buckets = new Map();
  const udp = udpPort === null ? null : createUdpRelay({ port: udpPort, bind: udpBind, rooms, now });
  const wss = new WebSocketServer({ noServer: true, maxPayload: 2048, perMessageDeflate: false });
  function endpoint(p, b) {
    for (const [ipKey, portKey] of [['publicIp', 'publicPort'], ['lanIp', 'lanPort']]) {
      if (isIP(b[ipKey]) === 4 && Number.isInteger(b[portKey]) && b[portKey] > 0 && b[portKey] <= 65535 &&
          !/^(0|224|225|226|227|228|229|23[0-9]|24[0-9]|25[0-5])\./.test(b[ipKey]) && b[ipKey] !== '255.255.255.255') {
        p[ipKey] = b[ipKey]; p[portKey] = b[portKey];
      }
    }
  }
  function rate(ip, cost = 1) {
    const t = now(); let b = buckets.get(ip);
    if (!b) { if (buckets.size >= 4096) throw new ApiError(503, 'Busy'); b = { tokens: 120, time: t }; buckets.set(ip, b); }
    b.tokens = Math.min(120, b.tokens + (t - b.time) * .02); b.time = t;
    requireThat(b.tokens >= cost, 429, 'Too many requests'); b.tokens -= cost;
  }
  function remove(room) {
    for (const p of [room.host, room.guest]) p?.ws?.close(1000, 'Room closed');
    rooms.delete(room.id);
  }
  function sweep() {
    for (const r of rooms.values()) {
      if (now() - r.host.seen > ttlMs) { remove(r); continue; }
      if (r.guest && now() - r.guest.seen > ttlMs) {
        if (r.status === 'in_match') { remove(r); continue; }
        r.guest.ws?.close(1000, 'Expired'); r.guest = null;
      }
    }
    for (const [ip, b] of buckets) if (now() - b.time > 60000) buckets.delete(ip);
  }
  function member(r, token) {
    if (r.host.token === token) return [r.host, 1];
    if (r.guest?.token === token) return [r.guest, 2];
    throw new ApiError(403, 'Invalid or expired membership');
  }
  function view(r, role, includeToken = false) {
    const p = role === 1 ? r.host : r.guest;
    const peer = role === 1 ? r.guest : r.host;
    return { roomId: r.id, roomName: r.name, roomCode: r.code, region: r.region,
      hostName: r.host.name, guestName: r.guest?.name ?? '', isPublic: r.public,
      guestConnected: role === 1 ? !!r.guest : true, playerCount: r.guest ? 2 : 1,
      maxPlayers: 2, hostReady: r.host.ready, guestReady: r.guest?.ready ?? false,
      status: r.status, stageIndex: r.stage, sessionId: r.session, sessionKey: r.key,
      matchSeed: r.seed, inputDelayFrames: r.delay, autoInputDelay: r.autoDelay,
      relayTransport: udp ? 'udp' : 'websocket', relayUdpPort: udp?.address()?.port ?? 0,
      relayPublicIp,
      peerPublicIp: peer?.publicIp ?? '', peerPublicPort: peer?.publicPort ?? 0,
      peerLanIp: p.source === peer?.source ? peer?.lanIp ?? '' : '',
      peerLanPort: p.source === peer?.source ? peer?.lanPort ?? 0 : 0,
      ...(includeToken ? { authToken: p.token } : {}) };
  }
  const player = n => ({ token: hex(32), name: n, ready: false, seen: now(), ws: null });
  function join(r, b, ip) {
    requireThat(r.status === 'waiting' && !r.guest, 409, 'Room is full or match started');
    r.guest = player(name(b.playerName, 'Fencer'));
    r.guest.source = ip;
    endpoint(r.guest, b);
    return view(r, 2, true);
  }
  function route(method, url, b, ip) {
    sweep(); rate(ip, url.pathname.endsWith('create') || url.pathname.endsWith('join_code') ? 10 : 1);
    if (method === 'GET' && url.pathname === '/health') return { ok: true, protocol: VERSION, transport: udp ? 'udp+wss' : 'websocket', rooms: rooms.size,
      uptimeSeconds: Math.floor(process.uptime()), udp: udp?.counters ?? null };
    if (method === 'GET' && url.pathname === '/api/rooms') {
      return { rooms: [...rooms.values()].filter(r => r.public &&
        (url.searchParams.get('available') !== '1' || (!r.guest && r.status === 'waiting')) &&
        (!url.searchParams.get('region') || url.searchParams.get('region') === r.region))
        .map(r => ({ roomId: r.id, roomName: r.name, region: r.region, playerCount: r.guest ? 2 : 1,
          maxPlayers: 2, status: r.status, stageIndex: r.stage })) };
    }
    requireThat(method === 'POST', 404, 'Unknown endpoint');
    switch (url.pathname) {
      case '/api/rooms/create': {
        requireThat(rooms.size < maxRooms, 503, 'Lobby is full');
        requireThat(Number.isInteger(b.stageIndex) && b.stageIndex >= 0 && b.stageIndex <= 3, 400, 'Invalid stage');
        requireThat(typeof b.isPublic === 'boolean', 400, 'Invalid visibility');
        let code; do { code = Array.from(randomBytes(6), x => 'ABCDEFGHJKLMNPQRSTUVWXYZ23456789'[x % 32]).join(''); }
        while ([...rooms.values()].some(r => r.code === code));
        const r = { id: hex(12), code, name: name(b.roomName, 'Arena'), region: name(b.region, 'all'),
          public: b.isPublic, stage: b.stageIndex,
          delay: Number.isInteger(b.inputDelayFrames) ? Math.max(1, Math.min(12, b.inputDelayFrames)) : 2,
          autoDelay: b.autoInputDelay !== false,
          host: player(name(b.hostName, 'Fencer')), guest: null,
          session: randomBytes(8).readBigUInt64LE().toString(), key: hex(32), seed: randomBytes(4).readUInt32LE() || 1, status: 'waiting' };
        r.host.source = ip; endpoint(r.host, b);
        rooms.set(r.id, r); return view(r, 1, true);
      }
      case '/api/rooms/join': {
        const r = rooms.get(b.roomId); requireThat(r?.public, 404, 'Public room not found'); return join(r, b, ip);
      }
      case '/api/rooms/join_code': {
        requireThat(typeof b.roomCode === 'string' && /^[A-Z2-9]{6}$/.test(b.roomCode), 400, 'Invalid room code');
        const r = [...rooms.values()].find(r => r.code === b.roomCode);
        requireThat(r, 404, 'Invalid or expired room code'); return join(r, b, ip);
      }
      case '/api/quickmatch': {
        const r = [...rooms.values()].find(r => r.public && !r.guest && r.status === 'waiting' && (!b.region || b.region === 'all' || b.region === r.region));
        requireThat(r, 404, 'No open rooms'); return join(r, b, ip);
      }
    }
    const r = rooms.get(b.roomId); requireThat(r, 404, 'Room expired or closed');
    const [p, role] = member(r, b.authToken); p.seen = now();
    switch (url.pathname) {
      case '/api/rooms/heartbeat': endpoint(p, b); break;
      case '/api/rooms/ready':
        requireThat(r.status === 'waiting' && typeof b.ready === 'boolean', 409, 'Cannot change readiness'); p.ready = b.ready; break;
      case '/api/rooms/visibility':
        requireThat(role === 1, 403, 'Host only'); requireThat(typeof b.isPublic === 'boolean', 400, 'Invalid visibility'); r.public = b.isPublic; break;
      case '/api/rooms/kick':
        requireThat(role === 1 && r.status === 'waiting', 403, 'Host only, before start'); r.guest?.ws?.close(1000, 'Removed'); r.guest = null; break;
      case '/api/rooms/start':
        requireThat(role === 1, 403, 'Host only');
        requireThat(r.guest && r.host.ready && r.guest.ready, 409, 'Both players must be ready');
        // A previously removed guest must not retain a key usable in a new match.
        if (r.status === 'waiting') {
          r.session = randomBytes(8).readBigUInt64LE().toString();
          r.key = hex(32); r.seed = randomBytes(4).readUInt32LE() || 1;
        }
        r.status = 'in_match'; break;
      case '/api/rooms/leave':
        if (role === 1 || r.status === 'in_match') remove(r);
        else { r.guest.ws?.close(1000, 'Left'); r.guest = null; }
        return { ok: true };
      default: throw new ApiError(404, 'Unknown endpoint');
    }
    return view(r, role);
  }
  function requestIp(req) {
    const local = ['127.0.0.1', '::1', '::ffff:127.0.0.1'].includes(req.socket.remoteAddress);
    const forwarded = process.env.TRUST_PROXY === 'caddy' ? req.headers['x-real-ip'] : req.headers['cf-connecting-ip'];
    return local && isIP(forwarded) ? forwarded : req.socket.remoteAddress;
  }
  const server = http.createServer(async (req, res) => {
    try {
      const url = new URL(req.url, 'http://localhost');
      if (url.pathname !== '/health') requireThat(req.headers['x-nidhogg-version'] === VERSION, 426, 'Client/server protocol mismatch; update the Windows client');
      let size = 0, chunks = [];
      for await (const chunk of req) { size += chunk.length; requireThat(size <= 4096, 413, 'Request too large'); chunks.push(chunk); }
      let b = {};
      try { b = size ? JSON.parse(Buffer.concat(chunks).toString('utf8')) : {}; } catch { throw new ApiError(400, 'Invalid JSON'); }
      requireThat(b && typeof b === 'object' && !Array.isArray(b), 400, 'Expected object');
      // Forwarded addresses are accepted only from the local trusted proxy.
      const result = route(req.method, url, b, requestIp(req));
      res.writeHead(200, { 'Content-Type': 'application/json', 'Cache-Control': 'no-store' }); res.end(JSON.stringify(result));
    } catch (e) {
      res.writeHead(e.status ?? 500, { 'Content-Type': 'application/json', 'Cache-Control': 'no-store' });
      res.end(JSON.stringify({ error: e.status ? e.message : 'Internal server error' }));
    }
  });
  server.requestTimeout = 5000; server.headersTimeout = 5000; server.maxConnections = 512;
  server.on('upgrade', (req, socket, head) => {
    try {
      sweep(); rate(requestIp(req), 5);
      requireThat(req.headers['x-nidhogg-version'] === VERSION, 426, 'Protocol mismatch');
      const url = new URL(req.url, 'http://localhost');
      requireThat(url.pathname === '/relay', 404, 'Unknown endpoint');
      const r = rooms.get(url.searchParams.get('roomId')); requireThat(r, 404, 'Room closed');
      const [p, role] = member(r, req.headers['x-nidhogg-token']);
      wss.handleUpgrade(req, socket, head, ws => {
        p.ws?.close(1000, 'Replaced'); p.ws = ws; ws.isAlive = true;
        let count = 0, window = now(), lastNonce = -1n;
        ws.on('pong', () => { ws.isAlive = true; });
        ws.on('error', () => {});
        ws.on('close', () => { if (p.ws === ws) p.ws = null; });
        ws.on('message', (data, binary) => {
          if (now() - window >= 1000) { count = 0; window = now(); }
          if (!binary || ++count > 600 || data.length < 40 || data.length > 2048 || !rooms.has(r.id)) { ws.close(1008, 'Invalid packet'); return; }
          if (data.readUInt32LE(0) !== 0x504e484e || data[5] !== role || data.readBigUInt64LE(8).toString() !== r.session) return;
          const mac = createHmac('sha256', Buffer.from(r.key, 'hex').subarray(16)).update(data.subarray(0, -16)).digest().subarray(0, 16);
          if (!timingSafeEqual(mac, data.subarray(-16))) { ws.close(1008, 'Authentication failed'); return; }
          const nonce = data.readBigUInt64LE(16);
          if (nonce <= lastNonce) return; lastNonce = nonce; p.seen = now();
          const peer = role === 1 ? r.guest?.ws : r.host.ws;
          if (peer?.readyState === WebSocket.OPEN) {
            if (peer.bufferedAmount > 64 * 1024) { peer.close(1013, 'Slow receiver'); return; }
            peer.send(data, { binary: true });
          }
        });
      });
    } catch (e) { socket.end(`HTTP/1.1 ${e.status ?? 400} Rejected\r\nConnection: close\r\n\r\n`); }
  });
  const timer = setInterval(() => {
    sweep();
    for (const ws of wss.clients) { if (!ws.isAlive) ws.terminate(); else { ws.isAlive = false; ws.ping(); } }
  }, 5000); timer.unref();
  return { server, rooms, sweep, udp, udpReady: udp?.ready ?? Promise.resolve(), close: async () => {
    clearInterval(timer); for (const ws of wss.clients) ws.terminate();
    server.closeAllConnections(); await new Promise(resolve => server.close(resolve));
    if (udp) await udp.close();
  } };
}
if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  const lobby = createLobby({ udpPort: process.env.UDP_PORT ? Number(process.env.UDP_PORT) : null,
    relayPublicIp: process.env.RELAY_PUBLIC_IP ?? '' });
  await lobby.udpReady;
  lobby.server.listen(Number(process.env.PORT ?? 27880), process.env.BIND ?? '127.0.0.1', () => console.log(`Nidhogg lobby listening on ${JSON.stringify(lobby.server.address())}`));
  for (const signal of ['SIGINT', 'SIGTERM']) process.on(signal, async () => { await lobby.close(); process.exit(0); });
}
