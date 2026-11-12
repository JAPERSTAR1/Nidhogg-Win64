# Azure operator guide — 2026-11-12

## Current deployment

* UAE North, Ubuntu 24.04 LTS, user-created `game-server-01`, resource group
  `game-server-01_group`, static IPv4 `40.119.184.25`.
* Standard_FX2mds_v2: 2 vCPU, 42 GB RAM. This is oversized for the Node relay;
  inexpensive B sizes were restricted on this FreeTrial subscription. The unrestricted
  UAE x64 inventory at two vCPU or less contained only FX2mds_v2/FX2ms_v2.
* Node 22.23.5 official Linux binary (SHA-256 verified), pinned `ws` dependency,
  Caddy from Ubuntu packages. No load balancer, NAT gateway or paid monitoring.
* Public inbound TCP 80/443, UDP 27881; SSH 22 restricted to the operator's IP.
  Node HTTP listens on 127.0.0.1:27880. DNS-only lobby A record permits UDP.
* `/opt/nidhogg-server` and `/opt/nidhogg-ops` are root-owned. The game service
  runs as non-login `nidhogg-server` with systemd filesystem/process limits.
* `nidhogg.service` restarts failures after 2 seconds. A 30-second health timer
  restarts a persistently unresponsive process after three failed checks.
  Sessions are in memory: a service/VM restart closes current matches.

The home Node server/tunnel and its startup shortcut were stopped/disabled.
The previous shortcut is backed up under the operator's LocalAppData. No player
PC or operator PC is needed to keep the cloud relay online.

## Costs and automatic stop — read before restarting

Azure Retail Prices API, UAE North, USD consumption prices checked 2026-11-12:

| Resource | Unit price | Estimate at 730 hours/month |
| --- | ---: | ---: |
| Linux FX2mds_v2 compute | $0.337/hour | $246.01 |
| Standard static IPv4 | $0.005/hour | $3.65 |
| Premium SSD P4 LRS | $5.76012/month | $5.76 |
| Total before excess traffic/tax | | **$255.42/month** |

This is not an indefinitely free server. The authenticated subscription is
FreeTrial, not Azure for Students. Its spending limit remains **On**; it has not
been upgraded. User authorization is limited to $100 promotional credits.
Exact remaining credit and actual Cost Management data could not be retrieved
for this offer (404/429), so no live remaining-balance claim is made.

An $80 resource-group budget is configured with email actual-spend alerts at
$24/$48/$64 and a forecast alert at $80. Configuration was read back; email
delivery and offer eligibility have not been triggered/verified. Budgets notify,
they do not themselves stop resources.

Independent VM-side guards run every 30 seconds and deallocate this VM if any
of these limits is reached:

* Estimated total $85: $0.337 × wall hours from 2026-11-12 13:00 UTC + $20 reserve.
  This reaches its threshold approximately **2026-11-20 16:53 Riyadh** (13:53 UTC).
* Counted outbound NIC traffic reaches 80 GiB, persisted across reboots.
* Hard deadline **2026-11-21 15:50 Riyadh** (12:50 UTC).

A daily reported-cost check also requests deallocation at $65 if the API becomes
available. The VM managed identity can only query costs and deallocate VMs in
this resource group; it cannot create/resize resources. Estimates deliberately
count stopped wall time and include a reserve for disks/IP/earlier probes.
Disk and public-IP charges continue after deallocation (about $9.41/month at
these prices). The estimate is a conservative safeguard, not a billing guarantee
or a subscription-wide cap. Do not disable guards, remove the spending limit or
restart after their deadline without a new funding decision.

Official references: [Retail Prices API](https://learn.microsoft.com/en-us/rest/api/cost-management/retail-prices/azure-retail-prices),
[spending limit](https://learn.microsoft.com/en-us/azure/cost-management-billing/manage/spending-limit),
[budget behavior](https://learn.microsoft.com/en-us/azure/cost-management-billing/costs/tutorial-acm-create-budgets).

## Operations

SSH uses the operator's secured key outside the repository. No private key,
Cloudflare token, Azure token or personal profile is part of a release.

```bash
sudo systemctl status nidhogg caddy
sudo journalctl -u nidhogg -n 100 --no-pager
sudo systemctl list-timers --all | grep nidhogg
curl -fsS http://127.0.0.1:27880/health
sudo systemctl restart nidhogg
```

Units/scripts/examples are in `deploy/azure/`. On a replacement Ubuntu server:
install checksum-verified Node and Caddy, create the non-login service user and
`/var/lib/nidhogg`, install the six allowlisted backend files plus `npm ci
--omit=dev --ignore-scripts`, then copy the service and monitoring files. Set
`/etc/nidhogg/server.env` from server.env.example (0640 root/service group).
Configure Caddy using Caddyfile.example. Restrict SSH, expose only documented
ports, and set the new static IP in both server.env and the lobby A record.

For cost guards, create root-only `/etc/nidhogg/cost-guard.env` with subscription,
group, VM, explicit budget start/price/reserve/threshold/traffic/deadline values.
Use the variable names in cost-guard.py. Configure a system identity and the
two-action least-privilege role before enabling its timers. Copy scripts into
`/opt/nidhogg-ops`; enable all three timers and the game/Caddy services. Never
reuse an expired deadline or copy operator credentials to a server.

Backend updates: back up `/opt/nidhogg-server` and configuration, copy only
server.mjs, udp-relay.mjs, package.json, package-lock.json and their two tests,
run `npm ci --omit=dev --ignore-scripts && npm test`, then explicitly restart
nidhogg and probe public HTTPS. `systemctl enable --now` does not reload an
already running process after source changes.

## DNS and rollback

Only `lobby.japer.me` was changed from the old tunnel CNAME to a DNS-only A.
Before/after zone snapshots confirmed the other 12 records unchanged. The apex,
www, email records, nameservers and website hosting were not edited. Automated
website requests returned 403 both before and after; website content was not
verified during this migration. A DNS-only lobby exposes the VM IP intentionally.

Operator-only snapshots/config backups are retained locally outside releases.
Rollback can restore the prior lobby record and home startup shortcut, but also
requires its matching protocol server/client and restores the old latency and
PC uptime dependency. Keep original assets and previous release packages intact.
