# Nidhogg Windows 1.1.0-rc.1

This candidate migrates `lobby.japer.me` to Azure UAE North and adds authenticated
UDP relay with WSS fallback, automatic input-delay agreement, a shared match
start and bounded wall-clock frame alignment. The original guest gameplay,
renderer, audio, saves and local controls remain in the existing runtime.

Both players must update: protocol 2.1 rejects older clients clearly. No VPN or
player-side networking tools are required. The GUI executable includes the
provided logo. Supply your own CUSA00920 assets for the asset-free GitHub ZIP.

Native automated matches through the real public Azure endpoint exercised
movement, attacks, throwing, death, respawning, level progression, victory,
results and rematch with matching state checks. Separate Windows PCs on separate
internet connections remain unverified; this is why the release is a candidate.
Full measurements and test scope are in AZURE-VERIFICATION.md in the repository.

There is no rollback or reconnect/resume. Victory scenes can still lower frame
rate. UDP may be blocked on some networks; WSS remains subject to TCP stalls.
The current $100-credit VM deployment has an automatic stop around November 20,
2026, or earlier if traffic/cost limits trigger. It is not permanent free hosting.
See AZURE-DEPLOYMENT.md before changing funding or restarting it later.
