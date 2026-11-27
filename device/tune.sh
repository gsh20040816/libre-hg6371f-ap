#!/bin/sh
# Safe RF, MAC, and kernel buffer tuning for HG6371F (BCM68252C).
# Idempotent and hitless. NEVER write /proc/irq/*/smp_affinity on this SoC
# (breaks Broadcom lockless wl1-kthrd/wfd1-thrd and causes kernel panics).
PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin:/opt/bcm/bin:/fhrom/bin
export PATH

# 1. Disable vendor wifi_rxdesens.sh 2-second polling loop
touch /fhconf/wifi_rxdesens_disable 2>/dev/null || true
for p in $(ps w | awk '/wifi_rxdesens\.sh/ && !/awk/{print $1}'); do
  kill "$p" 2>/dev/null || true
done
rm -f /var/wifi_rxdesens_flag 2>/dev/null || true

# 2. Kernel socket/queue buffers (no IRQ affinity changes)
sysctl -w net.core.netdev_max_backlog=4096 >/dev/null 2>&1 || true
sysctl -w net.core.rmem_max=4194304 >/dev/null 2>&1 || true
sysctl -w net.core.wmem_max=4194304 >/dev/null 2>&1 || true

# 3. Wait briefly during early boot for wl1 to finish initializing
apply_wl() {
  wl -i wl0 frameburst 1 2>/dev/null || true
  wl -i wl1 frameburst 1 2>/dev/null || true
  wl -i wl1 ampdu_rts 0 2>/dev/null || true
  wl -i wl1 phy_rxdesens 15 2>/dev/null || true
  wl -i wl1 wme_ac ap be aifsn 2 ecwmin 3 ecwmax 5 txop 47 2>/dev/null || true
}

if [ "$(wl -i wl1 isup 2>/dev/null)" = "1" ]; then
  apply_wl
else
  (
    i=0
    while [ "$i" -lt 30 ]; do
      sleep 3
      i=$((i + 1))
      if [ "$(wl -i wl1 isup 2>/dev/null)" = "1" ]; then
        sleep 2
        apply_wl
        exit 0
      fi
    done
  ) </dev/null >/dev/null 2>&1 &
fi
