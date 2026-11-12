# Nidhogg Windows v1.0.0

Playable Windows x64 release targeting **offline and local multiplayer**.
The native runtime executes the original PS4 game code with Direct3D 11 graphics,
Windows audio and configurable keyboard/XInput controls.

## Install and play

1. Download `Nidhogg-Windows-v1.0.0-x64.zip` from this release.
2. Extract the whole archive into a writable folder.
3. Run `Nidhogg.exe` and choose your CUSA00920 folder containing `eboot.bin` and
   `Data/`, or place that folder beside `Nidhogg.exe` for automatic discovery.
4. Press F1 for display, audio and controls. Use the original menus for Arcade
   or local two-player matches.

Requirements: Windows 10/11 x64 and a Direct3D 11-capable graphics device.
Players do not need Visual Studio, Python, Node.js, PowerShell commands or a VPN.
Original game executables/assets are not included. Instructions are supplied in
both English and Arabic. The ZIP includes its source revision in
`release-manifest.json`; `SHA256SUMS.txt` accompanies the download.

## Verification and limits

This release is verified on the development Windows machine; it is not a promise
that every hardware/driver combination has been tested. Exact packaged-binary
checks and their results are recorded in [RELEASE-VERIFICATION.md](RELEASE-VERIFICATION.md).

**Online multiplayer remains experimental.** The lobby, private/public rooms,
WSS relay and best-effort UDP/STUN input synchronization are included. Prior
same-PC tests completed matching full games through a public tunnel. Separate
physical PCs on independent connections, general NAT compatibility, low-latency
relay play and the complete click-driven online flow are not certified.
There is no rollback or reconnect/resume. See [NETWORK-VERIFICATION.md](NETWORK-VERIFICATION.md).

The default online endpoint is `https://lobby.japer.me`. Its operator computer
must remain on; offline play does not require that service.

This is an unofficial community runtime. Source history includes an explicitly
reconstructed schedule; synthetic commit timestamps are not release test dates.
