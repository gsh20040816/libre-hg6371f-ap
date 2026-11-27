#!/usr/bin/env python3
"""End-to-end A/B rootfs deployment pipeline for HG6371F.

Validates a SquashFS rootfs image locally, uploads and flashes it into the
INACTIVE A/B slot (with optional matching bootfs), arms one-shot boot, reboots,
waits for the new boot to pass health checks, and permanently commits the slot.

Usage:
  python3 firmware/deploy-rootfs.py <rootfs.sqfs> [options]

Examples:
  # Local validation only (does not touch the AP)
  python3 firmware/deploy-rootfs.py /tmp/rootfs-new.sqfs --check-only

  # Full flow: validate -> flash inactive slot -> one-shot reboot -> verify -> commit
  python3 firmware/deploy-rootfs.py /tmp/rootfs-new.sqfs

  # Flash and verify on the new slot, but stop before permanent commit
  python3 firmware/deploy-rootfs.py /tmp/rootfs-new.sqfs --no-commit
"""
import argparse
import os
import re
import shutil
import struct
import subprocess
import sys
import time
from pathlib import Path

MAX_ROOTFS = 44314624  # 349 LEBs (42.2 MiB) on ubi0_4 / ubi0_6
MAX_BOOTFS = 16252928  # 128 LEBs (15.5 MiB) on ubi0_3 / ubi0_5
SQUASHFS_MAGIC = 0x73717368  # "hsqs" (little-endian)
COMP_NAMES = {1: "gzip", 2: "lzma", 3: "lzo", 4: "xz", 5: "lz4", 6: "zstd"}
SUPPORTED_COMPS = {1, 4}  # Kernel enables CONFIG_SQUASHFS_XZ and CONFIG_SQUASHFS_ZLIB only

REPO_ROOT = Path(__file__).resolve().parent.parent
FLASH_AB_SH = REPO_ROOT / "firmware" / "flash-ab.sh"
LOCAL_WP_BIN = REPO_ROOT / "build" / "fh-mtd-wp"
REMOTE_WP_BIN = "/opt/upt/apps/ap-panel/bin/fh-mtd-wp"

SSH_BASE = [
    "ssh",
    "-o", "BatchMode=yes",
    "-o", "ConnectTimeout=6",
    "-o", "ServerAliveInterval=10",
    "-o", "ServerAliveCountMax=3",
]


def log(msg: str) -> None:
    ts = time.strftime("%H:%M:%S")
    print(f"[{ts}] {msg}", flush=True)


def die(msg: str, code: int = 1) -> None:
    print(f"ERROR: {msg}", file=sys.stderr, flush=True)
    raise SystemExit(code)


