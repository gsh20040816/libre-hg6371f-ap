#!/bin/sh
# Restricted forced-command receiver for automated TLS certificate push.
# Install on the AP at /opt/upt/apps/ap-panel/bin/recv-tls-cert.sh (chmod 755)
# and lock the dedicated SSH key in authorized_keys with:
#   command="/opt/upt/apps/ap-panel/bin/recv-tls-cert.sh",no-port-forwarding,no-X11-forwarding,no-agent-forwarding,no-pty ssh-ed25519 AAAA...
set -eu
umask 077

DST="${AP_TLS_DIR:-/opt/upt/apps/ap-panel/etc}"
TMP=$(mktemp -d /tmp/ap-tls-recv.XXXXXX)
trap 'rm -rf "$TMP"' EXIT

tar -xf - -C "$TMP" tls.crt tls.key 2>/dev/null || { echo "invalid tar payload" >&2; exit 1; }

grep -q "BEGIN CERTIFICATE" "$TMP/tls.crt" || { echo "invalid tls.crt" >&2; exit 1; }
grep -q "BEGIN .*PRIVATE KEY" "$TMP/tls.key" || { echo "invalid tls.key" >&2; exit 1; }

NEW_HASH=$(cat "$TMP/tls.crt" "$TMP/tls.key" | sha256sum | awk '{print $1}')
OLD_HASH=""
if [ -f "$DST/tls.crt" ] && [ -f "$DST/tls.key" ]; then
  OLD_HASH=$(cat "$DST/tls.crt" "$DST/tls.key" | sha256sum | awk '{print $1}')
fi

if [ "$NEW_HASH" = "$OLD_HASH" ]; then
  echo "unchanged ($NEW_HASH)"
  exit 0
fi

mkdir -p "$DST"
chmod 700 "$DST"
cp "$TMP/tls.crt" "$DST/tls.crt.tmp"
cp "$TMP/tls.key" "$DST/tls.key.tmp"
chmod 644 "$DST/tls.crt.tmp"
chmod 600 "$DST/tls.key.tmp"
mv -f "$DST/tls.crt.tmp" "$DST/tls.crt"
mv -f "$DST/tls.key.tmp" "$DST/tls.key"

# First-ever cert install needs one restart to bind :443; subsequent renewals hot-reload by mtime.
if ! ss -tln 2>/dev/null | grep -q ":443 "; then
  rc-service ap-panel restart >/dev/null 2>&1 || true
fi
echo "updated ($NEW_HASH)"
