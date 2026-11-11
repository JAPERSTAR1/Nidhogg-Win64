# Nidhogg Windows multiplayer

Work is confined to this secondary checkout. Checkpoint before changes: `993f8ba`
on `codex/internet-multiplayer`. No original CUSA00920 assets are distributed or
edited. The original executable SHA-256 remained
`5107c9b552458e3acd47701be0da118478294d6c551a8c3a639405e696d0570a`.

## Play

Build with `port/build.ps1`. `port/out/Nidhogg.exe` is the GUI application;
`port/out/nidhogg_host.exe` is the diagnostic console build. Both use the same
objects and original PS4 simulation. Select the game assets when prompted.

Open Online Multiplayer from the game menu, or press F1 and choose Online.
The online overlay supports public browsing, Quick Match, private six-character
codes, Ready, host Start, visibility and Leave. Each player uses their own
Player 1 keyboard/controller bindings; the guest becomes Orange/Player 2 online.

Set Online Settings > Lobby Server to the deployed HTTPS origin. Alternatively:

```powershell
.\start-game.ps1 -Endpoint 'http://127.0.0.1:27880'
```

`start-game.cmd` targets **https://lobby.japer.me**, which is NOT deployed yet.
Do not mistake that shortcut/configuration for an available production service.
The default client setting retains localhost for development. `--lobby-url`
overrides the saved endpoint without overwriting it. Endpoint URLs accept an
HTTP/HTTPS origin, no path, query or credentials. Localhost HTTP is for testing;
use HTTPS over the internet. No VPN, networking tools, Node or PowerShell are
required on player PCs.

Starting a match opens a clean guest process automatically, then closes the menu
process. This is necessary because the original menus retain actors, alarms and
random state. Both fresh guests exchange delayed inputs from frame zero; they
run the original menu/stage/match logic. Membership credentials pass through a
private child environment, never command-line arguments or public files. The
parent waits for the child, so test scripts can track its actual exit status.

## Architecture and limits

* Lobby: Node.js 20+ and one pinned dependency, `ws`; HTTP JSON on loopback 27880.
* Gameplay: original two-player simulation with frame-indexed input lockstep,
  negotiated host input delay (1-8 frames), 16-frame input history and retries.
  This is the port's input synchronization layer, **not recovered PSN rollback**.
* Direct transport: authenticated UDP to exchanged LAN/STUN IPv4 candidates.
  STUN binding discovery uses `stun.l.google.com:19302` on the gameplay socket;
  it can fail harmlessly. This is best-effort hole punching, not full ICE/TURN.
  Symmetric NAT/firewalls may prevent direct play. Players' candidate addresses
  are visible only to their authenticated opponent; LAN candidates are shared
  only when lobby requests originate from the same public address.
* Fallback: persistent binary WebSocket through the same HTTPS origin. It
  preserves sealed packets and session isolation. A stalled direct path sends
  backup packets via WebSocket; a dead direct path returns to relay. The server
  bounds messages, rates, queues, room count and request bodies; rejects forged
  membership, altered packets, cross-role packets and replayed relay nonces;
  expires absent members and closes matches on leave.
* Encryption: existing AES-128-CTR + truncated HMAC-SHA256 packet format, now
  with disjoint sender nonce spaces and receiver replay protection. This is
  room authentication, not an anti-cheat system; the service/operator receives
  room keys over HTTPS. Never expose the loopback HTTP origin directly.
* State checks compare room, native simulation PRNG, entity type, initialized
  base motion/sprite/flag fields and normalized disabled alarms. They exclude
  pointers/padding. They are useful divergence detection, **not serialization
  of every custom game variable**. Winner and room progression are checked
  separately in full-match tests. Detected divergence/disconnection stops online
  progression with an error/Leave UI; there is no reconnect/resume or rollback.
* A self-hosted server and tunnel must remain running. Turning off the operator
  PC makes matchmaking and relay unavailable. Free software/service does not
  eliminate electricity, existing internet/domain renewal costs or bandwidth.

