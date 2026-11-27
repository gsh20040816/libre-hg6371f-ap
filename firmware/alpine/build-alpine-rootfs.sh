#!/bin/sh
# Build a clean Alpine Linux (armv7, musl + apk + OpenRC) rootfs for HG6371F,
# keeping ONLY the minimal required Broadcom kernel modules and tools extracted
# from a stock rootfs image.
#
# Must run as root on an x86_64 Linux host (e.g. a Linux build host) with
# curl, tar, unsquashfs, and mksquashfs (XZ support).
#
# Usage: sh firmware/alpine/build-alpine-rootfs.sh <stock-rootfs.bin> <out-alpine.sqfs> [bcmctl-bin]
set -eu
umask 022

[ "$(id -u)" = "0" ] || { echo "must run as root on Linux" >&2; exit 1; }
[ $# -ge 2 ] && [ $# -le 3 ] || { echo "usage: $0 <stock-rootfs.bin> <out-alpine.sqfs> [bcmctl-bin]" >&2; exit 1; }

STOCK_IMG=$1
OUT_IMG=$2
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
BCMCTL_BIN=${3:-"$SCRIPT_DIR/../../build/bcmctl"}
PKG_LIST="$SCRIPT_DIR/packages.txt"
OVERLAY="$SCRIPT_DIR/rootfs"
MAX_BYTES=44314624 # 349 LEBs (42.2 MiB) on ubi0_4 / ubi0_6

[ -f "$BCMCTL_BIN" ] || { echo "missing bcmctl binary: $BCMCTL_BIN (run 'make build-bcmopen')" >&2; exit 1; }
python3 - "$BCMCTL_BIN" <<'PY'
import struct, sys
p = sys.argv[1]
with open(p, "rb") as f:
    hdr = f.read(20)
if len(hdr) < 20 or hdr[:4] != b"\x7fELF" or hdr[4] != 1 or hdr[5] != 1:
    sys.exit(f"invalid bcmctl ELF header (expected 32-bit LE ARM ELF): {p}")
e_machine = struct.unpack_from("<H", hdr, 18)[0]
if e_machine != 40:
    sys.exit(f"wrong bcmctl architecture e_machine={e_machine} (expected 40=EM_ARM): {p}")
PY

ALPINE_VER="3.21"
ALPINE_MINI="3.21.10"
MIRROR="${ALPINE_MIRROR:-https://mirrors.tuna.tsinghua.edu.cn/alpine}"
MINIROOTFS_URL="${MIRROR}/v${ALPINE_VER}/releases/armv7/alpine-minirootfs-${ALPINE_MINI}-armv7.tar.gz"
APK_STATIC_PKG="apk-tools-static-2.14.13-r0.apk"
APK_STATIC_URL="${MIRROR}/v${ALPINE_VER}/main/x86_64/${APK_STATIC_PKG}"

for bin in curl tar unsquashfs mksquashfs; do
  command -v "$bin" >/dev/null || { echo "missing $bin" >&2; exit 1; }
done

WORK=$(mktemp -d /tmp/hg6371f-alpine.XXXXXX)
trap 'rm -rf "$WORK"' EXIT

ROOT="$WORK/root"
STOCK="$WORK/stock"
mkdir -p "$ROOT" "$WORK/apk"

echo "[1/6] fetching Alpine ${ALPINE_MINI} armv7 minirootfs and x86_64 apk.static ..."
curl -fsSL "$MINIROOTFS_URL" | tar -xzf - -C "$ROOT"
curl -fsSL "$APK_STATIC_URL" | tar -xzf - -C "$WORK/apk" sbin/apk.static 2>/dev/null
APK="$WORK/apk/sbin/apk.static"
[ -x "$APK" ] || { echo "failed to extract apk.static" >&2; exit 1; }

echo "[2/6] installing Alpine armv7 packages from packages.txt ..."
mkdir -p "$ROOT/etc/apk"
printf '%s/v%s/main\n%s/v%s/community\n' "$MIRROR" "$ALPINE_VER" "$MIRROR" "$ALPINE_VER" > "$ROOT/etc/apk/repositories"
PKGS=$(grep -vE '^[[:space:]]*(#|$)' "$PKG_LIST" | tr '\n' ' ')
# shellcheck disable=SC2086
"$APK" --root "$ROOT" --arch armv7 --no-scripts --no-cache add $PKGS

# Ensure service users skipped by --no-scripts exist cleanly in passwd/group/shadow
ensure_group() {
  grep -q "^$1:" "$ROOT/etc/group" || echo "$1:x:$2:" >> "$ROOT/etc/group"
}
ensure_user() {
  grep -q "^$1:" "$ROOT/etc/passwd" || echo "$1:x:$2:$3:$4:$5:/sbin/nologin" >> "$ROOT/etc/passwd"
  grep -q "^$1:" "$ROOT/etc/shadow" || echo "$1:!:0:0:99999:7:::" >> "$ROOT/etc/shadow"
}
ensure_group sshd 22
ensure_user sshd 22 22 "sshd" /var/empty
ensure_group chrony 123
ensure_user chrony 123 123 "chrony" /var/log/chrony
ensure_group dhcpcd 68
ensure_user dhcpcd 68 68 "dhcpcd" /var/lib/dhcpcd
mkdir -p "$ROOT/var/empty" "$ROOT/var/lib/chrony" "$ROOT/var/log/chrony" "$ROOT/var/lib/dhcpcd"
chown 123:123 "$ROOT/var/lib/chrony" "$ROOT/var/log/chrony"
chown 68:68 "$ROOT/var/lib/dhcpcd"

echo "[3/6] extracting minimal Broadcom kernel modules and installing open-source bcmctl ..."
unsquashfs -q -d "$STOCK" "$STOCK_IMG"

KVER="4.19.235"
KMOD_SRC="$STOCK/lib/modules/$KVER"
KMOD_DST="$ROOT/lib/modules/$KVER"
mkdir -p "$KMOD_DST/extra" "$KMOD_DST/kernel/drivers/base/firmware_loader" "$KMOD_DST/kernel/net/wireless"

# Curated minimal Broadcom network/RDPA/FlowCache/WFD/PCIe/WLAN modules (no GPON, no fhdrv_*, no USB, no netfilter)
BCM_EXTRA_MODS="
  bcmlibs.ko
  bcm_knvram.ko
  bdmf.ko
  rdpa_gpl.ko
  bcm_bpm.ko
  rdpa_gpl_ext.ko
  rdpa.ko
  rdpa_usr.ko
  rdpa_mw.ko
  bcm_bp3drv.ko
  gdx.ko
  bcm_ingqos.ko
  pktflow.ko
  cmdlist.ko
  sw_gso.ko
  bcm_enet.ko
  rdpa_cmd.ko
  pktrunner.ko
  bcmmcast.ko
  bcm_pcie_hcd.ko
  wfd.ko
  bcm_thermal.ko
  wlshared.ko
  hnd.ko
  emf.ko
  igs.ko
  wl.ko
  wlct.ko
"
for ko in $BCM_EXTRA_MODS; do
  [ -f "$KMOD_SRC/extra/$ko" ] || { echo "missing stock module: extra/$ko" >&2; exit 1; }
  cp "$KMOD_SRC/extra/$ko" "$KMOD_DST/extra/$ko"
done
# Patch wl.ko CSIMON Netlink protocol assignment at file offset 0x30b084:
# Stock hardcodes cmp r1,#2; movlo r1,#26; movhs r1,#24 which collides with
# kernel Netlink 24-27 (causing Netlink subsystem = -1). Reassign to:
#   cmp r1,#1; movlo r1,#28 (wl0); movhs r1,#23 (wl1)
python3 - "$KMOD_DST/extra/wl.ko" <<'PY'
import sys
p = sys.argv[1]
off = 0x30b084
orig = bytes.fromhex("020051e31a10a0331810a023")
repl = bytes.fromhex("010051e31c10a0331710a023")
with open(p, "r+b") as f:
    f.seek(off)
    cur = f.read(12)
    if cur == orig:
        f.seek(off)
        f.write(repl)
        f.seek(off)
        assert f.read(12) == repl, "wl.ko CSIMON patch verify failed"
        print("  patched wl.ko CSIMON Netlink: wl0->28, wl1->23")
    elif cur == repl:
        print("  wl.ko CSIMON Netlink already patched")
    else:
        print(f"  warning: wl.ko offset 0x30b084 mismatch ({cur.hex()}), skipping CSIMON patch")
PY
cp "$KMOD_SRC/kernel/drivers/base/firmware_loader/firmware_class.ko" "$KMOD_DST/kernel/drivers/base/firmware_loader/"
cp "$KMOD_SRC/kernel/net/wireless/cfg80211.ko" "$KMOD_DST/kernel/net/wireless/"
chown -R 0:0 "$KMOD_DST"

# Pure open-source static musl userland tools (multi-call bcmctl; zero /opt/bcm, zero glibc)
mkdir -p "$ROOT/usr/local/sbin"
cp "$BCMCTL_BIN" "$ROOT/usr/local/sbin/bcmctl"
chmod 755 "$ROOT/usr/local/sbin/bcmctl"
for tool in wl wlctl fcctl ethswctl bdmf_shell bs bcm_bootstate bcm_flashutil nvram wdtctl; do
  ln -sf bcmctl "$ROOT/usr/local/sbin/$tool"
done

# Broadcom RF calibration defaults (used by wl.ko) and stock NAND mountpoints
cp -r "$STOCK/etc/wlan" "$ROOT/etc/wlan"
chown -R 0:0 "$ROOT/usr/local/sbin" "$ROOT/etc/wlan"
mkdir -p "$ROOT/fhdata" "$ROOT/fhconf" "$ROOT/opt/upt/apps"
ln -sf /fhconf/data "$ROOT/data"

echo "[4/6] applying Alpine OpenRC/network/SSH/hostapd config overlay ..."
(
  cd "$OVERLAY"
  find . -type f ! -name '.DS_Store' ! -name '._*' | while IFS= read -r f; do
    rel=${f#./}
    dir=$(dirname "$rel")
    [ -d "$ROOT/$dir" ] || mkdir -p "$ROOT/$dir"
    rm -f "$ROOT/$rel"
    cp "$f" "$ROOT/$rel"
    chown 0:0 "$ROOT/$rel"
  done
)
chmod 755 \
  "$ROOT/usr/local/sbin/bcm-early-mount" \
  "$ROOT/usr/local/sbin/bcm-rf" \
  "$ROOT/usr/local/sbin/ap-rollback-guard" \
  "$ROOT/usr/local/sbin/ap-led" \
  "$ROOT/etc/init.d/ap-led" \
  "$ROOT/etc/init.d/bcm-hw" \
  "$ROOT/etc/init.d/ap-panel" \
  "$ROOT/etc/network/if-pre-up.d/z-brmac"

# Standard OpenRC multi-instance hostapd symlinks
ln -sf hostapd "$ROOT/etc/init.d/hostapd.wl0"
ln -sf hostapd "$ROOT/etc/init.d/hostapd.wl1"

echo "[5/6] configuring OpenRC runlevels ..."
enable_svc() {
  rl=$1
  svc=$2
  mkdir -p "$ROOT/etc/runlevels/$rl"
  [ -e "$ROOT/etc/init.d/$svc" ] || { echo "missing OpenRC service: $svc" >&2; exit 1; }
  ln -sf "/etc/init.d/$svc" "$ROOT/etc/runlevels/$rl/$svc"
}
for svc in devfs dmesg; do enable_svc sysinit "$svc"; done
for svc in hostname localmount sysctl bootmisc syslog haveged ap-led bcm-hw networking; do enable_svc boot "$svc"; done
for svc in sshd chronyd hostapd.wl0 hostapd.wl1 ap-panel local; do enable_svc default "$svc"; done
for svc in killprocs mount-ro; do enable_svc shutdown "$svc"; done

# Ensure OpenRC works cleanly with read-only / and overlay /etc
mkdir -p "$ROOT/run/openrc" "$ROOT/etc/hostapd" "$ROOT/etc/ssh" "$ROOT/root/.ssh"
chmod 700 "$ROOT/root" "$ROOT/root/.ssh"

echo "[6/6] packing XZ SquashFS (128 KiB block) ..."
rm -f "$OUT_IMG"
mksquashfs "$ROOT" "$OUT_IMG" -comp xz -b 131072 -noappend -no-progress

SIZE=$(wc -c < "$OUT_IMG" | tr -d ' ')
if [ "$SIZE" -gt "$MAX_BYTES" ]; then
  echo "image too large: $SIZE > $MAX_BYTES bytes" >&2
  exit 1
fi

echo "built $OUT_IMG ($SIZE / $MAX_BYTES bytes)"
