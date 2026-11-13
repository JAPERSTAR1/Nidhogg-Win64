# Reduced transition effects and flashes — 2026-11-13

The game has separate `FinalFantasyVIIITransition` and `Flash` entities. Capturing
every frame reproduced a completely white frame (551), horizontal streaks when
crossing between Castle rooms, and vertical streaks when returning to level select.
Sparse screenshots had missed the complete white frame.

Graphics now has **Room Transition Effects** and **Screen Flashes**, both **Off**
by default. Select **Original** to restore either effect independently. The latter
also suppresses the original combat flash. These are presentation preferences;
they do not change combat, room progression, input, timers or the 60 Hz simulation.

The runtime redirects only the two entities' Draw virtual entries to an existing
empty void event in the privately mapped CUSA00920 image. Create, Step and Destroy
still execute, including their random-number calls. No original file is patched.
This approach avoids changing entity visibility/state or suppressing update logic.
The original pointer and the empty event's RET instruction are checked before each
entry is changed. An unrecognized entry is left intact and logged. Switching the
setting back to Original restores the original Draw entry.

Verified CUSA00920 entries (image-relative offsets):

| Entity | Draw entry | Original Draw function |
| --- | --- | --- |
| FinalFantasyVIIITransition | `0x329c10` | `0x4fec0` |
| Flash | `0x329c90` | `0x4ffb0` |

The empty event is `0x4fa20`. Entries were identified from the constructor/type
name, ELF relocations and function disassembly. The transition Step updates its
effect image/timer; its Draw only sets the drawing mode and draws that image.
Flash Draw issues the full-screen rectangle. Neither render function advances RNG.

Evidence lives in ignored `tests/results/ui-performance-2026-11-13/`:

- `portal_all_frames`: 2,001 original-effect captures, frames 450–2450; full-white
  frame 551 and transition streaks reproduced.
- `portal_reduced_all_frames`: the same capture range after the change. No fully
  white frames; room progression and match winner match the baseline, including
  win, match stats, level select and rematch. `before-after.png` shows frames 551,
  1035, 1518 and 2389 side by side. `portal-verification.json` records the checks.
- `portal_effects_settings`: both options switched Original/Off independently,
  restored at runtime, and persisted to settings JSON. Screenshots inspected.
- `portal-mixed-effects`: two native processes completed a localhost relay match
  and rematch with the host's effects Off and the guest's Original. Twenty common
  state hashes matched, with no desync or timeout. This checks determinism and
  transport, not performance or connectivity across independent networks.
- `portal-offline-final`: GUI local-versus victory, Arcade entry, audio, screenshot
  and clean-shutdown regression results after the effect change.

To run an original-effect diagnostic with selected screenshots:

```powershell
python tests/render_regression.py --name fx-original --integrated --uncapped --fps 0 --frames 2650 --original-effects --shots 551,1035,1518,2389
python tests/render_regression.py --name fx-reduced --integrated --uncapped --fps 0 --frames 2650 --shots 551,1035,1518,2389
python tests/local_online_smoke.py --label fx-mixed --mixed-effects
```

For the full capture comparison pass every frame number from 450 through 2450 to
`--shots`, then run `tests/verify_portal_effects.py <original-case> <reduced-case>`.
This verifier checks the reproduced white frame, its absence afterward, the capture
count and match progression, and creates the comparison image. All clients use
isolated user directories. The original game folder is read-only throughout.
