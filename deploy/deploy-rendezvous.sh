#!/usr/bin/env bash
# Deploy the deskbeam-rendezvous binary to a fresh Linux VPS.
#
# Usage:
#   ./deploy-rendezvous.sh user@host[:port]
#
# Assumes:
#   - The local working tree has a built binary at
#     build-linux/bin/deskbeam-rendezvous (Linux x86_64).  If you build
#     somewhere else, set BIN=path/to/deskbeam-rendezvous before running.
#   - The remote user can sudo without a password (or you have time to
#     watch the prompts).
#
# What it does on the remote:
#   1. apt-get update + install nothing (the binary is statically linked
#      against everything except glibc).
#   2. Create system user 'deskbeam' (no shell, no home).
#   3. Copy the binary to /usr/local/bin/deskbeam-rendezvous and chmod +x.
#   4. Drop the systemd unit at /etc/systemd/system/deskbeam-rendezvous.service.
#   5. Open UDP/7000 in ufw (skipped if ufw isn't installed).
#   6. systemctl daemon-reload + enable + restart.
#   7. systemctl status to verify.

set -euo pipefail

if [[ $# -ne 1 ]]; then
    echo "usage: $0 user@host[:port]" >&2
    exit 1
fi

TARGET="$1"
BIN="${BIN:-build-linux/bin/deskbeam-rendezvous}"
SERVICE_FILE="$(dirname "$0")/deskbeam-rendezvous.service"

if [[ ! -x "$BIN" ]]; then
    echo "binary not found: $BIN" >&2
    echo "Build it first:  cmake -B build-linux -G Ninja  && cmake --build build-linux --target deskbeam-rendezvous" >&2
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

echo "==> Uploading binary + unit file to $HOSTPART"
scp "${SCP_OPTS[@]}" "$BIN" "$SERVICE_FILE" "$HOSTPART:/tmp/" 1>/dev/null

echo "==> Installing on remote"
ssh "${SSH_OPTS[@]}" "$HOSTPART" 'bash -s' <<'REMOTE'
set -euo pipefail

# 1. System user — no login, no home, no shell.  Idempotent.
if ! id deskbeam &>/dev/null; then
    sudo useradd --system --no-create-home --shell /usr/sbin/nologin deskbeam
    echo "  + created system user 'deskbeam'"
fi

# 2. Binary into /usr/local/bin (root-owned, world-readable, executable).
sudo install -m 0755 -o root -g root /tmp/deskbeam-rendezvous /usr/local/bin/deskbeam-rendezvous
echo "  + /usr/local/bin/deskbeam-rendezvous installed"

# 3. systemd unit.
sudo install -m 0644 -o root -g root /tmp/deskbeam-rendezvous.service /etc/systemd/system/
echo "  + /etc/systemd/system/deskbeam-rendezvous.service installed"

# 4. Firewall — best-effort.  ufw is the Ubuntu default; iptables fallback
#    is left to the operator.
if command -v ufw &>/dev/null; then
    sudo ufw allow 7000/udp >/dev/null 2>&1 || true
    echo "  + ufw allow 7000/udp"
fi

# 5. Enable + restart.
sudo systemctl daemon-reload
sudo systemctl enable --now deskbeam-rendezvous.service
echo "  + service enabled + started"

# 6. Status.
sudo systemctl --no-pager --full status deskbeam-rendezvous.service | head -20

# 7. Cleanup tmp.
rm -f /tmp/deskbeam-rendezvous /tmp/deskbeam-rendezvous.service
REMOTE

echo "==> Done.  Tail logs with:"
echo "    ssh $TARGET 'sudo journalctl -u deskbeam-rendezvous -f'"