def validate_rootfs_local(path: Path) -> None:
    if not path.is_file():
        die(f"rootfs image not found: {path}")
    size = path.stat().st_size
    if size < 96:
        die(f"rootfs image too small ({size} bytes)")
    if size > MAX_ROOTFS:
        die(f"rootfs exceeds volume limit: {size} > {MAX_ROOTFS} bytes (349 LEBs)")

    with path.open("rb") as f:
        sb = f.read(96)
    magic, inodes, _mkfs_time, block_size, _frags, comp, _b_log, _flags, _ids, major, minor, _root_inode, bytes_used = (
        struct.unpack("<IIIIIHHHHHHQQ", sb[:48])
    )
    if magic != SQUASHFS_MAGIC:
        die(f"invalid SquashFS magic: 0x{magic:08x} (expected 0x{SQUASHFS_MAGIC:08x})")
    if (major, minor) != (4, 0):
        die(f"unsupported SquashFS version {major}.{minor} (expected 4.0)")
    comp_name = COMP_NAMES.get(comp, f"unknown({comp})")
    if comp not in SUPPORTED_COMPS:
        die(f"unsupported SquashFS compression '{comp_name}' (kernel only supports xz/zlib)")
    if block_size != 131072:
        die(f"unexpected SquashFS block size {block_size} (expected 131072)")
    if inodes < 100:
        die(f"suspiciously low inode count: {inodes}")
    if bytes_used <= 96 or bytes_used > size:
        die(f"corrupt SquashFS superblock: bytes_used={bytes_used}, file_size={size}")

    log(f"superblock OK: SquashFS 4.0, comp={comp_name}, block={block_size}, inodes={inodes}, size={size}/{MAX_ROOTFS}")

    unsquashfs = shutil.which("unsquashfs")
    if not unsquashfs:
        log("note: unsquashfs not found locally; skipped deep inode/permission check")
        return

    res = subprocess.run([unsquashfs, "-ll", str(path)], capture_output=True, text=True)
    if res.returncode != 0:
        die(f"unsquashfs -ll failed:\n{res.stderr.strip()}")

    entries = {}
    symlinks = {}
    ds_stores = []
    for line in res.stdout.splitlines():
        parts = line.split()
        if len(parts) < 6:
            continue
        mode, owner = parts[0], parts[1]
        # Regular/dir lines have: mode owner size date time path [-> target]
        # Device lines have: mode owner major, minor date time path
        if parts[2].endswith(","):
            entry_path = parts[6] if len(parts) >= 7 else ""
            target = ""
        else:
            entry_path = parts[5]
            target = parts[7] if len(parts) >= 8 and parts[6] == "->" else ""
        if not entry_path:
            continue
        entries[entry_path] = (mode, owner)
        if mode.startswith("l") and target:
            symlinks[entry_path] = target
        if ".DS_Store" in entry_path:
            ds_stores.append(entry_path)

    if ds_stores:
        die(f"found macOS junk in rootfs image: {ds_stores[:5]}")

    for must_root in ("squashfs-root", "squashfs-root/etc", "squashfs-root/usr", "squashfs-root/usr/sbin"):
        if must_root not in entries:
            die(f"missing required directory in rootfs: {must_root}")
        _, owner = entries[must_root]
        if owner not in ("root/root", "0/0"):
            die(f"bad directory ownership on {must_root}: {owner} (expected root/root)")

    for must_exist in ("squashfs-root/etc/inittab", "squashfs-root/bin/busybox"):
        if must_exist not in entries:
            die(f"missing required file in rootfs: {must_exist}")

    is_alpine = "squashfs-root/sbin/openrc" in entries and "squashfs-root/lib/ld-musl-armhf.so.1" in entries
    if is_alpine:
        for must_alpine in (
            "squashfs-root/usr/local/sbin/bcm-early-mount",
            "squashfs-root/usr/local/sbin/bcm-rf",
            "squashfs-root/etc/bcm-rf.conf",
            "squashfs-root/usr/local/sbin/ap-rollback-guard",
            "squashfs-root/usr/local/sbin/bcmctl",
            "squashfs-root/usr/local/sbin/wdtctl",
            "squashfs-root/usr/local/sbin/wl",
            "squashfs-root/usr/local/sbin/wlctl",
            "squashfs-root/usr/local/sbin/bcm_bootstate",
            "squashfs-root/usr/local/sbin/bcm_flashutil",
            "squashfs-root/usr/local/sbin/fcctl",
            "squashfs-root/usr/local/sbin/nvram",
            "squashfs-root/usr/local/sbin/ethswctl",
            "squashfs-root/usr/local/sbin/bdmf_shell",
            "squashfs-root/usr/local/sbin/bs",
            "squashfs-root/etc/network/if-pre-up.d/bridge",
            "squashfs-root/sbin/dhcpcd",
            "squashfs-root/etc/ssl/certs/ca-certificates.crt",
        ):
            if must_alpine not in entries:
                die(f"missing required Alpine/bcmctl file in rootfs: {must_alpine}")
        bcm_mode, bcm_owner = entries["squashfs-root/usr/local/sbin/bcmctl"]
        if bcm_owner not in ("root/root", "0/0") or not bcm_mode.startswith("-rwx"):
            die(f"expected regular executable root-owned bcmctl, got {bcm_mode} {bcm_owner}")
        for tool in ("wdtctl", "wl", "wlctl", "bcm_bootstate", "bcm_flashutil",
                     "fcctl", "nvram", "ethswctl", "bdmf_shell", "bs"):
            tp = f"squashfs-root/usr/local/sbin/{tool}"
            tmode, _ = entries[tp]
            if not tmode.startswith("l") or symlinks.get(tp) != "bcmctl":
                die(f"expected {tp} -> bcmctl symlink, got {tmode} -> {symlinks.get(tp)!r}")
        forbidden = [
            p for p in entries
            if p == "squashfs-root/lib/ld-linux.so.3"
            or p == "squashfs-root/etc/get_rootfs_dev.sh"
            or p == "squashfs-root/opt/bcm"
            or p.startswith("squashfs-root/opt/bcm/")
        ]
        if forbidden:
            die(f"closed-source /opt/bcm or glibc ld-linux remnants found in open-userland Alpine image: {forbidden[:5]}")
        bb_mode, bb_owner = entries.get("squashfs-root/bin/bbsuid", entries["squashfs-root/bin/busybox"])
        if bb_owner not in ("root/root", "0/0") or "s" not in bb_mode[:4]:
            die(f"expected setuid root bbsuid/busybox, got {bb_mode} {bb_owner}")
        log(f"deep check OK (Alpine OpenRC + open bcmctl): root dirs owned by root, bcmctl={bcm_mode}, bbsuid={bb_mode}, zero /opt/bcm & zero ld-linux.so.3")
    else:
        if "squashfs-root/etc/rc.d/rcS3" not in entries:
            die("missing required file in stock-based rootfs: squashfs-root/etc/rc.d/rcS3")
        bb_mode, bb_owner = entries["squashfs-root/bin/busybox"]
        if not bb_mode.startswith("-rws") or bb_owner not in ("root/root", "0/0"):
            die(f"/bin/busybox must be setuid root (4755), got {bb_mode} {bb_owner}")
        has_hook = "squashfs-root/usr/sbin/ap-boot-hook.sh" in entries
        log(f"deep check OK: root dirs owned by root, busybox={bb_mode}, ap-boot-hook={'present' if has_hook else 'absent'}")


