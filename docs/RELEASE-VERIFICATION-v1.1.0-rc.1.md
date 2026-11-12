# Release-candidate verification scope

The executable comes from the existing MSVC build and retains the provided logo.
Native network results and infrastructure checks are recorded in
AZURE-VERIFICATION.md. Offline/package verification runs against the extracted
distribution ZIP using isolated profiles and locally supplied original assets.
The published release includes its machine-readable result and SHA-256 checksums.

The package harness checks asset verification, all four stage entries, keyboard
input, local combat/winner/results/rematch, arcade entry/gameplay, audio/mute,
save persistence, screenshots, renderer error summaries and unchanged eboot hash.
These automated checks do not replace physical controller, listening or visual
judgment, and do not establish two-PC internet gameplay. No second physical PC
was available. There is no promise of fixed 60 FPS or rollback/reconnect support.

v1.0.0 remains the historical offline/local release. This 1.1 candidate requires
both online players to update to protocol 2.1. Current Azure credits are finite;
read AZURE-DEPLOYMENT.md before relying on long-term service availability.
