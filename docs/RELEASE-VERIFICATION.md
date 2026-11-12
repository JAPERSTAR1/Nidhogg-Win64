# v1.0.0 release verification — 2026-11-12

The distributed Windows GUI binary was rebuilt with the existing MSVC build
system, packaged, extracted to a fresh directory and exercised with locally
supplied original CUSA00920 files. Profiles, saves, logs and screenshots were
isolated under ignored `tests/results/`. Original assets were not modified.

Binary SHA-256:
`10cbd20cbada0b7586a497293c553e80114dbea656df0f0632276320f2097791`.

| Subsystem | Status | Evidence |
| --- | --- | --- |
| Windows compilation/linking | VERIFIED WORKING | `port/build.ps1` exits 0; both GUI and diagnostic hosts built. Existing compiler warnings remain. |
| Packaged GUI startup and asset verification | VERIFIED WORKING | Extracted ZIP's executable starts without developer tools and accepts supplied original files; exit 0. |
| Save creation and restart | VERIFIED WORKING | Options creates 16-byte preferences; a second process preserves their bytes. |
| Castle, Wilds, Mines, Clouds | VERIFIED WORKING | Each original stage loads, renders a screenshot, mixes nonzero audio and exits cleanly. |
| Local two-player match and rematch | VERIFIED WORKING | Real original guest reaches the same Castle progression, Yellow victory, match results and another Castle start; 6500 simulation frames exercised. This is not a long-duration hardware soak test. |
| Arcade | VERIFIED WORKING | Progress screen and original stage gameplay reached in a 3500-frame run; full ladder completion is not claimed. |
| Audio mute | VERIFIED WORKING | All sampled audio peaks in the muted run are zero. |
| Icon/artwork | VERIFIED WORKING | All seven embedded icon sizes and the byte-identical original PNG pass PE-resource verification. |
| Lobby unit/integration checks | VERIFIED WORKING | `npm test --prefix server` passes authorization, isolation, relay packet, expiration and cleanup assertions. |
| Independent-network online matches | BLOCKED | No second physical Windows PC and independent network available; remains experimental. |
| Other Windows hardware/driver combinations | NOT IMPLEMENTED | No compatibility matrix beyond the development machine has been tested. |

`tests/release_smoke.py` performs 47 checks on the ZIP itself. Run it with
`--assets PATH_TO_CUSA00920`; its machine-readable report and screenshots stay
under `tests/results/release-v1.0.0-*`. A redacted report with release/source
identity is attached to the GitHub release. The source revision is also included
in the packaged `release-manifest.json` and checksums accompany the download.

This is the release baseline for offline/local play. It does not certify
rollback, reconnect, all input devices/resolutions, every save-error case,
full Arcade completion or competitive internet latency.
