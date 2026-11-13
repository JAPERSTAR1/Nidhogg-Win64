# Local menu and rendering update — 2026-11-13

All source edits, builds, settings, saves and evidence for this update are inside
the authorized `Desktop/nidhogg-alpha/nidhogg-port` working copy. Original game
assets are opened read-only. No server was redeployed. These changes are packaged
in the 1.1.0-rc.2 runtime release.

## Run and configure

Run `start-game.cmd`, or `port/out/Nidhogg.exe`. These use the newly built GUI
runtime. Released ZIPs identify their exact executable/source in release-manifest.json.
Choose **Options** in the game for the full settings screen. **Multiplayer** opens
Local Versus, public sessions and public/private hosting or code joining. These
screens own the viewport; no gameplay image or floating panel is composited behind
them. **F1** remains a settings shortcut. See [the frontend update](NATIVE-FRONTEND.md)
for in-room map choice and match startup without an application restart.
During offline gameplay the native pause/settings screen holds simulation and
excludes the pause duration from the guest clock. Online simulation keeps running.

The menus read the supplied game's `Data/fonts/fMenu` atlas/metrics, with a built-in
fallback. No original art is embedded in the runtime. Keyboard, mouse and the
configured Player 1 XInput controller use the menu; shoulders/Tab change categories.

**HUD** selects the connection display and each field: ping, input delay, connection
type and sync status. Position, size and background opacity are independent. **F3**
hides/shows the connection display. Defaults are hidden with only ping selected;
an existing saved `showNetStats` preference is retained. Settings persist in JSON.

For a 240 Hz display, choose **Graphics → Presentation FPS Limit → 240**. VSync Off
allows uncapped-by-refresh presentation, potentially with tearing; VSync On follows
the display's configured refresh. Filters, internal render scale, monitor/mode,
buffer count, audio and bindings remain configurable. Original gameplay is 60 Hz.
Additional presents reuse the latest rendered game image; this is not interpolated
240 Hz animation and does not guarantee a sustained 240 FPS.

## Renderer changes

Indexed draws now copy only the referenced vertex range and rebase their indices.
The original transform is baked into homogeneous positions, retaining W. Adjacent
draws with the same target, texture, sampler, viewport, blend state and factors
share an upload/draw. Clear operations and dependent passes flush the batch in order.
Dynamic buffers and vectors retain capacity. Immutable texture descriptors are
cached for one submission and revalidated on the next submission.

Feedback passes snapshot the preceding render target before sampling it. This
avoids D3D11's invalid simultaneous SRV/RTV binding. Size changes flush queued work.
A continuous presentation deadline replaces the per-simulation-frame reset. FPS
and simulation counters use counted frames over a time window, not averaged
instantaneous reciprocals. Simulation counting is independent of skipped presents.

Backbuffer resizing now unbinds the previous render target before releasing it.
Display changes requested during UI drawing are deferred to the next composition,
so the UI cannot replace the backbuffer it is currently drawing into.

Audio output holds the device lock through waiting, mixing and writing, so close
and device changes cannot destroy an event/header/device being used. Shutdown
signals the waiter before acquiring the lock. This addresses the access violation
observed during the first short local-versus shutdown test.

## Evidence and limits

Evidence is under `tests/results/ui-performance-2026-11-13/` (ignored by Git).
`baseline.exe` and `baseline-revision.txt` preserve the starting runtime/revision.

- `integrated_settings`, `settings_visual_v2`, `hud_customization`, and
  `online_back_navigation`: game-pad-import scripts enter the real menu path;
  screenshots visually inspected. HUD selections, corner, scale and opacity saved
  with the expected values. Back navigation returns through Multiplayer to Options.
- `integrated_local_final` and `castle_feedback_diagnostics`: complete Castle
  traversal, worm/win, match stats and rematch. The latter enabled D3D11 validation
  and recorded 33,480 feedback passes without resource-binding warnings.
- `clouds_render_debug`: Sky Center → Right A → Right B → Right End;
  `mines_transition_debug` and `wilds_render_debug`: stage load and play smoke tests.
  Mines/Wilds scripts did not complete their matches. Debug validation was enabled
  and did not log warning/error messages in these runs.
- `offline-delivered/report.json`: GUI/console subsystem, local-versus completion,
  Arcade entry, audio, captures and clean shutdown all passed after the audio fix.
- `online-relay-verified/report.json`: two owned native processes used an owned
  localhost UDP relay, completed a match and rematch with 20 common matching state
  hashes, no desync, and 60.00 simulation FPS. Ping-only HUD inspected at 640×360.
- `display_resize_checked` and `buffer_resize_checked`: changed the window from
  1280×720 to 1280×768, confirmed it, and changed double to triple buffering.
  Captures continued and D3D11 validation logged no warnings/errors.
- `final_240_victory`: paced native-menu match, win and rematch completed in
  50.34 seconds for 3,000 simulation frames. Whole-run presentation was about
  210 presents/second, so this is not a claim of sustained 240 in every scene.
- `paced_30_menu`, `paced_60_menu`, `paced_240_menu`: measured approximately
  30, 60 and 238 presents/second respectively, with 60 Hz simulation pacing.
- `npm --prefix server test`: both lobby and authenticated UDP relay test suites pass.

The initial instrumented Castle Right End baseline had renderer median 13.217 ms
and p95 14.748 ms. A later uncapped diagnostic run (`rt_allocation_profile`) measured
median 5.194 ms and p95 5.691 ms, with maximum copied vertices reduced from 2,758,900 to
4,700 per frame. These are CPU renderer timings on this machine, not full-frame
or monitor FPS. Runs with D3D validation, screenshots, caps and other workloads
have different overhead. Compare equivalent modes when benchmarking.

The follow-up frame-by-frame capture reproduced a full-white frame and the
horizontal/vertical transition streaks. Independent Graphics controls now disable
their original render events by default. See [the follow-up verification](REDUCED-FLASHES.md)
for the 2,001-frame comparison and online peers with different effect settings.
Physical controller use with the redesigned menus and manual pause/resume were
not exercised by these scripted runs. Internet play across independent PCs/NATs
remains unverified by this local test; existing service status is documented separately.

## Reproduce

Build with `port/build.ps1`. Run `tests/render_regression.py --name <unique-name>
--integrated --frames 3000 --fps 240` for a paced native-menu full-match run.
Use `--uncapped --fps 0 --debug-d3d` for a fast diagnostic capture/validation run;
uncapped simulation is a diagnostic mode, not a way to speed up normal gameplay.
`--stage 0/1/2/3` chooses Castle/Wilds/Mines/Clouds. `NIDHOGG_LEGACY_MENU_TEST` is
a diagnostic bypass for old frame scripts; normal gameplay does not set it.

`tests/local_online_smoke.py` owns a localhost lobby/UDP relay and two clients,
then stops only the processes it created. It never contacts the public service.
All test clients use isolated user directories. Profiling is opt-in via
`NIDHOGG_RENDER_PROFILE`; D3D validation is opt-in via `NIDHOGG_D3D_DEBUG` and logs
when the debug layer is unavailable. Both are off during normal play.
