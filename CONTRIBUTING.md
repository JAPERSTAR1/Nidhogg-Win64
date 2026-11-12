# Contributing

Keep changes small and preserve the original game simulation. Explain the
observed failure, the source/call site or trace supporting the fix, and the
scope of your verification. Do not infer PS4 API semantics from a function name.

Use a topic branch and open a pull request. Include relevant build output and
redacted diagnostics. Never commit original executables/assets, personal saves,
account credentials, room tokens, operator configs or generated test results.

## Checks

- Runtime changes: `port/build.ps1`; run the relevant offline or two-process
  gameplay harness with isolated `--user-dir` directories.
- Backend changes: `npm ci --prefix server` then `npm test --prefix server`.
- Resource changes: `python tools/verify-icon.py` after building.
- Packaging changes: inspect the ZIP manifest and confirm it excludes game data
  and secrets. `tools/package-windows.ps1` is the asset-free release packager.

The manual GitHub Actions validation workflow builds the runtime, checks its
resources and runs backend tests without proprietary assets. Native gameplay
requires local assets and a working graphics/audio environment and is not
claimed as covered by that workflow.

Mark results with their precise scope: local, public relay, or independent
physical networks. Do not turn an unperformed test into a passing status.

Some archived investigation scripts contain paths from the original research
machine. Inspect their arguments and paths before use; they are not player tools.
See docs/HISTORY.md before interpreting reconstructed commit dates.
