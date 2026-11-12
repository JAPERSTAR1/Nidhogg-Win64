# Reconstructed project history

This branch is a **source reconstruction**, created on 2026-11-12.
Its **632 commits** have synthetic author dates distributed irregularly from
2026-02-12 to 2026-11-12 at the owner's request.
They do **not** demonstrate nine months of actual development or contributions.
Committer dates record the real reconstruction time. Every commit body repeats
this disclosure and names its recovered source snapshot.

## What was recovered

Both the old Downloads checkout and the secondary Desktop checkout were read.
Original commits have real author dates on 2026-11-11 and 2026-11-12. Uncommitted
legacy launcher/native-server sources were captured before any changes. The
legacy side path is shown before transitioning to the current secondary branch;
this is an editorial ordering of snapshots, not a recovered event timeline.

| Snapshot | Original source reference | Reconstructed commits |
| --- | --- | ---: |
| Repository scaffolding | `0f477f0dbf3d2eed911526658730e4f6be724160` | 4 |
| SELF analysis and initial Windows host | `fd92abb3eb1bdbee333af88f7cadde1235d02b5d` | 75 |
| Guest execution and compatibility development | `0e15e10049e8328e66ea1d3a17f2770cadf72bf0` | 8 |
| NGS2 and ATRAC9 audio | `75ddaebfd9c81dec37b413bc97fc8c210004d262` | 66 |
| Original beta checkpoint | `18df3ca12bb0cd970d1413f90db2cc53e8fb483e` | 15 |
| Legacy offline runtime checkpoint | `4a30e0d886e1dfc3e15cb41dc5deb100605edb4d` | 168 |
| Recovered legacy launcher and native lobby prototype | `original-working-source.zip over 4a30e0d` | 105 |
| Secondary checkout baseline and prototype transition | `993f8ba0f72c32389086b413ebcb4c64a1261915` | 83 |
| Authenticated input netplay and relay implementation | `9f6fa9190d394cc2ed58f63e18baf11c929a7e3d` | 69 |
| Cloudflare account handoff documentation | `a2733d214ede6049937ad17b61116e86399de86e` | 1 |
| Permanent lobby deployment configuration | `5feaeb0511f7deb212ed91815e3c8b7754b3c5b4` | 6 |
| Embedded artwork and portable packaging | `50ac5391b39f621af07f9a8eadbfe925c41733b3` | 4 |
| Repository documentation, source hygiene and validation tooling | `validated secondary working tree, 2026-11-12` | 27 |
| Provenance and schedule documentation | reconstruction tooling | 1 |

Source changes were split into file/diff sections, preserving their snapshot
content. Commits are nonempty, but intermediate partial-file states are not
individually build-certified. Only the final tree and explicitly recorded actual
test runs are verified. Do not treat synthetic dates or commit messages as test
evidence. Personal saves, generated results, game binaries and operator secrets
are excluded from the reconstructed branch.

## Recoverability

- Original remote main is preserved on `main` (no force-push).
- `archive/secondary-before-reconstruction` retains the real secondary history.
- `archive/legacy-working-snapshot` retains the old real history and recovered
  uncommitted source checkpoint. The original Downloads checkout was not edited.
- Local Git bundles and source ZIPs are stored in a separate sibling backup
  directory. Their manifest records source heads and bundle SHA-256 values.
- Original release tags remain attached to their original commit hashes.

The default branch may point at this reconstructed branch; archived refs remain
available. These extra archive branches mean all-refs object counts are larger
than 632. The **reconstructed branch's reachable commit count is exactly 632**.

## Audit

`HISTORY-MANIFEST.json` records original snapshot hashes, actual original dates,
source-tree digests and the synthetic schedule's daily counts.
`HISTORY-SCHEDULE.csv` maps all 632 sequence numbers to synthetic dates and source
stages. Tree digests use sorted UTF-8 paths, a zero byte and each file's binary
SHA-256 digest, excluding `docs/HISTORY*` to avoid self-reference.

```powershell
git rev-list --count codex/reconstructed-timeline
git log --reverse --format='%h %aI %cI %s' codex/reconstructed-timeline
git log archive/legacy-working-snapshot
git log archive/secondary-before-reconstruction
```
