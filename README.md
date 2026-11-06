# nidhogg-port

Current Windows multiplayer work (2026-11-12): see
[deployment and operation](docs/MULTIPLAYER.md) and
[verification status](docs/NETWORK-VERIFICATION.md). Both original guests have
completed matching local and public-tunnel matches; separate physical networks
remain unverified. The permanent `lobby.japer.me` tunnel is deployed; its normal
resolver test on this PC awaits upstream ISP DNS propagation.
Build `port/build.ps1`; player GUI `port/out/Nidhogg.exe`.

The Windows EXEs embed the supplied artwork as a multi-size icon and the original
PNG resource from `port/resources/`. `python tools/package-desktop.py --assets
"PATH\TO\CUSA00920"` creates separate ready-to-play Windows and exact original
PS4 folders/ZIPs on the Windows Desktop, verifies copied file hashes, and leaves
the asset source unchanged. Existing desktop packages are kept. Use
`python tools/verify-icon.py` and `python tests/verify-desktop-package.py` to
check embedded resources and portable asset discovery/startup.

The initial extraction notes below are historical; their "State" table does not
describe the current rendering/audio/runtime implementation.

Native Windows x64 port work for **Nidhogg** (PS4, `UP2143-CUSA00920`, v1.00).

Source dump: `C:\Users\puy12\Downloads\g\CUSA00920`.

## State

| step | status |
|---|---|
| SELF → ELF extraction | done, `work/nidhogg.elf` |
| image survey (segments, code, relocs, imports) | done, `analysis/` |
| Windows host that maps and executes the image | done, `port/out/nidhogg_host.exe` |
| relocations applied | 7,972 |
| import slots found and stubbed | 465 |
| entry point | **broken in the dump**, must be recovered |
| SCE shim layer | not started |
| renderer (GNM → D3D11) | not started |

## What the dump actually contains

The eboot is a SELF wrapping an `ET_SCE_RELEXEC` ELF. The code segments are
plaintext x86-64, MSVC-built, with RTTI and the original build paths intact:

```
C:/Projects/FOCAL/Nidhogg/ps4/Nidhogg.elf
C:\Projects\FOCAL\FOCAL\ps4\savedata.cpp
```

Image layout (vaddr = file offset − 0x4000):

```
LOAD 0x000000  0x1ecf58  R+X   code
LOAD 0x1f0000  0x22ec48  RW    data + bss
PLT  0x16f310            465 entries, slots ascending from 0x33b2d8
RELA 0x345980..0x374a80  7,972 R_X86_64_RELATIVE records
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
| `port/src/core/`, `ui/` | Settings, diagnostics and native overlays |
| `port/src/net/` | Input synchronization, packet authentication, UDP/STUN and WSS |
| `server/`, `deploy/` | Lobby/relay backend and tunnel configuration template |
| `analysis/`, `tools/` | Reverse-engineering records and investigation utilities |
| `tests/` | Backend-independent native gameplay regression harnesses |

## Documentation

- [Multiplayer architecture and deployment](docs/MULTIPLAYER.md)
- [Verification status and outstanding limitations](docs/NETWORK-VERIFICATION.md)
- [Runtime architecture](docs/ARCHITECTURE.md)
- [Reconstructed history and original snapshot provenance](docs/HISTORY.md)
- [Initial research archive](docs/archive/INITIAL-RESEARCH.md)
- [Contributing](CONTRIBUTING.md), [security reporting](SECURITY.md),
  [third-party notices](THIRD_PARTY_NOTICES.md), [changelog](CHANGELOG.md)

The reconstructed branch contains 632 commits with an explicitly synthetic
author-date schedule. Those dates do not establish nine months of actual work.
Original commit histories are retained on archive branches and in local bundles.
