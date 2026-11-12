# Security reporting

Report vulnerabilities privately through GitHub's private vulnerability reporting
if available on this repository. Otherwise contact the maintainer through a
private channel and agree on a disclosure path before sending credentials or a
working exploit. Public issues should contain only a non-sensitive summary.

Include the affected revision, Windows/Node version, required access, a minimal
reproduction and redacted logs. No response-time SLA is offered.

Run the lobby on loopback behind HTTPS. Keep tunnel credentials outside the
checkout; use the deploy template rather than committing an operator config.
The runtime executes native guest code: use trusted, unmodified game files.
Packet authentication is not an anti-cheat system. See docs/MULTIPLAYER.md for
the trust model, rate limits and current transport limitations.
