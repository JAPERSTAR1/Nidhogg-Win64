# Azure multiplayer verification — 2026-11-12

## Scope and diagnosis

Tests use two independent native Windows guest processes on **one physical PC
and one Saudi internet connection**, routed out to the actual public Azure
endpoint. Original PS4 simulation and assets execute in both instances. This is
real public relay gameplay, but it does not establish connectivity or perceived
responsiveness between two physical PCs behind different NATs. The owner
confirmed a second PC is unavailable. No external-player result is fabricated.

The earlier home/Cloudflare route measured 122 ms one-way WSS forwarding and
223–275 ms in-game RTT. A fixed two-frame input buffer was far smaller than the
observed route delay. Native progress from frame 480 to 1560 took 52.7 seconds
(about 20.5 simulation FPS). A first baseline Join timed out; its retry completed
without desync but stalled badly. There was no evidence that a larger CPU alone
would solve this route/buffer mismatch.

The implementation also retried every 4 ms while stalled, had short HTTP
timeouts, and let asynchronous lobby replies overwrite negotiated delay. Peer
frame phases drifted despite sharing input indices. Changes address these paths:
Azure raw UDP relay, bounded retries, delay committed before frame zero, shared
future start, phase trim and less frequent in-match lobby polling. Original
gameplay/renderer/audio were retained. The port's lockstep layer is not recovered
PSN rollback. No claim is made to have recreated Sony APIs or original PSN peers.

## Region probes

Each probe sent 1,200 UDP echoes at 60 Hz over about 20 seconds, from this
operator connection. Jitter here is successive-RTT absolute difference.

| Region | RTT p50 | RTT p95 | RTT p99 | Jitter p95 | Loss |
| --- | ---: | ---: | ---: | ---: | ---: |
| UAE North | 28.82 ms | 29.82 ms | 30.82 ms | 1.96 ms | 0/1200 |
| France Central | 94.37 ms | 94.46 ms | 94.98 ms | 1.05 ms | 0/1200 |
| Italy North | 94.36 ms | 94.45 ms | 94.88 ms | 1.05 ms | 0/1200 |

UAE was selected for the measured route, not geographic proximity alone.
Saudi Arabia East was not available in this subscription's Compute region list.
Qatar had no sufficiently verified priced/deployable small candidate in the
investigation. France/Italy probes were deallocated and their owned resource
groups deleted. B-series sizes were subscription-restricted; the UAE inventory
had only FX2mds_v2/FX2ms_v2 unrestricted among x64 sizes with at most two vCPU.
The user supplied the running FX2mds_v2. Its cost is explicitly documented.

## Native gameplay results

`tests/multiplayer_pair.py` drives actual input. Combat exercises movement,
attacks, throwing, collision/death, respawns, room/round progression, victory,
results and rematch. Host and guest logs report the same winners/room sequence;
state checks cover initialized base state, not every custom variable.

| Run | Result | Delay | FPS, host/guest | Frame p50 | Frame p95, host/guest | Input capture→simulation p50 |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| Home WSS baseline | Matched state, severe stalls | 2 | ~20.5 in sampled interval | Not instrumented | Not instrumented | Not measured |
| Azure UDP, phase trim | Full match + rematch, 81 checks each, no desync | 3 | 55.14 / 55.14 | 16.67 ms | 22.31 / 23.34 ms | 50.03 / 50.04 ms |
| Azure UDP, injected loss/jitter | Full match + rematch, 81 checks each, no desync | 5 | 54.84 / 54.84 | 16.67–16.68 ms | 23.75 / 23.17 ms | 83.36 / 83.35 ms |
| Azure UDP, 12,000-frame soak | Five completed matches, 387 checks each, no desync | 3 | 53.06 / 53.07 | 16.67 ms | 25.77 / 26.43 ms | 50.03 / 50.04 ms |
| Azure forced WSS, final client | Full match + rematch, 81 checks each, no desync | 3 | 54.57 / 54.57 | 16.67 ms | 23.86 / 24.97 ms | 50.01 / 50.01 ms |