Cloudflare Tunnel publishes HTTP/WebSocket, not public raw UDP. Its free public
hostname route needs no player-side cloudflared. A WebSocket relay uses TCP and
can suffer head-of-line blocking and long routes. The measured temporary tunnel
had ~250-360 ms RTT on this connection; that is too high for a good competitive
experience at the default delay. Prefer direct UDP; increase host delay to 8
frames if relay latency requires it, accepting the input delay. No latency or
NAT success guarantee is made for other networks.

## Operator setup

1. Install Node.js 20+ **on the server PC only**.
2. Double-click `start-server.cmd`. It installs the locked dependency on first
   run and serves loopback only. The console can be stopped with Ctrl+C.
3. Check `http://127.0.0.1:27880/health` (protocol `nidhogg-win64-2.0`).
4. Obtain Cloudflare's official Windows x64 cloudflared executable and put it
   in `.tools/cloudflared.exe` (operator-only, excluded from Git).

For an accountless **temporary** test:

```powershell
.\.tools\cloudflared.exe tunnel --url http://127.0.0.1:27880 --no-autoupdate
```

Use the printed `https://...trycloudflare.com` origin in both clients. This
address changes when the tunnel restarts and has no availability guarantee.
It does not configure `lobby.japer.me` or alter the website.

### Stable lobby.japer.me — requires user account/DNS action

Observed DNS on 2026-11-12: `japer.me` uses
`dns1.registrar-servers.com` / `dns2.registrar-servers.com`; `lobby.japer.me`
does not exist. **No DNS or external account changes have been made.**

The ordinary free Cloudflare custom-hostname Tunnel setup requires the domain
to be an active Cloudflare DNS zone. A CNAME-only/partial setup is not a free
workaround for a zone remaining at the existing registrar DNS. Moving the zone
nameservers is an external change and requires explicit user authorization.
Do not perform it merely to follow this guide.

After authorization, export the complete current zone first. Import and verify
all existing website, mail, verification and other records in Cloudflare before
changing nameservers at the registrar. Keep the GitHub Pages apex and `www`
records/targets and website contents unchanged; retain DNS-only routing for the
website during migration. The multiplayer service uses only `lobby` after that.
Save the old zone/nameservers for recovery. DNS migration still needs user review.

After the zone is authorized and active:

```powershell
.\.tools\cloudflared.exe tunnel login
.\.tools\cloudflared.exe tunnel create nidhogg-lobby
# External DNS mutation: run ONLY after explicit user approval:
.\.tools\cloudflared.exe tunnel route dns nidhogg-lobby lobby.japer.me
```

Copy `deploy/cloudflared.example.yml` to
`%LOCALAPPDATA%\NidhoggServer\config.yml`, fill in the returned UUID and the
credential file path in the operator's profile. Keep credentials outside the
checkout and never send them to players. Then:

```powershell
.\start-server.ps1 -TunnelConfig "$env:LOCALAPPDATA\NidhoggServer\config.yml"
```

The configuration routes only `lobby.japer.me` to loopback; unmatched hostnames
get 404. Cloudflare terminates HTTPS/WSS. Do not add an interactive Cloudflare
Access login to `/api` or `/relay`; the native game cannot complete a browser
login. Preserve WebSocket support and bypass caching for this hostname/API.
Re-check HTTPS health, room operations and a full game before distributing.

Optional automatic startup (operator account, after the named tunnel works):

```powershell
.\install-server-startup.ps1 -TunnelConfig "$env:LOCALAPPDATA\NidhoggServer\config.yml"
.\stop-server.ps1
# Remove startup later:
.\install-server-startup.ps1 -TunnelConfig "$env:LOCALAPPDATA\NidhoggServer\config.yml" -Remove
```

Startup uses a per-user sign-in shortcut and hidden operator processes. It has
not been installed during development. Process identity/start time are recorded
to avoid stopping an unrelated reused PID. The operator logs are under `.tools`.

## Validation

