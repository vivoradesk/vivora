#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Andrii Myronov
# SPDX-License-Identifier: AGPL-3.0-or-later

# Install the Vivora servers on the machine you are running this on.
#
#   sudo ./install.sh                  # both servers
#   sudo ./install.sh --rendezvous     # signalling only
#   sudo ./install.sh --relay          # media relay only
#
# The deploy-*.sh scripts in the source tree push a build to a remote box over
# ssh; this is the other half, for when you already have the tarball on the
# server.  Re-running it is safe: everything below is idempotent.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

want_rendezvous=1
want_relay=1
case "${1:-}" in
    --rendezvous) want_relay=0 ;;
    --relay)      want_rendezvous=0 ;;
    "")           ;;
    -h|--help)    sed -n '2,10p' "$0" | sed 's/^# \?//'; exit 0 ;;
    *)            echo "unknown option: $1" >&2; exit 1 ;;
esac

if [[ $EUID -ne 0 ]]; then
    echo "This needs root to create a system user and install unit files." >&2
    echo "Re-run with: sudo $0 ${1:-}" >&2
    exit 1
fi

if ! command -v systemctl >/dev/null 2>&1; then
    echo "No systemd on this machine." >&2
    echo "The binaries in bin/ are self-contained -- run them directly, or" >&2
    echo "adapt systemd/*.service to whatever init you use." >&2
    exit 1
fi

# A dedicated unprivileged account.  The units lock these processes down hard
# (ProtectSystem=strict, no new privileges, no filesystem to speak of), and
# running them as a user with nothing to lose is the first half of that.
if ! id vivora >/dev/null 2>&1; then
    useradd --system --no-create-home --shell /usr/sbin/nologin vivora
    echo "  + created system user 'vivora'"
else
    echo "  = system user 'vivora' already exists"
fi

install_one() {
    local name="$1" port="$2"
    install -m 0755 -o root -g root "$here/bin/$name" "/usr/local/bin/$name"
    install -m 0644 -o root -g root "$here/systemd/$name.service" "/etc/systemd/system/"
    echo "  + /usr/local/bin/$name"
    echo "  + /etc/systemd/system/$name.service"
    systemctl enable --now "$name" >/dev/null
    echo "  + $name enabled and started (UDP $port)"
}

[[ $want_rendezvous -eq 1 ]] && install_one vivora-rendezvous 7000
[[ $want_relay      -eq 1 ]] && install_one vivora-relay      7100

systemctl daemon-reload

echo
echo "Done.  Status:"
[[ $want_rendezvous -eq 1 ]] && systemctl --no-pager --lines=0 status vivora-rendezvous || true
[[ $want_relay      -eq 1 ]] && systemctl --no-pager --lines=0 status vivora-relay      || true

echo
echo "Open the ports yourself -- this script will not touch your firewall:"
[[ $want_rendezvous -eq 1 ]] && echo "  ufw allow 7000/udp   # rendezvous, signalling only"
[[ $want_relay      -eq 1 ]] && echo "  ufw allow 7100/udp   # relay, carries the full stream"
echo
echo "Then point clients at this machine, in Settings or on the command line:"
[[ $want_rendezvous -eq 1 ]] && echo "  vivora --view <peer> --rendezvous $(hostname -f 2>/dev/null || hostname):7000"
[[ $want_relay      -eq 1 ]] && echo "  vivora --view <peer> --relay      $(hostname -f 2>/dev/null || hostname):7100"
echo
echo "Logs:      journalctl -u vivora-rendezvous -f"
echo "Uninstall: systemctl disable --now vivora-rendezvous vivora-relay"
echo "           rm /usr/local/bin/vivora-{rendezvous,relay}"
echo "           rm /etc/systemd/system/vivora-{rendezvous,relay}.service"
