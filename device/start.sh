#!/bin/sh
# AP-side entry point (/opt/upt/apps/ap-panel/start.sh).
# Run via `setsid sh /opt/upt/apps/ap-panel/start.sh` from an interactive SSH
# session so killing the old dropbear instance does not tear down the caller.
set -u

BASE=/opt/upt/apps/ap-panel
LISTEN_IP=192.168.1.2

[ -x "$BASE/bin/dropbear" ] || { echo "missing $BASE/bin/dropbear" >&2; exit 1; }
[ -f "$BASE/etc/dropbear_ed25519_host_key" ] || \
  "$BASE/bin/dropbearkey" -t ed25519 -f "$BASE/etc/dropbear_ed25519_host_key" >/dev/null 2>&1

# Point root's HOME/shell at persistent writable storage
awk -F: -v h="$BASE/root" 'BEGIN{OFS=":"} $1=="root"{$6=h; $7="/bin/sh"} {print}' \
  /tmp/passwd > /tmp/.passwd.new && cat /tmp/.passwd.new > /tmp/passwd && rm -f /tmp/.passwd.new
chmod 700 "$BASE/root" 2>/dev/null || true
chmod 600 "$BASE/root/.ssh/authorized_keys" 2>/dev/null || true

# Allow SSH (22) and panel (8081) from local VLAN subnets
iptables -C INPUT -p tcp --dport 22 -j ACCEPT 2>/dev/null || \
  iptables -I INPUT 1 -p tcp --dport 22 -j ACCEPT
while :; do
  n=$(iptables -L INPUT -n --line-numbers | awk '/dpt:8081/{print $1; exit}')
  [ -n "$n" ] || break
  iptables -D INPUT "$n"
done
for net in 192.168.1.0/24 192.168.2.0/24; do
  iptables -I INPUT 1 -p tcp --dport 8081 -s "$net" -j ACCEPT
done

# Dropbear SSH (public-key only)
[ -f "$BASE/dropbear.pid" ] && kill "$(cat "$BASE/dropbear.pid")" 2>/dev/null || true
"$BASE/bin/dropbear" -s -p "${LISTEN_IP}:22" \
  -r "$BASE/etc/dropbear_ed25519_host_key" -P "$BASE/dropbear.pid"

# Go web panel server
mkdir -p "$BASE/log"
kill $(pgrep -f 'uhttpd.*8081' 2>/dev/null) 2>/dev/null || true
kill $(pgrep -f 'ap-panel-srv' 2>/dev/null) 2>/dev/null || true
sleep 1
if [ -x "$BASE/bin/ap-panel-srv" ]; then
  nohup "$BASE/bin/ap-panel-srv" -listen "${LISTEN_IP}:8081" -base "$BASE" \
    >>"$BASE/log/panel.log" 2>&1 </dev/null &
  echo $! > "$BASE/panel.pid"
  renice 0 $! 2>/dev/null || true
fi

# Safe wireless & sysctl tuning (never touches IRQ smp_affinity)
[ -x "$BASE/tune.sh" ] && sh "$BASE/tune.sh"

sleep 1
netstat -lnt 2>/dev/null | grep -E "${LISTEN_IP}:(22|8081)" || { echo "service startup failed" >&2; exit 1; }