```powershell
.\port\build.ps1
.\start-server.ps1
.\test-multiplayer.ps1 -Combat
python tests/multiplayer_pair.py --direct --frames 1000 --label direct-check
python tests/multiplayer_pair.py --disconnect-frame 900 --frames 1200 --label disconnect-check
python tests/multiplayer_pair.py --endpoint 'https://YOUR-ENDPOINT' --delay 8 --combat --frames 2800 --label public-duel
```

The Python harness launches independent guest processes and isolated user/save
directories below `tests/results/`. It returns failure for crashes, divergence,
missing comparable state checks, or a requested combat run without matching
winner results. `NIDHOGG_NET_AUTO_EXIT=1` is test-only: it exits when a peer
disconnects; ordinary clients display the responsive error UI. Tests never write
to the original asset folder. `--latency` exercises the UDP simulator only;
it is not a simulated WebSocket RTT measurement.

**Two local processes through an internet HTTPS tunnel are still two local
processes.** A separate PC on another internet connection must repeat: host,
join by code/public list, ready, fight, throw, die/respawn, progress, complete,
compare winner/results, leave and close. That physical-network test remains
required, including NAT/firewall and practical latency assessment.

## Source evidence / architecture alternatives

* Original `Data/network/config.csv`: ranking/trophy labels, caching counts,
  session status and matchmaking/search timeouts; no lobby URL or gameplay port.
* Existing HLE refuses NpToolkit sample user requests with `0x80552c00`; known
  user/pad/time/CRT APIs are implemented, many remaining imports still return
  stub zero. Runtime reports Matching initialization and unavailable TUS/PSN.
  This work does not claim recovered SCE networking semantics or PSN access.
* Guest random helpers (`+0x22460`, `+0x21e10`) choose native LCG at `+0x1f0068`
  versus CRT rand for drawing. Original match-seed sites `+0x70c04/+0x70d97`
  store to `+0x3e3160` and `+0x70e6e` calls native srand. Offline Versus uses a
  different entry path, so the original `match start` event now applies the
  agreed seed before new match actors initialize. Offline behavior is untouched.
* Base object constructor `+0x234f0` and destructor `+0x236b0` prove pointer
  fields `+0x88/+0x90`, flags and initialized ranges. Old bulk hashing included
  heap addresses/padding and falsely printed later mismatches as `sync=OK`.
* Original sync cleanup (`+0x3a860/+0x3aa40`) names Doorways, Dropped/Flying
  Weapons, Frame, Man, Match, PickupContainers, RecentInputs, Room and Clouds.
  This is evidence of native synchronization structures, not sufficient proof
  to claim the original replication/rollback protocol has been recovered.
* [Cloudflare Tunnel protocol limits](https://developers.cloudflare.com/cloudflare-one/networks/connectors/cloudflare-tunnel/routing-to-tunnel/protocols/)
  and [WebSocket support](https://developers.cloudflare.com/cloudflare-one/faq/cloudflare-tunnels-faq/).
* [Cloudflare DNS setup modes](https://developers.cloudflare.com/dns/zone-setups/):
  free full-zone setup versus paid partial/CNAME and enterprise subdomain setup.
* [Durable Objects pricing](https://developers.cloudflare.com/durable-objects/platform/pricing/):
  Workers Free supports SQLite-backed Objects with daily compute/request/storage
  quotas: 100,000 requests/day, 13,000 GB-s/day; incoming WebSocket messages
  count at 20:1 for compute billing. SQLite storage allows 5 million rows read
  and 100,000 rows written/day, 5 GB total storage (verified 2026-11-12).
  A persistent high-rate game relay consumes those limits; a Workers/DO
  backend was evaluated but not implemented. Self-hosting avoids a metered relay
  bill and another backend rewrite, at the cost of operator-PC uptime.
* [ICE RFC 8445](https://www.rfc-editor.org/rfc/rfc8445.html) and
  [STUN RFC 8489](https://www.rfc-editor.org/rfc/rfc8489.html): STUN alone cannot
  guarantee reachability. WSS is this implementation's non-UDP fallback; there
  is no hidden paid TURN/VPS dependency.
