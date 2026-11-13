# Nidhogg Win64 Port

Experimental native Windows x64 compatibility runtime for the PS4 version of
**Nidhogg (CUSA00920)**. It runs the original x86-64 game code, translates PS4
graphics commands to Direct3D 11, and provides Windows audio, input and storage.

This is a community engineering project, unaffiliated with the game's creators
or Sony. Original game executables and assets are not included in this repository
or its asset-free runtime packages. Supply your own CUSA00920 files.

**Online play is experimental.** The 1.1 candidate uses Azure UAE North UDP relay
with WSS fallback, automatic delay negotiation and clock-aligned input sampling.
Two native processes completed real matches through the public endpoint; two
physical PCs on separate connections remain unverified. Server RTT measured
29 ms on the operator connection. See [current measurements](docs/AZURE-VERIFICATION.md)
and [deployment/cost limits](docs/AZURE-DEPLOYMENT.md).

## Play on Windows

1. Download [Nidhogg-Windows-v1.1.0-rc.2-x64.zip](https://github.com/JAPERSTAR1/Nidhogg-Win64-Port/releases/tag/v1.1.0-rc.2)
   and extract the complete ZIP, or build the runtime below. The previous v1.0.0 remains available for offline/local play;
   both online players must use the new client.
2. Run `Nidhogg.exe` and select your CUSA00920 folder containing `eboot.bin` and
   `Data/`. A sibling `CUSA00920/` folder is also discovered automatically.
3. Choose the game's **Options** for graphics, display, audio, controls, online and HUD
   settings; **F1** also opens this menu. Keyboard and
   XInput controllers are supported; bindings are configurable.
4. **Multiplayer** opens a full game-styled session
   screen: Local Versus, public sessions, Host Public/Private, or Join With Code.
   Private rooms have six-character codes. The host chooses the map in the room
   and presses **Start Game** once a second player joins; no manual Ready is required.
   Each online player uses their own Player 1 bindings. Starting keeps the same
   game process and window. Older releases retain their previous UI.

The default endpoint is `https://lobby.japer.me`. The Azure server operates independently
of the operator PC, subject to its promotional-credit shutdown limits. Players need no VPN, Node.js or tunnel software.
An unavailable endpoint does not prevent offline play.

The current release integrates the menus and adds a customizable connection
HUD. **F3** hides/shows it; **HUD** settings select ping, input delay, connection type,
sync status, corner, size and background opacity. New settings default to hidden,
with ping as the only selected item. `start-game.cmd` opens the game normally;
`start-game.ps1 -Online` opens the online menu directly.

Options and Multiplayer own the whole game viewport, using the original font and
art loaded from your assets. They replace the scene while open, with no gameplay
image or floating settings panel behind them. See [frontend verification](docs/NATIVE-FRONTEND.md).

Graphics settings offer 30/60/120/144/240/unlimited presentation, internal resolution,
filters, VSync and buffering. Gameplay remains at its original 60 Hz. Presentations
above 60 repeat the latest completed game image; they do not add animation frames.
Actual presentation rate depends on rendering work, display and VSync. See
[the local UI/performance verification](docs/INTEGRATED-UI-PERFORMANCE.md).

**Room Transition Effects** and **Screen Flashes** in Graphics default to **Off**
to remove the transition streaks and full-screen flash. Each can be restored to
**Original** independently; gameplay updates and online state remain unchanged.
See [frame-by-frame verification](docs/REDUCED-FLASHES.md).

## Build

Requirements: Windows 10/11 x64, Visual Studio Build Tools with the MSVC C++ x64
toolset and Windows SDK. Python 3 is used by diagnostics; Node.js 20+ is required
only for the lobby server and its tests. No original game files are needed to
compile the runtime or run backend unit tests.

```powershell
git clone https://github.com/JAPERSTAR1/Nidhogg-Win64-Port.git
cd Nidhogg-Win64-Port
.\port\build.ps1
.\port\out\Nidhogg.exe
```

Outputs: GUI `port/out/Nidhogg.exe` and console diagnostic host
`port/out/nidhogg_host.exe`. Both share the same compiled runtime. Existing
compiler warnings are not promoted to errors.

## Develop and verify

```powershell
npm ci --prefix server
npm test --prefix server
python tools/verify-icon.py
.\start-server.ps1
.\start-game.ps1 -Endpoint http://127.0.0.1:27880 -Assets 'D:\Games\CUSA00920'
```

Use `python tests/multiplayer_pair.py --assets 'D:\Games\CUSA00920' --combat
--frames 2800 --label my-local-duel` with a running local server for a real
two-process match. Local tests do not certify connectivity across separate NATs.
For the offline regression, set `NIDHOGG_ASSETS` to your asset folder and run
`python tests/offline_network_regression.py`. Results and isolated saves go under
ignored `tests/results/`. Never run tests against personal save directories.

`tools/package-windows.ps1` packages the runtime and operating instructions using
an explicit manifest; original assets and operator credentials are excluded.
`tools/package-desktop.py --assets 'D:\Games\CUSA00920'` is a separate local
backup/portable-copy tool. Its asset-containing output must not be attached to
source releases.

## Project map

| Location | Responsibility |
| --- | --- |
| `port/src/image.*`, `thunks.*`, `hle.*` | Guest mapping, ABI bridging, SCE compatibility |
| `port/src/gnm/` | PM4 interpretation and Direct3D 11 rendering |
| `port/src/audio/`, `atrac9/` | Windows audio and codec integration |
| `port/src/core/`, `ui/` | Settings, diagnostics and full frontend screens |
| `port/src/net/` | Input synchronization, packet authentication, UDP/STUN and WSS |
| `server/`, `deploy/` | Lobby/relay backend, Azure services and monitoring |
| `analysis/`, `tools/` | Reverse-engineering records and investigation utilities |
| `tests/` | Backend-independent native gameplay regression harnesses |

## Documentation

- [Multiplayer architecture and deployment](docs/MULTIPLAYER.md)
- [Verification status and outstanding limitations](docs/AZURE-VERIFICATION.md)
- [Runtime architecture](docs/ARCHITECTURE.md)
- [Reconstructed history and original snapshot provenance](docs/HISTORY.md)
- [Initial research archive](docs/archive/INITIAL-RESEARCH.md)
- [Contributing](CONTRIBUTING.md), [security reporting](SECURITY.md),
  [third-party notices](THIRD_PARTY_NOTICES.md), [changelog](CHANGELOG.md)

The v1.0.0 reconstructed segment contains 632 commits with an explicitly synthetic
author/committer date schedule. Those dates do not establish nine months of actual work.
Original commit histories are retained on archive branches and in local bundles.
