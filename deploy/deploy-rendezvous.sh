#!/usr/bin/env bash
# Deploy the vivora-rendezvous binary to a fresh Linux VPS.
#
# Usage:
#   ./deploy-rendezvous.sh user@host[:port]
#
# Assumes:
#   - The local working tree has a built binary at
#     build-linux/bin/vivora-rendezvous (Linux x86_64).  If you build
#     somewhere else, set BIN=path/to/vivora-rendezvous before running.
#   - The remote user can sudo without a password (or you have time to
#     watch the prompts).
#
# What it does on the remote:
#   1. apt-get update + install nothing (the binary is statically linked
#      against everything except glibc).
#   2. Create system user 'vivora' (no shell, no home).
#   3. Copy the binary to /usr/local/bin/vivora-rendezvous and chmod +x.
#   4. Drop the systemd unit at /etc/systemd/system/vivora-rendezvous.service.
#   5. Open UDP/7000 in ufw (skipped if ufw isn't installed).
#   6. systemctl daemon-reload + enable + restart.
#   7. systemctl status to verify.

set -euo pipefail

if [[ $# -ne 1 ]]; then
    echo "usage: $0 user@host[:port]" >&2
    exit 1
fi

TARGET="$1"
BIN="${BIN:-build-linux/bin/vivora-rendezvous}"
SERVICE_FILE="$(dirname "$0")/vivora-rendezvous.service"

if [[ ! -x "$BIN" ]]; then
    echo "binary not found: $BIN" >&2
    echo "Build it first:  cmake -B build-linux -G Ninja  && cmake --build build-linux --target vivora-rendezvous" >&2
    exit 1
fi
if [[ ! -f "$SERVICE_FILE" ]]; then
    echo "service file missing: $SERVICE_FILE" >&2
    exit 1
fi

# Split user@host:port → ssh-friendly forms.
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

# Optional SSH identity file via SSH_KEY env var — for VPSes that require
# a specific key (e.g. Oracle's auto-generated key, AWS .pem).
if [[ -n "${SSH_KEY:-}" ]]; then
    SSH_OPTS+=(-i "$SSH_KEY")
    SCP_OPTS+=(-i "$SSH_KEY")
fi
# Skip the strict-host-key prompt on first deploy.
SSH_OPTS+=(-o StrictHostKeyChecking=accept-new)
SCP_OPTS+=(-o StrictHostKeyChecking=accept-new)

echo "==> Uploading binary + unit file to $HOSTPART"
scp "${SCP_OPTS[@]}" "$BIN" "$SERVICE_FILE" "$HOSTPART:/tmp/" 1>/dev/null

echo "==> Installing on remote"
ssh "${SSH_OPTS[@]}" "$HOSTPART" 'bash -s' <<'REMOTE'
set -euo pipefail

# 1. System user — no login, no home, no shell.  Idempotent.
if ! id vivora &>/dev/null; then
    sudo useradd --system --no-create-home --shell /usr/sbin/nologin vivora
    echo "  + created system user 'vivora'"
fi

# 2. Binary into /usr/local/bin (root-owned, world-readable, executable).
sudo install -m 0755 -o root -g root /tmp/vivora-rendezvous /usr/local/bin/vivora-rendezvous
echo "  + /usr/local/bin/vivora-rendezvous installed"

# 3. systemd unit.
sudo install -m 0644 -o root -g root /tmp/vivora-rendezvous.service /etc/systemd/system/
echo "  + /etc/systemd/system/vivora-rendezvous.service installed"

# 4. Firewall — best-effort.  ufw is the Ubuntu default; iptables fallback
#    is left to the operator.
if command -v ufw &>/dev/null; then
    sudo ufw allow 7000/udp >/dev/null 2>&1 || true
    echo "  + ufw allow 7000/udp"
fi

# 5. Enable + (re)start.  Use restart explicitly so a redeploy always
#    picks up the new binary even when the unit was already active.
sudo systemctl daemon-reload
sudo systemctl enable vivora-rendezvous.service >/dev/null 2>&1 || true
sudo systemctl restart vivora-rendezvous.service
echo "  + service enabled + (re)started"

# 6. Status.
sudo systemctl --no-pager --full status vivora-rendezvous.service | head -20

# 7. Cleanup tmp.
rm -f /tmp/vivora-rendezvous /tmp/vivora-rendezvous.service
REMOTE

echo "==> Done.  Tail logs with:"
echo "    ssh $TARGET 'sudo journalctl -u vivora-rendezvous -f'"
