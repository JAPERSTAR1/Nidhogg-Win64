# Nidhogg Windows 1.1.0-rc.2

Options and Multiplayer now open full game-styled screens with the original
menu font and art. Browse public sessions, create public or private rooms, or
join a six-character code. The host selects the map in-room and starts as soon
as the challenger connects; no manual Ready step is needed. Match startup keeps
the same application process and window.

Graphics, display, sound, controls, network and HUD settings share the integrated
frontend. Customize connection details, size, opacity and corner, or hide them
with F3. Room transition effects and screen flashes default to Off, independently
restorable to Original. Gameplay updates remain unchanged.

Renderer geometry compaction, batching, feedback snapshots and reusable buffers
reduce rendering work. Audio shutdown and display resize races are fixed.
Presentation supports 30/60/120/144/240/unlimited. Gameplay and animation remain
60 Hz; higher presentation repeats the latest image and does not guarantee
sustained 240 FPS.

Both online players should update to this candidate. It supports the existing
hosted relay without a server deployment. Online play remains experimental:
this update passed two-client localhost matches, map selection, victory/rematch,
matching state checks and unchanged PID/window tests. Two physical PCs on
independent networks remain unverified. There is no rollback or reconnect.
Hosted service availability is subject to the deployment's credit limits.

Download and extract the complete Windows x64 ZIP, then run Nidhogg.exe.
Supply your own CUSA00920 folder containing eboot.bin and Data. Original game
files are not included. Release assets include checksums and a machine-readable
verification report. See NATIVE-FRONTEND.md and RELEASE-VERIFICATION.md for scope.