def validate_bootfs_local(path: Path) -> None:
    if not path.is_file():
        die(f"bootfs image not found: {path}")
    size = path.stat().st_size
    if size < 64 or size > MAX_BOOTFS:
        die(f"bootfs size out of range: {size} (max {MAX_BOOTFS})")
    with path.open("rb") as f:
        magic = struct.unpack(">I", f.read(4))[0]
    if magic != 0xD00DFEED:
        die(f"invalid bootfs FIT magic: 0x{magic:08x} (expected 0xd00dfeed)")
    log(f"bootfs OK: valid FIT header, size={size}/{MAX_BOOTFS}")


def ssh_run(host: str, cmd: str, check: bool = True, timeout: int = 30) -> subprocess.CompletedProcess:
    res = subprocess.run(SSH_BASE + [host, cmd], capture_output=True, text=True, timeout=timeout)
    if check and res.returncode != 0:
        die(f"remote command failed (rc={res.returncode}): {cmd}\n{res.stdout}{res.stderr}")
    return res


def ssh_upload(host: str, local_path: Path, remote_path: str, executable: bool = False) -> None:
    size = local_path.stat().st_size
    log(f"uploading {local_path.name} ({size} bytes) -> {host}:{remote_path} ...")
    remote_cmd = f"cat > '{remote_path}'" + (f" && chmod 755 '{remote_path}'" if executable else "")
    with local_path.open("rb") as f:
        res = subprocess.run(SSH_BASE + [host, remote_cmd], stdin=f, capture_output=True, text=True, timeout=600)
    if res.returncode != 0:
        die(f"upload failed for {local_path}: {res.stderr.strip()}")
    chk = ssh_run(host, f"stat -c %s '{remote_path}'")
    got = int(chk.stdout.strip())
    if got != size:
        die(f"upload size mismatch on {remote_path}: local={size}, remote={got}")


def parse_bootstate(text: str) -> dict:
    info = {}
    m = re.search(r"Booted Partition:\s*(First|Second)", text)
    if m:
        info["booted"] = m.group(1)
    m = re.search(r"Reboot Partition:\s*(First|Second)", text)
    if m:
        info["reboot"] = m.group(1)
    m = re.search(r"committed\s+(\d+)", text)
    if m:
        info["committed"] = int(m.group(1))
    for part in ("First", "Second"):
        m = re.search(rf"{part}\s+partition commit flag\s*:\s*(\d+)", text)
        if m:
            info[f"commit_{part.lower()}"] = int(m.group(1))
    if "committed" not in info:
        if info.get("commit_first") == 1:
            info["committed"] = 1
        elif info.get("commit_second") == 1:
            info["committed"] = 2
    return info


