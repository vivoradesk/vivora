#!/usr/bin/env bash
# Deploy the vivora-relay binary to a Linux VPS (typically the same
# host already running vivora-rendezvous).
#
# Usage:
#   ./deploy-relay.sh user@host[:port]
#
# Same shape as deploy-rendezvous.sh — assumes a Linux build at
# build-linux/bin/vivora-relay (override via BIN=).  The 'vivora'
# system user is created by deploy-rendezvous.sh; this script reuses
# it and only adds the relay binary + unit + ufw rule.

set -euo pipefail

if [[ $# -ne 1 ]]; then
    echo "usage: $0 user@host[:port]" >&2
    exit 1
fi

TARGET="$1"
BIN="${BIN:-build-linux/bin/vivora-relay}"
SERVICE_FILE="$(dirname "$0")/vivora-relay.service"

if [[ ! -x "$BIN" ]]; then
    echo "binary not found: $BIN" >&2
    echo "Build it first: cmake --build build-linux --target vivora-relay" >&2
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

# Optional: also upload the license public key.  Pulled from
# LICENSE_PK env var (path to local file).  If unset and the
# unit file references --require-license, deployment will still
# work but the relay will refuse to start until the file is on
# disk under /etc/vivora/license.pk.
if [[ -n "${LICENSE_PK:-}" ]]; then
    if [[ ! -f "$LICENSE_PK" ]]; then
        echo "LICENSE_PK=$LICENSE_PK does not exist" >&2
        exit 1
    fi
    scp "${SCP_OPTS[@]}" "$LICENSE_PK" "$HOSTPART:/tmp/license.pk" 1>/dev/null
    UPLOAD_LICENSE_PK=1
else
    UPLOAD_LICENSE_PK=0
fi
export UPLOAD_LICENSE_PK

echo "==> Installing on remote"
ssh "${SSH_OPTS[@]}" "$HOSTPART" "UPLOAD_LICENSE_PK=$UPLOAD_LICENSE_PK bash -s" <<'REMOTE'
set -euo pipefail

# 'vivora' user comes from deploy-rendezvous.sh; create if missing
# so this script also works on a fresh box.
if ! id vivora &>/dev/null; then
    sudo useradd --system --no-create-home --shell /usr/sbin/nologin vivora
    echo "  + created system user 'vivora'"
fi

sudo install -m 0755 -o root -g root /tmp/vivora-relay /usr/local/bin/vivora-relay
echo "  + /usr/local/bin/vivora-relay installed"

sudo install -m 0644 -o root -g root /tmp/vivora-relay.service /etc/systemd/system/
echo "  + /etc/systemd/system/vivora-relay.service installed"

# Install license public key if uploaded.  /etc/vivora is created with
# 0755 so the vivora user can read the key under ProtectSystem=strict.
if [[ "${UPLOAD_LICENSE_PK:-0}" == "1" ]]; then
    sudo mkdir -p /etc/vivora
    sudo install -m 0644 -o root -g root /tmp/license.pk /etc/vivora/license.pk
    rm -f /tmp/license.pk
    echo "  + /etc/vivora/license.pk installed"
fi

if command -v ufw &>/dev/null; then
    sudo ufw allow 7100/udp >/dev/null 2>&1 || true
    echo "  + ufw allow 7100/udp"
fi

sudo systemctl daemon-reload
sudo systemctl enable vivora-relay.service >/dev/null 2>&1 || true
sudo systemctl restart vivora-relay.service
echo "  + service enabled + (re)started"

sudo systemctl --no-pager --full status vivora-relay.service | head -15

rm -f /tmp/vivora-relay /tmp/vivora-relay.service
REMOTE

echo "==> Done.  Tail logs with:"
echo "    ssh $TARGET 'sudo journalctl -u vivora-relay -f'"
