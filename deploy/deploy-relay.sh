#!/usr/bin/env bash
# Deploy the deskbeam-relay binary to a Linux VPS (typically the same
# host already running deskbeam-rendezvous).
#
# Usage:
#   ./deploy-relay.sh user@host[:port]
#
# Same shape as deploy-rendezvous.sh — assumes a Linux build at
# build-linux/bin/deskbeam-relay (override via BIN=).  The 'deskbeam'
# system user is created by deploy-rendezvous.sh; this script reuses
# it and only adds the relay binary + unit + ufw rule.

set -euo pipefail

if [[ $# -ne 1 ]]; then
    echo "usage: $0 user@host[:port]" >&2
    exit 1
fi

TARGET="$1"
BIN="${BIN:-build-linux/bin/deskbeam-relay}"
SERVICE_FILE="$(dirname "$0")/deskbeam-relay.service"

if [[ ! -x "$BIN" ]]; then
    echo "binary not found: $BIN" >&2
    echo "Build it first: cmake --build build-linux --target deskbeam-relay" >&2
    exit 1
fi
if [[ ! -f "$SERVICE_FILE" ]]; then
    echo "service file missing: $SERVICE_FILE" >&2
    exit 1
fi

if [[ "$TARGET" == *:* ]]; then
    PORT="${TARGET##*:}"
    HOSTPART="${TARGET%:*}"
    SSH_OPTS=(-p "$PORT")
    SCP_OPTS=(-P "$PORT")
else
    HOSTPART="$TARGET"
    SSH_OPTS=()
    SCP_OPTS=()
fi
if [[ -n "${SSH_KEY:-}" ]]; then
    SSH_OPTS+=(-i "$SSH_KEY")
    SCP_OPTS+=(-i "$SSH_KEY")
fi
SSH_OPTS+=(-o StrictHostKeyChecking=accept-new)
SCP_OPTS+=(-o StrictHostKeyChecking=accept-new)

echo "==> Uploading binary + unit file to $HOSTPART"
scp "${SCP_OPTS[@]}" "$BIN" "$SERVICE_FILE" "$HOSTPART:/tmp/" 1>/dev/null

echo "==> Installing on remote"
ssh "${SSH_OPTS[@]}" "$HOSTPART" 'bash -s' <<'REMOTE'
set -euo pipefail

# 'deskbeam' user comes from deploy-rendezvous.sh; create if missing
# so this script also works on a fresh box.
if ! id deskbeam &>/dev/null; then
    sudo useradd --system --no-create-home --shell /usr/sbin/nologin deskbeam
    echo "  + created system user 'deskbeam'"
fi

sudo install -m 0755 -o root -g root /tmp/deskbeam-relay /usr/local/bin/deskbeam-relay
echo "  + /usr/local/bin/deskbeam-relay installed"

sudo install -m 0644 -o root -g root /tmp/deskbeam-relay.service /etc/systemd/system/
echo "  + /etc/systemd/system/deskbeam-relay.service installed"

if command -v ufw &>/dev/null; then
    sudo ufw allow 7100/udp >/dev/null 2>&1 || true
    echo "  + ufw allow 7100/udp"
fi

sudo systemctl daemon-reload
sudo systemctl enable deskbeam-relay.service >/dev/null 2>&1 || true
sudo systemctl restart deskbeam-relay.service
echo "  + service enabled + (re)started"

sudo systemctl --no-pager --full status deskbeam-relay.service | head -15

rm -f /tmp/deskbeam-relay /tmp/deskbeam-relay.service
REMOTE

echo "==> Done.  Tail logs with:"
echo "    ssh $TARGET 'sudo journalctl -u deskbeam-relay -f'"
