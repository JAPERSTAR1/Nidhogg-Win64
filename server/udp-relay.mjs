import dgram from 'node:dgram';
import { createHmac, randomBytes, timingSafeEqual } from 'node:crypto';
import { once } from 'node:events';

// UDP carries the same authenticated ciphertext as WSS. A stateless return-route
// cookie proves address ownership before binding a room member to a UDP endpoint.
// No response is larger than its request; unknown sessions receive no response.
export function createUdpRelay({ port, bind = '0.0.0.0', rooms, now = Date.now }) {
  const socket = dgram.createSocket('udp4'), secret = randomBytes(32);
  const counters = { received: 0, forwarded: 0, rejected: 0, challenges: 0, registered: 0, rateDropped: 0 };
  const sessions = new Map(), rates = new Map();
  let globalTime = now(), globalCount = 0;
  const sameAddress = (a, b) => a?.address === b.address && a?.port === b.port;
  function cookie(session, role, peer, epoch) {
    return createHmac('sha256', secret).update(`${session}/${role}/${peer.address}/${peer.port}/${epoch}`).digest().subarray(0, 16);
  }
  function roomFor(session) {
    let r = sessions.get(session);
    if (r && rooms.get(r.id) === r && r.session === session) return r;
    sessions.delete(session);
    for (const candidate of rooms.values()) if (candidate.session === session) {
      sessions.set(session, candidate); return candidate;
    }
  }
  function rate(peer, roleState) {
    const t = now();
    if (roleState) {
      if (t - (roleState.time ?? 0) >= 1000) { roleState.time = t; roleState.count = 0; }
      return ++roleState.count <= 240;
    }
    if (t - globalTime >= 1000) { globalTime = t; globalCount = 0; }
    if (++globalCount > 4000) return false;
    let item = rates.get(peer.address);
    if (!item) {
      if (rates.size >= 4096) return false;
      rates.set(peer.address, item = { time: t, count: 0 });
    }
    if (t - item.time >= 1000) { item.time = t; item.count = 0; }
    if (++item.count > 1000) return false;
    return true;
  }
  socket.on('message', (packet, from) => {
    counters.received++;
    try {
      if (packet.length < 26 + 40 || packet.length > 512 || packet[0] !== 0x52 || !rate(from)) { counters.rateDropped++; return; }
      const session = packet.readBigUInt64LE(1).toString(), role = packet[9];
      const room = roomFor(session);
      if (!room || ![1, 2].includes(role) || room.status !== 'in_match') { counters.rejected++; return; }
      const member = role === 1 ? room.host : room.guest;
      if (!member) { counters.rejected++; return; }
      const sealed = packet.subarray(26);
      if (sealed.readUInt32LE(0) !== 0x504e484e || sealed[5] !== role || sealed.readBigUInt64LE(8).toString() !== session || ![1, 2, 3, 4, 5, 6, 7].includes(sealed[4])) { counters.rejected++; return; }
      const key = Buffer.from(room.key, 'hex');
      const mac = createHmac('sha256', key.subarray(16)).update(sealed.subarray(0, -16)).digest().subarray(0, 16);
      if (!timingSafeEqual(mac, sealed.subarray(-16))) { counters.rejected++; return; }
      const nonce = sealed.readBigUInt64LE(16);
      if ((nonce >> 63n) !== BigInt(role - 1)) { counters.rejected++; return; }
      const supplied = packet.subarray(10, 26), epoch = Math.floor(now() / 300000);
      const known = member.udp && sameAddress(member.udp, from) && timingSafeEqual(supplied, member.udp.cookie);
      if (!known && ![epoch, epoch - 1].some(e => timingSafeEqual(supplied, cookie(session, role, from, e)))) {
        const challenge = Buffer.alloc(26); challenge[0] = 0x53;
        challenge.writeBigUInt64LE(BigInt(session), 1); challenge[9] = role;
        cookie(session, role, from, epoch).copy(challenge, 10);
        socket.send(challenge, from.port, from.address); counters.challenges++; return;
      }
      const state = member.udpState ??= { high: -1n, mask: 0n, time: now(), count: 0 };
      if (!rate(from, state)) { counters.rateDropped++; return; }
      if (nonce > state.high) {
        const shift = nonce - state.high;
        state.mask = shift >= 128n ? 1n : ((state.mask << shift) | 1n) & ((1n << 128n) - 1n);
        state.high = nonce;
      } else {
        const age = state.high - nonce;
        if (age >= 128n || (state.mask & (1n << age))) { counters.rejected++; return; }
        state.mask |= 1n << age;
      }
      if (!known) counters.registered++;
      member.udp = { address: from.address, port: from.port, cookie: Buffer.from(supplied) };
      member.seen = now();
      if (sealed[4] === 1) {
        const ack = Buffer.alloc(34); ack[0] = 0x55;
        ack.writeBigUInt64LE(BigInt(session), 1); ack[9] = role;
        supplied.copy(ack, 10); ack.writeBigUInt64LE(nonce, 26);
        socket.send(ack, from.port, from.address); return;
      }
      const peer = role === 1 ? room.guest : room.host;
      if (peer?.udp) { socket.send(sealed, peer.udp.port, peer.udp.address); counters.forwarded++; }
    } catch { counters.rejected++; }
  });
  socket.on('error', e => console.error('UDP relay error:', e.code));
  socket.bind(port, bind);
  const ready = once(socket, 'listening');
  const timer = setInterval(() => {
    for (const [session, r] of sessions) if (rooms.get(r.id) !== r || r.session !== session) sessions.delete(session);
    for (const [ip, item] of rates) if (now() - item.time > 60000) rates.delete(ip);
  }, 10000); timer.unref();
  return { ready, address: () => { try { return socket.address(); } catch { return null; } }, counters, close: async () => {
    clearInterval(timer); await ready; await new Promise(resolve => socket.close(resolve));
  } };
}
