# Full game frontend — 2026-11-13

Run `start-game.cmd` or `port/out/Nidhogg.exe` from the authorized alpha working
copy, or Nidhogg.exe from the 1.1.0-rc.2 release ZIP. Older releases retain their
previous frontend.

Options and Multiplayer now replace the whole viewport. The compositor omits the
guest scene while either screen owns it. The runtime reads the original menu
font, logo and background texture from the selected game assets; no original
asset is modified or embedded in the executable. A blue scene, gold floor/title,
pixel text and translucent selection rows follow the original menu style.

Multiplayer offers Local Versus, Host Public Game, Host Private Game, Join With
Code, and a refreshing public-session list. Private rooms are excluded from that
list. Select a public session to join, or share a private room's six-character
code. The active room shows both players, privacy, code, map and host controls.
The host changes MAP with Left/Right or confirmation and starts once the second
player is connected. Neither player needs to press Ready. Both peers should use
this updated runtime.

Options keeps Graphics, Display, Audio, Controls, Network and HUD categories in
the same full-screen frontend. Tab/shoulders change settings categories;
arrows/D-pad select, Enter/A confirms and Escape/B returns. Mouse controls remain
available. Network includes player name, region, automatic/manual delay and the
service URL; HUD controls the optional connection details separately.

## Match startup

The host's Start keeps the process, HWND, D3D device and room transport alive.
The guest yields to a host fiber at an import boundary while holding no guest
mutex. Worker threads exit cooperatively at the same safe boundary. Once all
workers have stopped, the runtime closes guest files/audio/resources, replaces
the private guest heap, decommits guest direct memory, restores the initial
relocated image and runs a fresh guest fiber. A full frontend loading screen
covers deterministic menu navigation until the selected stage is created.
If a worker cannot quiesce within the bounded wait, startup fails without freeing
memory underneath that worker.

An authenticated boot agreement forwards the host's map using the existing
reserved payload field. Older relays forward this unchanged, and automatic
readiness satisfies older service start policies. Updated server source also
accepts a connected challenger without readiness and validates the chosen map.
No external service was redeployed for this change.

Waiting-room heartbeat responses can finish after Start. Once match credentials
are committed, those stale responses are ignored so they cannot restore the old
seed/session key during the in-process reset.

## Verification

Ignored evidence is in `tests/results/integrated-front-end-2026-11-13/` and
`tests/results/ui-performance-2026-11-13/`.

- `native-flow-2/report.json`: the host uses the original Multiplayer entry and
  game-pad input to create a private room, choose Wilds and start after the guest
  joins. Both peers retain their original PID and HWND, exit cleanly, and produce
  10 common matching simulation hashes without desync. Preserved frontend
  captures show one seeded public session and omit the seeded private session.
- `in-process-wilds-legacy/report.json`: an owned localhost service enforces the
  old manual-readiness policy and ignores the start-map field. Updated clients
  still start automatically in Wilds and produce six matching hashes.
- `in-process-prototype/report.json`: a full two-client match, win and rematch
  after the in-process reset, with 20 matching hashes.
- `in-process-final-combat/report.json`: final runtime, older service policy,
  different transition/flash settings on each peer, completed match and rematch,
  20 matching hashes, and unchanged PID/HWND on both clients.
- `offline-final/report.json`: GUI local-versus completion, Arcade entry, audio,
  captures and clean exit pass after the guest heap/fiber changes.
- Fullscreen Options captures are visually checked; Escape returns to the
  original menu. The same original art remains visible when the frontend closes.
- `npm --prefix server test`: lobby authorization, private/public isolation,
  host start without readiness, map validation and authenticated relay tests.

These are owned local tests, including actual native guest gameplay. They do not
prove connection quality between two independent internet networks. Gameplay
remains 60 Hz; the existing higher presentation-rate options are unchanged.
