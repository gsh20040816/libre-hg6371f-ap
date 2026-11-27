#!/bin/sh
# Minimal async boot hook injected into read-only rootfs.
# Create /fhconf/ap_hook_disable to bypass custom startup without reflashing.
[ -f /fhconf/ap_hook_disable ] && exit 0

START=/opt/upt/apps/ap-panel/start.sh
LOG=/tmp/ap-boot-hook.log

i=0
while [ "$i" -lt 60 ]; do
  if [ -x "$START" ]; then
    LISTEN_IP=$(sed -n 's/^LISTEN_IP=//p' "$START" 2>/dev/null | head -n 1 | tr -d "\"'" | sed 's/.*:-//; s/}$//')
    if [ -n "$LISTEN_IP" ]; then
      ready=$(ip -4 addr show br0 2>/dev/null | grep -F "inet ${LISTEN_IP}/")
    else
      ready=$(ip -4 addr show br0 2>/dev/null | grep ' br0:101$')
    fi
    if [ -n "$ready" ]; then
      sleep 3
      sh "$START" >"$LOG" 2>&1
      exit 0
    fi
  fi
  sleep 3
  i=$((i + 1))
done

echo "timed out waiting for ${LISTEN_IP:-br0:101} or $START" >"$LOG"
exit 0
