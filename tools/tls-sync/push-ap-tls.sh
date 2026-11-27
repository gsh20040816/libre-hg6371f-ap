#!/bin/sh
# Push renewed TLS certificate from an upstream router/ACME host to the HG6371F AP
# via a restricted forced-command SSH key. Skips SSH & flash writes when unchanged.
#
# Configure via environment variables or edit the defaults below:
#   CERT     Path to fullchain PEM certificate
#   KEY      Path to private key PEM
#   SSH_KEY  Dedicated Ed25519 private key (Dropbear or OpenSSH format)
#   AP_HOST  Target AP IP or hostname
#   SSH_BIN  "dbclient" (OpenWrt Dropbear) or "ssh" (OpenSSH)
set -eu

CERT="${CERT:-/etc/ssl/acme/example.fullchain.crt}"
KEY="${KEY:-/etc/ssl/acme/example.key}"
SSH_KEY="${SSH_KEY:-/etc/dropbear/ap_cert_push_ed25519}"
AP_HOST="${AP_HOST:-192.168.1.2}"
STATE="${STATE:-/etc/acme/.ap_tls_pushed.sha256}"
SSH_BIN="${SSH_BIN:-dbclient}"

[ -r "$CERT" ] && [ -r "$KEY" ] || { echo "missing cert or key" >&2; exit 1; }
[ -r "$SSH_KEY" ] || { echo "missing ssh key: $SSH_KEY" >&2; exit 1; }

CUR_HASH=$(cat "$CERT" "$KEY" | sha256sum | awk '{print $1}')
OLD_HASH=$(cat "$STATE" 2>/dev/null || true)

if [ "${1:-}" != "--force" ] && [ "$CUR_HASH" = "$OLD_HASH" ]; then
  echo "ap-tls-push: cert unchanged ($CUR_HASH), skipping"
  exit 0
fi

TMP=$(mktemp -d /tmp/ap-tls-push.XXXXXX)
trap 'rm -rf "$TMP"' EXIT
cp "$CERT" "$TMP/tls.crt"
cp "$KEY" "$TMP/tls.key"

if [ "$SSH_BIN" = "dbclient" ]; then
  OUT=$(tar -cf - -C "$TMP" tls.crt tls.key | dbclient -T -i "$SSH_KEY" "root@$AP_HOST" "sync-tls" 2>&1)
else
  OUT=$(tar -cf - -C "$TMP" tls.crt tls.key | "$SSH_BIN" -T -o BatchMode=yes -i "$SSH_KEY" "root@$AP_HOST" "sync-tls" 2>&1)
fi

mkdir -p "$(dirname "$STATE")"
printf '%s\n' "$CUR_HASH" > "$STATE"
chmod 600 "$STATE"
echo "ap-tls-push: $OUT"
