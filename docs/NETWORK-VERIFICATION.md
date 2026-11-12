# Verification record — 2026-11-12

The v1.0.0 offline/local release has a separate [packaged-binary verification
record](RELEASE-VERIFICATION.md). Its release classification does not certify the
experimental online path or change the independent-network limitation below.

## Repository reconstruction validation

Before reconstructing the source history on 2026-11-12, the final secondary
working tree was rebuilt successfully. Backend tests passed; embedded original
PNG and all seven icon sizes passed resource verification. The offline harness
again passed 12/12 checks. `history-release-validation/report.json` records a new
2800-frame two-process localhost relay run: both exit 0, both Yellow winner,
matching Castle progression through results/rematch, 20 matching state checks
per client and no detected divergence/timeouts. These are new local tests, not
additional independent-network verification.

The asset-free ZIP now also includes LibAtrac9's MIT notice and third-party
notices. The GitHub workflow is manual and has not been executed on GitHub.
See [HISTORY.md](HISTORY.md) for the explicitly synthetic author-date schedule;
that schedule is not evidence of past testing or development activity.

This is a secondary checkout, branch `codex/internet-multiplayer`. Pre-change
checkpoint: `993f8ba`. No original game data or other checkout was edited.

| Subsystem | Status | Evidence and precise scope |
| --- | --- | --- |
| Windows build | VERIFIED WORKING | `port/build.ps1` exits 0; `port/out/Nidhogg.exe` GUI subsystem 2 and `nidhogg_host.exe` console subsystem 3. Existing MSVC warnings remain. |
| Lobby / room backend | VERIFIED WORKING | `node --test server/test.mjs` passes room creation/discovery/privacy, readiness/start, authorization, HMAC, sender-role checks, replay rejection, expiration and cleanup. `Test-NetConnection 127.0.0.1 -Port 27880` succeeded. |
| Two-process local game | VERIFIED WORKING | `tests/results/final-local-duel/report.json`: both exit 0, both Yellow winner, Castle Center -> RightA -> RightB -> RightEnd -> MatchStats -> rematch; no detected divergence. |
| Public HTTPS/WSS game path | VERIFIED WORKING | `tests/results/verified-public-match/report.json` and each client's `logs/nidhogg.log`: two original guests routed through a real accountless Cloudflare public HTTPS tunnel, forced relay, host delay 8; full match, same Yellow winner, same room progression; **81 checks matched per client, no desync**, 2800 network frames, both exit 0. This remains a same-PC/same-network test through a public service. |
| Internet latency | PARTIALLY WORKING | Forced relay RTT ~250 ms on the first tunnel, ~350-360 ms on the later route. Synchronized play works but this route is unsuitable for a strong competitive experience. Direct transport is preferred. No assertion of acceptable latency between separate networks. |
| UDP/STUN direct transport | IMPLEMENTED BUT NOT EXTERNALLY VERIFIED | `tests/results/direct-stun/report.json` and final `tests/results/final-direct-smoke/report.json`: public STUN candidate obtained and DirectP2P selected, matching state checks, no desync, ~1-18 ms local measured RTT including frame scheduling. Hole punching across distinct NATs was not tested. Symmetric NAT can fall back to WSS. |
| Disconnect behavior | VERIFIED WORKING | `tests/results/disconnect-cleanup/report.json`: guest exits at frame 900; host records `match stopped`, exits via test-only auto-exit, both exit 0, no divergence. Normal clients instead display the error/Leave interface and pause progression. |
| Offline rendering/audio/local versus/Arcade | VERIFIED WORKING | `tests/results/offline-network-regression/report.json`: 12/12 checks pass: clean GUI exits, render logs with zero GNM errors, active audio mixing, screenshots, offline full-match victory/results, Arcade progress entry. This does not prove all resolutions, devices, stages or save-corruption cases. |
| Host/Join UI changes | PARTIALLY WORKING | Built native overlay and original-menu hook; network actions now run off the rendering thread. Windows UI capture failed twice (`FrameArrived timed out`, `window capture timed out`), so the actual button flow was not certified by clicking. CLI integration exercises the same networking APIs. No security/firewall dialogs were automated. |
| Operator scripts | VERIFIED WORKING | Managed server launched successfully on isolated port 27882 and `stop-server.ps1` stopped its owned process. Production server and named tunnel are now running under the managed launcher. A startup shortcut is installed; a complete next-logon test is pending. |
| Automatic named-tunnel startup | IMPLEMENTED BUT NOT EXTERNALLY VERIFIED | Per-user startup shortcut installed with the authorized named tunnel configuration. Current managed launch works; actual logoff/logon verification remains pending. |
| lobby.japer.me production deployment | PARTIALLY WORKING | User completed authorization and moved nameservers to Cloudflare. Named `nidhogg-lobby` tunnel is connected, and only the lobby CNAME was added. HTTPS health, private room/create/join/readiness/start and binary WSS relay pass with real TLS hostname validation using a process-only pinned public DNS answer. The local ISP resolvers still return the old delegation/NXDOMAIN, so the normal-resolver native game test on this hostname remains pending. |
| Separate PCs / separate internet links | BLOCKED | A second physical Windows PC and independent internet connection were unavailable. Public-tunnel testing does not replace this test. |
| Original PSN protocol / rollback / reconnect | NOT IMPLEMENTED | Existing incomplete SCE networking emulation remains. This implementation uses original local game logic with synchronized input, not an invented PSN implementation. It has no reconnect/resume or rollback. |