The impairment run configures **5% outgoing UDP loss plus up to 12 ms jitter
per client**, rather than claiming the physical network lost exactly 5%.
Delay negotiation increased to five frames; 16-frame input history/retries
recovered missing messages without changing outcomes. In-game echo RTT p50 was
about 100 ms under impairment and 66.7 ms in ordinary Azure runs. A game RTT
includes both relay legs and application scheduling, unlike a server echo.

In the short UDP comparison, input-wait p95 fell from 14.52/1.10 ms before phase
trim to 0/0 ms afterward. Frames waiting more than 1 ms fell from 1984/130 to
13/15. This is an observed pair of runs, not a controlled statistical guarantee.
Input latency p95 after trim was 70.97/68.62 ms; in the longer soak it was
81.14/80.08 ms (first 8192 samples retained). Input sampling excludes device,
display scanout and human perception. It is not a mouse-to-photon measurement.

Victory/end scenes remain CPU/rendering-heavy (a pre-change uncapped offline
trace already showed roughly 50 FPS there in one process). Running two native
renderers on one PC also affects whole-match averages. Median active frames
are near 16.67 ms; a locked 60 FPS throughout is **not verified**. The initial
baseline interval and full-run Azure FPS use different windows; they demonstrate
the stall reduction but should not be presented as an exact speedup percentage.

## Deployment and protections

Public `https://lobby.japer.me` passed system-DNS TLS hostname validation, health,
private create/join, ready/start and byte-exact binary WSS forwarding (29 ms).
These passed after stopping both the old home server/tunnel and the diagnostic
SSH forward. Native logs identify `UDPRelay` when forced through Azure.

Node authentication/room/isolation/expiry suite and UDP cookie/HMAC/reordering/
replay/session-rotation suite passed on Windows and Ubuntu. MSVC /W4 /WX timing
model checks cover independent clock origins, latency scales, asymmetric paths,
overflow/invalid timestamps and bounded phase convergence. The full existing
MSVC build succeeds with pre-existing warnings.

Only the lobby DNS record changed. Before/after snapshots show the other twelve
records unchanged. Website HTTP automation returned 403 before and after, so
website content cannot be certified here. No apex/www/mail settings were edited.
Public NSG rules allow TCP 80/443 and UDP 27881, with SSH restricted to the
operator IP. Temporary probe rules were removed and HTTP origin is loopback-only.

Killing the server with SIGKILL changed its PID from 8715 to 8730 and restored
health automatically. An actual Azure VM reboot at 14:39:20 UTC restored Node,
Caddy and all three monitoring timers automatically. Health passed with a new
uptime and 92 MB service memory; the root-only traffic counter survived reboot.
An initial native attempt launched during reboot could not create its room;
its failed logs were retained before retrying after public health recovered.
The ready-after-reboot retry passed 1500 frames at 60.00 FPS on both clients,
with matching hashes, zero frames waiting more than 1 ms and no desync.
Local direct UDP also passed 1400 frames at about 60 FPS with matching hashes;
an intentional guest departure stopped the host cleanly. The bounded offline
regression passed all 12 checks for local completion, arcade, renderer, audio
and screenshots. Package verification is reported separately with the release.
Budget configuration was
read back, but notification delivery and real deallocation were not triggered.
The conservative guard dry-run reported no stop under normal limits and a
required stop when its estimate threshold was deliberately set to zero. It
retained the VM because the test used `--dry-run`. Cost API failures are reported
honestly; finite-credit limits and exact shutdown dates are in AZURE-DEPLOYMENT.md.

## Remaining verification

* Two Windows PCs on separate internet connections completing a match: **BLOCKED**
  by unavailable second device/network. The release remains a candidate.
* Direct UDP across independent NATs: **IMPLEMENTED BUT NOT EXTERNALLY VERIFIED**.
  Relay success does not guarantee peer-to-peer hole punching on every NAT.
* Rollback, reconnect/resume, complete game-state serialization: **NOT IMPLEMENTED**.
* Low-latency lockstep over arbitrarily bad routes or sustained loss: no guarantee;
  automatic delay has a twelve-frame cap and timeouts stop rather than fake play.
* Whole-match fixed 60 FPS: **PARTIALLY WORKING**; heavier victory scenes still dip.

Local detailed logs/screenshots remain under ignored `tests/results/azure-*`.
The release includes sanitized aggregate evidence and package-test results;
neither credentials, original assets nor player saves are uploaded.