def main() -> int:
    ap = argparse.ArgumentParser(description="Validate, flash, one-shot boot, verify, and commit HG6371F rootfs")
    ap.add_argument("rootfs", type=Path, help="Path to SquashFS rootfs image (.sqfs)")
    ap.add_argument("--bootfs", type=Path, default=None, help="Optional matching bootfs FIT image (.bin)")
    ap.add_argument("--host", default="root@192.168.1.2", help="AP SSH target (default: root@192.168.1.2)")
    ap.add_argument("--timeout", type=int, default=240, help="Seconds to wait for AP reboot + SSH (default: 240)")
    ap.add_argument("--no-commit", action="store_true", help="Verify new slot after reboot, but do not permanently commit")
    ap.add_argument("--check-only", action="store_true", help="Only validate image(s) locally; do not touch AP")
    args = ap.parse_args()

    # 1. Local pre-flight validation
    validate_rootfs_local(args.rootfs)
    if args.bootfs is not None:
        validate_bootfs_local(args.bootfs)
    if args.check_only:
        log("check-only complete")
        return 0
    if not FLASH_AB_SH.is_file():
        die(f"missing helper script: {FLASH_AB_SH}")

    # 2. Query current active slot & pre-reboot boot_id
    log(f"probing active slot on {args.host} ...")
    probe = ssh_run(args.host, "cat /proc/sys/kernel/random/boot_id && bcm_bootstate")
    lines = probe.stdout.splitlines()
    old_boot_id = lines[0].strip() if lines else ""
    bs = parse_bootstate("\n".join(lines[1:]))
    booted = bs.get("booted")
    if booted == "First":
        tgt, tgt_word, tgt_ubiblock, commit_code = 2, "Second", "/dev/ubiblock0_6", 7
    elif booted == "Second":
        tgt, tgt_word, tgt_ubiblock, commit_code = 1, "First", "/dev/ubiblock0_4", 5
    else:
        die(f"cannot determine active slot from bcm_bootstate:\n{probe.stdout}")
    log(f"active slot is {booted} (committed={bs.get('committed')}); target inactive slot = {tgt} ({tgt_word})")

    # Ensure fh-mtd-wp is installed on AP
    has_wp = ssh_run(args.host, f"test -x '{REMOTE_WP_BIN}'", check=False).returncode == 0
    if not has_wp:
        if LOCAL_WP_BIN.is_file():
            ssh_upload(args.host, LOCAL_WP_BIN, REMOTE_WP_BIN, executable=True)
        else:
            die(f"missing {REMOTE_WP_BIN} on AP and {LOCAL_WP_BIN} locally (run 'make build-mtd-wp' first)")

    # 3. Upload & flash inactive slot (one-shot)
    remote_root = "/tmp/deploy-rootfs.sqfs"
    remote_boot = "/tmp/deploy-bootfs.bin"
    remote_flash = "/tmp/deploy-flash-ab.sh"
    try:
        ssh_upload(args.host, FLASH_AB_SH, remote_flash, executable=True)
        ssh_upload(args.host, args.rootfs, remote_root)
        flash_cmd = f"sh '{remote_flash}' '{remote_root}'"
        if args.bootfs is not None:
            ssh_upload(args.host, args.bootfs, remote_boot)
            flash_cmd += f" '{remote_boot}'"
        log(f"flashing inactive slot {tgt} and arming one-shot boot ...")
        fl = ssh_run(args.host, flash_cmd, timeout=180)
        for ln in fl.stdout.splitlines():
            if ln.strip():
                log(f"  ap: {ln}")
    finally:
        ssh_run(args.host, f"rm -f '{remote_root}' '{remote_boot}' '{remote_flash}'", check=False)

    # 4. Trigger reboot & wait for new boot_id
    log("rebooting AP into one-shot slot (typically takes 80-120s) ...")
    ssh_run(args.host, "sync && (sleep 1; reboot) </dev/null >/dev/null 2>&1 &", check=False, timeout=10)

    deadline = time.time() + args.timeout
    time.sleep(45)
    new_boot_id = ""
    while time.time() < deadline:
        try:
            r = subprocess.run(
                SSH_BASE + [args.host, "cat /proc/sys/kernel/random/boot_id"],
                capture_output=True, text=True, timeout=8,
            )
            if r.returncode == 0:
                bid = r.stdout.strip()
                if bid and bid != old_boot_id:
                    new_boot_id = bid
                    break
        except subprocess.TimeoutExpired:
            pass
        time.sleep(5)

    if not new_boot_id:
        die(f"timed out after {args.timeout}s waiting for AP SSH after reboot; NOT committed (old slot remains safe)")
    log(f"AP is back online (new boot_id={new_boot_id[:8]}...), verifying slot {tgt} health ...")

    # 5. Post-boot health verification (allow up to 60s for wl1 / panel to settle)
    health_cmd = (
        "echo BOOT_START; bcm_bootstate; echo MOUNT_START; mount | grep ' / '; "
        "echo CMDLINE_START; cat /proc/cmdline; "
        "echo NET_START; netstat -lnt 2>/dev/null | grep -E ':(22|80|8081)[[:space:]]' || true; "
        "echo WL_START; echo wl0=$(wl -i wl0 isup 2>/dev/null) wl1=$(wl -i wl1 isup 2>/dev/null); "
        "echo BB_START; (test -u /bin/busybox || test -u /bin/bbsuid) && echo BB_SETUID=1 || echo BB_SETUID=0; "
        "echo DMESG_START; dmesg | grep -E 'Kernel panic|Oops:' | tail -n 5 || true"
    )
    verify_deadline = time.time() + 60
    last_reason = "unknown"
    while True:
        res = ssh_run(args.host, health_cmd, timeout=20)
        out = res.stdout
        cur_bs = parse_bootstate(out)
        if cur_bs.get("booted") != tgt_word:
            die(f"AP booted into {cur_bs.get('booted')} instead of {tgt_word} (rolled back); NOT committed")
        if f"{tgt_ubiblock} on / " not in out:
            die(f"root mount mismatch (expected {tgt_ubiblock} on /); NOT committed:\n{out}")
        if f"img_index={tgt}" not in out:
            die(f"cmdline missing img_index={tgt}; NOT committed")
        if "BB_SETUID=1" not in out:
            die("/bin/busybox lost setuid bit; NOT committed")
        dmesg_tail = out.split("DMESG_START", 1)[1].strip()
        if dmesg_tail:
            die(f"kernel Oops/panic detected in dmesg; NOT committed:\n{dmesg_tail}")

        net_section = out.split("NET_START", 1)[1].split("WL_START", 1)[0]
        net_ok = (":8081 " in net_section) or (":80 " in net_section) or (":80\t" in net_section) or (":8081\t" in net_section)
        wl_ok = "wl0=1 wl1=1" in out
        if net_ok and wl_ok:
            log(f"health OK: slot={tgt} ({tgt_word}), root={tgt_ubiblock}, ssh+panel up, wl0/wl1 up, setuid OK")
            ssh_run(args.host, "test -x /usr/local/sbin/ap-rollback-guard && touch /tmp/ap-clean-ok || true", check=False, timeout=10)
            break
        last_reason = f"panel_http={net_ok}, wl0_wl1_up={wl_ok}"
        if time.time() >= verify_deadline:
            die(f"post-boot health check timed out ({last_reason}); NOT committed")
        log(f"waiting for services to settle ({last_reason}) ...")
        time.sleep(5)

    # 6. Permanent commit
    if args.no_commit:
        log(f"--no-commit specified: slot {tgt} verified but NOT permanently committed")
        log(f"to commit manually: {REMOTE_WP_BIN} 2 0 && bcm_bootstate {commit_code} && {REMOTE_WP_BIN} 2 1")
        return 0

    log(f"committing slot {tgt} permanently (bcm_bootstate {commit_code}) ...")
    commit_cmd = (
        f"set -eu; "
        f"if [ -e /dev/fhdrv_kdrv_board ]; then "
        f"  trap '{REMOTE_WP_BIN} 2 1 2>/dev/null || true' EXIT; '{REMOTE_WP_BIN}' 2 0; "
        f"fi; "
        f"bcm_bootstate {commit_code} >/dev/null; "
        f"if [ -e /dev/fhdrv_kdrv_board ]; then '{REMOTE_WP_BIN}' 2 1; fi; "
        f"if [ -x /usr/local/sbin/ap-rollback-guard ]; then touch /fhconf/ap_alpine_committed /tmp/ap-clean-ok; fi; "
        f"bcm_bootstate"
    )
    cres = ssh_run(args.host, commit_cmd, timeout=20)
    final_bs = parse_bootstate(cres.stdout)
    want_flag = final_bs.get(f"commit_{tgt_word.lower()}")
    if final_bs.get("booted") != tgt_word or final_bs.get("reboot") != tgt_word or want_flag != 1:
        die(f"commit verification failed: {final_bs}\n{cres.stdout}")

    log(f"DONE: slot {tgt} ({tgt_word}) is now permanently committed (committed={final_bs.get('committed')})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