Later failed attempts are retained, not reported as success:

* `public-full-duel`: HTTP 530 from an unavailable earlier temporary tunnel;
  neither client joined a shared network match.
* `public-full-v2`: initial lobby request timed out before the new temporary
  endpoint was usable; no network match.
* `public-full-v3`: state synchronization succeeded, but the old absolute-frame
  QA control sequence did not complete the match at delay 8. The harness was
  corrected to schedule real control input relative to the native match-start
  event and negotiated delay. `verified-public-match` then passed the full run.
* Early `state-probe`/`boot-probe`/`rng-split` runs exposed genuine divergence
  and hashing defects; their failures led to the fixes described in
  [MULTIPLAYER.md](MULTIPLAYER.md). They are not release verification.

State checks deliberately exclude pointer values and padding and do not cover
every derived-class field; agreement plus same winner/progression is evidence,
not a mathematical proof of complete deterministic equivalence.

Original executable SHA-256 before and after work:
`5107c9b552458e3acd47701be0da118478294d6c551a8c3a639405e696d0570a`.

For startup/deployment, limitations and public documentation sources see
[MULTIPLAYER.md](MULTIPLAYER.md). Local logs/results are ignored by Git but
remain in this checkout. Release packaging excludes all original game assets,
operator credentials, Node/cloudflared and development logs.

Public DNS/website baselines for the authorized Cloudflare preparation are saved
under `.tools/*before-cloudflare*.json`, with a draft observed-record review in
`.tools/cloudflare-dns-review.json`. Both website URLs returned HTTP 200 and the
same body digest. Public queries also found MX/TXT email records: preserve those
and any unenumerated DKIM/other records from the complete registrar export.
These snapshots are read-only evidence, not a complete zone backup or permission
to apply the draft.

## Permanent deployment follow-up

The user subsequently completed cloudflared authorization. Tunnel UUID
`359b2724-95ef-4271-8ef3-bb04d724812e` was created and connected with multiple
Cloudflare edge connections. Operator config is
`%LOCALAPPDATA%\NidhoggServer\config.yml`; credentials remain in the operator's
`.cloudflared` profile, outside Git and the player package.

The only DNS mutation by this work was adding the proxied `lobby.japer.me`
CNAME to the tunnel. `.tools/cloudflare-records-before-lobby.json` and
`after-lobby.json` prove all 12 existing records retained their content, IDs,
TTL, proxy status and priorities. No website/mail records were changed. Both
apex and www still returned HTTP 200 with the original HTML digest.

`node server/probe-deployment.mjs https://lobby.japer.me --resolve 104.21.17.51`
passed HTTPS health, private room creation/join, readiness/start and byte-exact
binary WebSocket forwarding (121 ms one-way forwarding in that probe). This
diagnostic preserves TLS SNI/certificate validation for `lobby.japer.me` and
pins a DNS answer only inside the probe process; it does not modify OS DNS.
It is a transport/API probe, not another completed native game match.

Google DNS and Cloudflare authoritative servers return the new lobby addresses.
The two local ISP DNS servers still return registrar-servers.com and fail to
resolve the lobby. Clearing the Windows DNS cache did not fix the upstream
cache. No hosts-file, DNS-server, VPN or firewall settings were changed.
Ordinary-resolver game verification on the permanent hostname awaits DNS
propagation. The prior full native match through a public temporary tunnel
remains valid evidence for the gameplay transport, with its stated scope.

Managed server/tunnel are running. A per-user `Nidhogg Lobby.lnk` startup
shortcut is installed; its next-logon behavior has not been tested by logging
the user out. Use `stop-server.ps1` for controlled shutdown.
