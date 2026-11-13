# Release verification — 1.1.0-rc.2

Built with MSVC on Windows x64, with the GUI subsystem, static runtime and the
existing port logo. The asset-free ZIP has an explicit entry allowlist, a source
commit identifier and executable SHA-256 in its manifest. The release includes
archive/executable checksums and the extracted-package smoke report.

Before packaging, real native guest tests on one Windows machine established:

- Original Multiplayer -> public list -> private room -> host map selection ->
  Start, with no player-facing Ready action. Both clients retained the same PID
  and HWND, entered Wilds and produced 10 matching state hashes without desync.
- An older readiness-gated service that ignores the new start-map field still
  started Wilds correctly using automatic readiness and authenticated map agreement.
- A full online Castle match, victory/results and rematch with different visual
  effect settings produced 20 matching hashes. Both clients kept their PID/HWND.
- GUI offline local-versus completion, Arcade entry, audio, capture and shutdown.
- Fullscreen settings appearance, persisted automatic/manual delay, Escape to the
  original menu and re-entry. No gameplay image is composited behind these screens.
- Lobby authorization, private/public isolation, host-only start without readiness,
  stage validation/idempotency and authenticated UDP relay tests pass.

The extracted ZIP smoke harness additionally exercises assets, all four stage
entries, local combat/winner/results/rematch, Arcade gameplay, sound/mute, native
settings persistence, screenshots and unchanged original eboot hash. Its attached
JSON records the actual checks and exact executable/source identity.

Prior instrumented renderer measurements and transition frame comparisons are in
INTEGRATED-UI-PERFORMANCE.md and REDUCED-FLASHES.md. Higher presentation settings
do not change 60 Hz animation/logic. Local test results do not certify independent
internet networks, physical controller behavior or sustained 240 FPS.
