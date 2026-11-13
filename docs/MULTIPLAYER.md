# Nidhogg Windows multiplayer

Both players need the updated 1.1.0-rc.2 client (protocol `nidhogg-win64-2.1`).
Run Nidhogg.exe, select your CUSA00920 assets, then choose Multiplayer.
Select a public session, Host Public/Private Game, or Join With Code. Private
rooms have six-character codes. The host chooses MAP in the room and starts once
the second player joins, without a manual Ready step. Each player uses their own
Player 1 controls. Options/F1 opens settings; F3 toggles the optional connection HUD.
Leave input delay on Auto; arrows select the minimum delay (1–12 frames).

The default endpoint is **https://lobby.japer.me**. Matchmaking and relay run on
Azure UAE North, independently of the operator PC. No VPN, port forwarding,
Node.js, cloudflared or terminal is required on player PCs. Offline/local play
works without the service. The current promotional-credit deployment has an
automatic spending stop; see [deployment](AZURE-DEPLOYMENT.md).

## Transport and simulation

The original PS4 x86-64 game logic still executes through the existing Windows
runtime. The port synchronizes frame-indexed inputs; it does not recover or
claim to implement the original PSN matchmaking/netcode or rollback.

* HTTPS JSON handles registration, rooms, readiness, heartbeats and cleanup.
  Caddy terminates TLS and proxies to Node on loopback TCP 27880.
* Best-effort direct authenticated UDP uses LAN/STUN IPv4 candidates. This is
  hole punching, not full ICE/TURN; symmetric NAT can prevent direct play.
* Azure UDP relay on 27881 avoids the former home/Cloudflare gameplay route.
  Address-proof cookies prevent reflection; session/role HMAC, replay windows,
  packet limits and rate limits isolate sessions. Raw UDP bypasses Cloudflare.
* If UDP is blocked or becomes unhealthy, binary WSS at the HTTPS origin carries
  the same authenticated packets. TCP head-of-line blocking remains possible.
* Inputs include 16 frames of redundant history. Retries are bounded to 50 Hz.
  Neither transport predicts, skips or changes simulation frames after loss.
* Before frame zero, both peers negotiate one delay using RTT/jitter samples.
  A shared future start accounts for independent monotonic clock origins.
  A filtered, bounded ±500µs wall-clock pacing correction keeps input sampling
  aligned; guest time, frame indices and PRNG remain deterministic.
* An in-process guest reset and a fresh fiber eliminate menu-state differences
  while preserving the application PID, window and room connection. Workers stop
  before guest memory is reset. See [frontend startup](NATIVE-FRONTEND.md).
* State hashes detect divergence using initialized entity/base motion/sprite/
  flag fields, room and PRNG. They do not serialize every custom game variable.
  Tests also compare winners/results/rematches. Detected desync or disconnection
  stops play with an error/Leave UI. Reconnect/resume and rollback are absent.

AES-128-CTR plus truncated HMAC-SHA256 uses disjoint role nonce spaces. The
operator receives session keys; this is room authentication, not anti-cheat.
Opponent candidates are shared only with authenticated room members. LAN
candidates are shared only for members observed at the same public address.

## Development and diagnostics

```powershell
npm ci --prefix server
npm test --prefix server
.\start-server.ps1
.\start-game.ps1 -Endpoint http://127.0.0.1:27880 -Assets D:\Games\CUSA00920
python tests/multiplayer_pair.py --assets D:\Games\CUSA00920 --endpoint https://lobby.japer.me --combat --frames 2800 --delay 2 --label azure-duel
```

The pair harness starts two independent native game processes with isolated
profiles. `--loss 5 --jitter 12` injects outgoing UDP impairments in each client;
`--wss` forces the backup path; `--direct` permits direct candidates. These
environment controls are diagnostics, not normal player setup. They do not
simulate independent physical networks. Logs contain aggregate frame/wait/RTT
and input-capture-to-simulation percentiles. Rendering, device input and display
scanout add latency beyond that last measurement.

See [measured results and limitations](AZURE-VERIFICATION.md). The historical
Cloudflare tests remain in NETWORK-VERIFICATION.md and are superseded for the
current deployment. Cloudflare Tunnel exposes HTTP/WSS, not arbitrary UDP.
