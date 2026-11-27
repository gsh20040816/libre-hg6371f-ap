#!/bin/sh
# Build a modified squashfs rootfs on a Linux host (must run as root to
# preserve device nodes and ownership).
# Usage: sudo sh firmware/build-rootfs.sh <base-rootfs.bin> <out-rootfs.sqfs>
set -eu
umask 022

[ "$(id -u)" = "0" ] || { echo "must run as root on Linux" >&2; exit 1; }
[ $# -eq 2 ] || { echo "usage: $0 <base-rootfs.bin> <out-rootfs.sqfs>" >&2; exit 1; }

BASE_IMG=$1
OUT_IMG=$2
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
OVERLAY="$SCRIPT_DIR/rootfs-overlay"
MAX_BYTES=44314624 # 349 LEBs (42.2 MiB) on ubi0_4 / ubi0_6

for bin in unsquashfs mksquashfs; do
  command -v "$bin" >/dev/null || { echo "missing $bin (install squashfs-tools)" >&2; exit 1; }
done

WORK=$(mktemp -d /tmp/hg6371f-rootfs.XXXXXX)
trap 'rm -rf "$WORK"' EXIT

unsquashfs -q -d "$WORK/root" "$BASE_IMG"

# Inject overlay files (root:root, without altering existing directory metadata)
(
  cd "$OVERLAY"
  find . -type f ! -name '.DS_Store' ! -name '._*' | while IFS= read -r f; do
    rel=${f#./}
    dir=$(dirname "$rel")
    [ -d "$WORK/root/$dir" ] || mkdir -p "$WORK/root/$dir"
    rm -f "$WORK/root/$rel"
    cp "$f" "$WORK/root/$rel"
    chown 0:0 "$WORK/root/$rel"
  done
)
chmod 755 "$WORK/root/usr/sbin/ap-boot-hook.sh"

# Hook at the end of /etc/rc.d/rcS3 (async, non-blocking)
RCS3="$WORK/root/etc/rc.d/rcS3"
[ -f "$RCS3" ] || { echo "missing $RCS3 in base image" >&2; exit 1; }
if ! grep -q 'ap-boot-hook.sh' "$RCS3"; then
  printf '\n[ -x /usr/sbin/ap-boot-hook.sh ] && /usr/sbin/ap-boot-hook.sh &\n' >> "$RCS3"
fi

# Kernel only enables CONFIG_SQUASHFS_XZ and CONFIG_SQUASHFS_ZLIB (no LZ4).
rm -f "$OUT_IMG"
mksquashfs "$WORK/root" "$OUT_IMG" -comp xz -b 131072 -noappend -no-progress

SIZE=$(wc -c < "$OUT_IMG" | tr -d ' ')
if [ "$SIZE" -gt "$MAX_BYTES" ]; then
  echo "image too large: $SIZE > $MAX_BYTES bytes" >&2
  exit 1
fi

echo "built $OUT_IMG ($SIZE / $MAX_BYTES bytes)"
