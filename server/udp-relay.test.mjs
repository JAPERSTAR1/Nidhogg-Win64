import test from 'node:test';
import assert from 'node:assert/strict';
import dgram from 'node:dgram';
import { once } from 'node:events';
import { createHmac } from 'node:crypto';
import { createUdpRelay } from './udp-relay.mjs';

test('UDP address proof, authenticated forwarding, reordering, replay and session isolation', async () => {
  const room = { id: 'test', session: '123456789', key: 'ab'.repeat(32),
    host: {}, guest: {}, status: 'in_match' };
  const rooms = new Map([[room.id, room]]);
  const relay = createUdpRelay({ port: 0, bind: '127.0.0.1', rooms });
  await relay.ready;
  const clients = [dgram.createSocket('udp4'), dgram.createSocket('udp4')];
  for (const s of clients) { s.bind(0, '127.0.0.1'); await once(s, 'listening'); }
  const cookies = [Buffer.alloc(16), Buffer.alloc(16)];
  const receive = s => Promise.race([once(s, 'message').then(([b]) => b),
    new Promise((_, reject) => setTimeout(() => reject(new Error('UDP test timeout')), 1000).unref())]);
  function packet(role, serial, type = 5, sid = room.session) {
    const sealed = Buffer.alloc(48);
    sealed.writeUInt32LE(0x504e484e); sealed[4] = type; sealed[5] = role;
    sealed.writeBigUInt64LE(BigInt(sid), 8);
    sealed.writeBigUInt64LE(BigInt(serial) | (BigInt(role - 1) << 63n), 16);
    sealed.writeBigUInt64LE(BigInt(serial * 19), 24);
    createHmac('sha256', Buffer.from(room.key, 'hex').subarray(16)).update(sealed.subarray(0, 32)).digest().copy(sealed, 32, 0, 16);
    const header = Buffer.alloc(26); header[0] = 0x52;
    header.writeBigUInt64LE(BigInt(sid), 1); header[9] = role;
    cookies[role - 1].copy(header, 10);
    return Buffer.concat([header, sealed]);
  }
  const send = (role, b) => clients[role - 1].send(b, relay.address().port, '127.0.0.1');
  try {
    for (const role of [1, 2]) {
      let arrived = receive(clients[role - 1]); send(role, packet(role, 1, 1));
      const challenge = await arrived; assert.equal(challenge[0], 0x53);
      assert.equal(challenge.length, 26); cookies[role - 1] = challenge.subarray(10);
      arrived = receive(clients[role - 1]); send(role, packet(role, 1, 1));
      const ack = await arrived; assert.equal(ack[0], 0x55); assert.equal(ack.length, 34);
    }
    for (const serial of [2, 4, 3]) {
      const expected = packet(1, serial), arrived = receive(clients[1]);
      send(1, expected); assert.deepEqual(await arrived, expected.subarray(26));
    }
    const forwarded = relay.counters.forwarded;
    send(1, packet(1, 3)); // replay
    const forged = packet(1, 5); forged[forged.length - 1] ^= 1; send(1, forged);
    send(1, packet(1, 6, 5, '9999')); // nonexistent session
    await new Promise(resolve => setTimeout(resolve, 30));
    assert.equal(relay.counters.forwarded, forwarded);
    assert.ok(relay.counters.rejected >= 3);
    room.session = '4567'; send(1, packet(1, 7, 5, '123456789'));
    await new Promise(resolve => setTimeout(resolve, 30));
    assert.equal(relay.counters.forwarded, forwarded);
    assert.equal(relay.counters.registered, 2);
  } finally {
    for (const s of clients) s.close(); await relay.close();
  }
});
