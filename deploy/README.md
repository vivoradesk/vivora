# DeskBeam deploy

Production deployment artefacts for the DeskBeam services that aren't
shipped inside the user-facing app binary.

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
`deskbeam` user, installs to `/usr/local/bin`, opens UDP/7000 in `ufw`,
and `systemctl enable --now`s the service.

### DNS

Point an A-record at the VPS IP.  Convention: `rdv.<your-domain>`.
Clients pass it as `--rendezvous rdv.deskbeam.dev:7000`.

### Operations

- Logs:    `journalctl -u deskbeam-rendezvous -f`
- Status:  `systemctl status deskbeam-rendezvous`
- Stop:    `sudo systemctl stop deskbeam-rendezvous`
- Update:  rerun `deploy-rendezvous.sh` (it overwrites the binary, then
           systemd restarts the unit on the next failure / reload).

The server is fully in-memory.  Restarts drop all registrations; hosts
re-register on their next 30 s keepalive tick.  No persistent state to
back up.
