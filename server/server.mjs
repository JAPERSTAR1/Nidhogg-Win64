import http from 'node:http';
import { randomBytes, createHmac, timingSafeEqual } from 'node:crypto';
import { WebSocketServer, WebSocket } from 'ws';
import { pathToFileURL } from 'node:url';
import { isIP } from 'node:net';

export const VERSION = 'nidhogg-win64-2.0';
const hex = n => randomBytes(n).toString('hex');
class ApiError extends Error { constructor(status, message) { super(message); this.status = status; } }
const requireThat = (condition, status, message) => { if (!condition) throw new ApiError(status, message); };
function name(value, fallback) {
  requireThat(value === undefined || typeof value === 'string', 400, 'Expected text');
  const s = (value ?? fallback).trim();
  requireThat(s.length >= 1 && s.length <= 48 && !/[\x00-\x1f{}"\\]/.test(s), 400, 'Text must contain 1-48 printable characters');
  return s;
}

export function createLobby({ ttlMs = 20000, maxRooms = 128, now = Date.now } = {}) {
  const rooms = new Map(), buckets = new Map();
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
      matchSeed: r.seed, inputDelayFrames: r.delay, relayTransport: 'websocket',
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
    if (method === 'GET' && url.pathname === '/health') return { ok: true, protocol: VERSION, transport: 'websocket', rooms: rooms.size };
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
          delay: Number.isInteger(b.inputDelayFrames) ? Math.max(1, Math.min(8, b.inputDelayFrames)) : 3,
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
  const server = http.createServer(async (req, res) => {
    try {
      const url = new URL(req.url, 'http://localhost');
