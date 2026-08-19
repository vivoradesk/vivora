# Vivora deploy

Deployment artefacts for the Vivora services that aren't shipped inside
the user-facing app binary.

## Layout

```
deploy/
├── vivora-operated/    units for the Vivora-run instances (rdv.vivora.dev,
│                       relay.vivora.dev): the relay requires a Pro license
│                       token and the rendezvous advertises it to peers
├── selfhost/           neutral units for your own box: open relay, no
│                       license gate, no relay advertised by default
├── deploy-rendezvous.sh
└── deploy-relay.sh     push a build + the vivora-operated/ unit over ssh
```

If you are self-hosting, take `selfhost/*.service` -- they are the same
hardened units with the Vivora-specific flags removed.  The comments in
each file say what to add back if you do want a license-gated relay.

## Rendezvous server

Stateless UDP signalling server.  ~1 MB binary, ~10 MB RSS at idle,
single thread.  See `src/relay/rendezvous_server.cpp` for the source.

### Hosting

Anything with a public IPv4 + a UDP port works.  Tested options:

| Provider              | Plan           | Cost     | Notes                                |
| --------------------- | -------------- | -------- | ------------------------------------ |
| Oracle Cloud Free Tier| Always Free    | $0       | ARM 1 OCPU + 1 GB RAM, ample for MVP |
| Hetzner Cloud         | CX11           | €4.5/mo  | Frankfurt or Helsinki, very stable   |
| DigitalOcean / Vultr  | Basic          | $6/mo    | Many regions, easy ramp-up           |

Bandwidth is negligible (signalling only — no media relay): ~100 B per
register/lookup × tens per minute even at scale.

### Deploy

Cross-build the binary for Linux x86_64 (or use the WSL2 build), then:

```sh
./deploy/deploy-rendezvous.sh user@your.vps.example.com
```

The script uploads the binary + systemd unit, creates an unprivileged
`vivora` user, installs to `/usr/local/bin`, opens UDP/7000 in `ufw`,
and `systemctl enable --now`s the service.

### DNS

Point an A-record at the VPS IP.  Convention: `rdv.<your-domain>`.
Clients pass it as `--rendezvous rdv.vivora.dev:7000`.

### Operations

- Logs:    `journalctl -u vivora-rendezvous -f`
- Status:  `systemctl status vivora-rendezvous`
- Stop:    `sudo systemctl stop vivora-rendezvous`
- Update:  rerun `deploy-rendezvous.sh` (it overwrites the binary, then
           systemd restarts the unit on the next failure / reload).

The server is fully in-memory.  Restarts drop all registrations; hosts
re-register on their next 30 s keepalive tick.  No persistent state to
back up.

## Relay server

Stateless UDP forwarder for the case where direct hole-punching can't
establish a peer-to-peer link (symmetric NAT, CGNAT, blocking firewalls).
Both peers BIND with a shared 32-byte session_id; the relay forwards
DBRL DATA packets between them.  See `src/relay/relay_server.cpp`.

### Deploy

```sh
./deploy/deploy-relay.sh user@your.vps.example.com
```

Same shape as the rendezvous deploy: scp binary + systemd unit, reuse
the `vivora` system user, open UDP/7100 in ufw, enable + (re)start
the service.

After a fresh box receives both deploys (`deploy-rendezvous.sh` then
`deploy-relay.sh`), the same VPS hosts both:

- `7000/udp` — rendezvous (signalling, kilobytes per session)
- `7100/udp` — relay (media path, megabits per session)

### Bandwidth note

Unlike rendezvous, the relay carries the full media stream (~15 Mbps
per 1080p60 HEVC session = ~1.6 TB/month/heavy user).  Oracle Free
Tier's 10 TB egress is enough for the early test phase but won't scale
— move the relay to a paid VPS with bandwidth headroom (Hetzner CCX23
~€27/mo includes 20 TB) once usage takes off.

The Vivora-operated instance gates access with `--require-license`
(see `vivora-operated/vivora-relay.service`); self-hosted instances stay
open by default.

### Operations

- Logs:    `journalctl -u vivora-relay -f`
- Status:  `systemctl status vivora-relay`
- Update:  rerun `deploy-relay.sh` (auto-restarts the unit)

State is fully in-memory.  Bindings expire after 60 s if no keepalive;
peers refresh every 20 s on their side.
