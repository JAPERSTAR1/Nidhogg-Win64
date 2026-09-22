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
