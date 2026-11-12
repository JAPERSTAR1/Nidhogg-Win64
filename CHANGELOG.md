# Changelog

## 1.1.0-rc.1 — 2026-11-12

- Azure UAE North authenticated UDP relay, with HTTPS/WSS fallback.
- Automatic delay agreement, shared start and bounded frame-phase correction.
- RTT/jitter/wait/input timing diagnostics and loss/reordering tests.
- Hardened systemd deployment, recovery and promotional-credit guards.
- Independent-network player validation remains outstanding.


Changes below summarize recovered snapshots. They are not release dates inferred
from the synthetic author-date schedule; see docs/HISTORY.md.

## v1.0.0 — 2026-11-12

Playable Windows x64 release targeting offline/local play. The release ZIP
contains the GUI runtime, artwork, English/Arabic startup instructions, exact
source revision, SHA-256 checksums and third-party notices. Original game files
are supplied separately by the player. Online multiplayer remains experimental.
See [release notes](docs/RELEASE-v1.0.0.md) for exact verification scope.

- Original PS4 game execution through Windows compatibility, GNM/PM4 rendering,
  NGS2/ATRAC9 audio, configurable display/input and native overlay settings.
- Authenticated lobby, private/public rooms, readiness, session cleanup, binary
  WSS relay and best-effort direct UDP/STUN input synchronization.
- Fresh-process match startup and deterministic native PRNG reseeding; sampled
  state comparison, matching local/public-tunnel full matches and rematches.
- Named-tunnel deployment templates, managed server startup/shutdown and
  configurable player endpoint; no player VPN or networking tool installation.
- Embedded game artwork, GUI runtime, portable asset discovery and separately
  verified local desktop packages.
- Source provenance, contribution/security guidance, third-party notices and a
  manually triggered asset-free validation workflow.
- Descriptive reconstructed commit messages and unique synthetic author/committer
  timestamps, with original provenance and actual reconstruction time retained.

Independent-network gameplay, general NAT compatibility, low-latency relay play,
rollback, reconnect/resume and complete in-menu click testing remain unverified
or unimplemented as detailed in docs/NETWORK-VERIFICATION.md.

## v1.0.0-beta.1 — original 2026-11-11 checkpoint

Original published beta checkpoint, retained on the original main history.
Earlier tags v0.1.0-alpha and v0.2.0-alpha remain attached to their original hashes.