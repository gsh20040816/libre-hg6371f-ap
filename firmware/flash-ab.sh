#!/bin/sh
# Flash modified rootfs (and optional matching bootfs) into the INACTIVE A/B
# slot on HG6371F, then arm a one-shot boot into that slot.
#
# Usage: sh flash-ab.sh <rootfs.sqfs> [bootfs.bin]
set -eu

[ $# -ge 1 ] && [ $# -le 2 ] || { echo "usage: $0 <rootfs.sqfs> [bootfs.bin]" >&2; exit 1; }

ROOTFS_IMG=$1
BOOTFS_IMG=${2:-}
MAX_ROOTFS=44314624 # 349 LEBs (42.2 MiB)
MAX_BOOTFS=16252928 # 128 LEBs (15.5 MiB)
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

[ -f "$ROOTFS_IMG" ] || { echo "missing $ROOTFS_IMG" >&2; exit 1; }
[ -z "$BOOTFS_IMG" ] || [ -f "$BOOTFS_IMG" ] || { echo "missing $BOOTFS_IMG" >&2; exit 1; }

# fhdrv_kdrv_board_impl2.ko locks mtd2 ("image") at boot; temporarily unlock
# only mtd2 (leaving mtd1/loader and mtd6/factory locked) and always re-lock.
WP_BIN=""
for cand in "$SCRIPT_DIR/fh-mtd-wp" /opt/upt/apps/ap-panel/bin/fh-mtd-wp "$(command -v fh-mtd-wp 2>/dev/null || true)"; do
  if [ -n "$cand" ] && [ -x "$cand" ]; then
    WP_BIN=$cand
    break
  fi
done
if [ -e /dev/fhdrv_kdrv_board ]; then
  if [ -n "$WP_BIN" ]; then
    trap '"$WP_BIN" 2 1 2>/dev/null || true' EXIT
    "$WP_BIN" 2 0
  else
    echo "warn: fh-mtd-wp not found; ubiupdatevol/bcm_bootstate will fail with EPERM if mtd2 is write-protected" >&2
  fi
fi

CUR=$(bcm_bootstate 2>/dev/null | sed -n 's/.*Booted Partition: *\(First\|Second\).*/\1/p' | head -n 1)
case "$CUR" in
  First)
    TGT=2; TGT_WORD=Second; TGT_BOOT=/dev/ubi0_5; TGT_ROOT=/dev/ubi0_6; ONCE=8; COMMIT=7
    ;;
  Second)
    TGT=1; TGT_WORD=First; TGT_BOOT=/dev/ubi0_3; TGT_ROOT=/dev/ubi0_4; ONCE=6; COMMIT=5
    ;;
  *)
    echo "cannot determine active slot from bcm_bootstate" >&2
    exit 1
    ;;
esac

RSIZE=$(stat -c %s "$ROOTFS_IMG")
[ "$RSIZE" -le "$MAX_ROOTFS" ] || { echo "rootfs exceeds $MAX_ROOTFS bytes ($RSIZE)" >&2; exit 1; }

if [ -n "$BOOTFS_IMG" ]; then
  BSIZE=$(stat -c %s "$BOOTFS_IMG")
  [ "$BSIZE" -le "$MAX_BOOTFS" ] || { echo "bootfs exceeds $MAX_BOOTFS bytes ($BSIZE)" >&2; exit 1; }
  echo "updating inactive bootfs$TGT ($TGT_BOOT, $BSIZE bytes)..."
  ubiupdatevol "$TGT_BOOT" "$BOOTFS_IMG"
fi

# Kernel cmdline binds both ubiblock0_4 and ubiblock0_6; detach inactive one first.
if [ -b "/dev/ubiblock0_${TGT_ROOT#/dev/ubi0_}" ]; then
  ubiblock -r "$TGT_ROOT"
fi

echo "updating inactive rootfs$TGT ($TGT_ROOT, $RSIZE bytes)..."
ubiupdatevol "$TGT_ROOT" "$ROOTFS_IMG"

echo "arming one-shot boot to slot $TGT (bcm_bootstate $ONCE)..."
bcm_bootstate "$ONCE" >/dev/null

NEXT=$(bcm_bootstate 2>/dev/null | sed -n 's/.*Reboot Partition: *\(First\|Second\).*/\1/p' | head -n 1)
if [ "$NEXT" != "$TGT_WORD" ]; then
  echo "bcm_bootstate $ONCE did not take effect (Reboot Partition=${NEXT:-unknown}, want $TGT_WORD)" >&2
  exit 1
fi

echo "ready. run 'reboot' to test slot $TGT."
echo "if boot fails, power-cycle to fall back automatically."
echo "once verified, commit permanently (unlocking mtd2 first):"
echo "  fh-mtd-wp 2 0 && bcm_bootstate $COMMIT && fh-mtd-wp 2 1"
