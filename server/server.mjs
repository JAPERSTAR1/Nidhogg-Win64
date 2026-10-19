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
