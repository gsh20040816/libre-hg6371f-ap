#!/usr/bin/env python3
"""
Local macOS QEMU ARMv7 Linux Syscall-Mock Diff Harness (Zero AP Touch!).

1. Cross-compiles build/bcmctl and build/bcmopen-obj/vm_mock_runner for arm-linux-musleabihf.
2. Packs a minimal newc gzipped initramfs containing:
   - /init (vm_mock_runner)
   - /opt/bcm/libexec/*, /opt/bcm/lib/*, /lib/ld-linux.so.3 (cached stock binaries)
   - /open/bcmctl + multi-call symlinks
   - /fixtures/metadata1.bin, /fixtures/metadata2.bin (real device backups)
3. Boots qemu-system-arm (-M virt -cpu cortex-a7) headless with Alpine armv7 kernel,
   executes all 60+ read & write syscall-mock side-by-side tests, and parses VM_SUMMARY.
"""
import gzip
import os
import pathlib
import stat
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
QEMU_DIR = ROOT / "build" / "qemu-arm"
KERNEL = QEMU_DIR / "vmlinuz-lts"
STOCK_ROOT = QEMU_DIR / "stock-root"
INITRAMFS = QEMU_DIR / "initramfs.cpio.gz"
RUNNER_BIN = ROOT / "build" / "bcmopen-obj" / "vm_mock_runner"
BCMCTL_BIN = ROOT / "build" / "bcmctl"

TOOLS = [
    "wdtctl", "fcctl", "nvram", "bcm_bootstate",
    "bcm_flashutil", "ethswctl", "bdmf_shell", "bs",
    "wl", "wlctl",
]


class CpioBuilder:
    def __init__(self):
        self.chunks = []
        self.ino = 1

    def _pad4(self, n):
        return b"\x00" * ((4 - (n % 4)) % 4)

    def _entry(self, name: str, mode: int, data: bytes = b""):
        name_b = name.lstrip("/").encode("utf-8") + b"\x00"
        hdr = (
            f"070701"
            f"{self.ino:08x}"
            f"{mode:08x}"
            f"{0:08x}{0:08x}"
            f"{1:08x}{0:08x}"
            f"{len(data):08x}"
            f"{0:08x}{0:08x}{0:08x}{0:08x}"
            f"{len(name_b):08x}"
            f"{0:08x}"
        ).encode("ascii")
        self.ino += 1
        self.chunks.append(hdr + name_b + self._pad4(len(hdr) + len(name_b)))
        if data:
            self.chunks.append(data + self._pad4(len(data)))

    def add_dir(self, path: str, mode: int = 0o755):
        self._entry(path, stat.S_IFDIR | mode)

    def add_file(self, path: str, data: bytes, mode: int = 0o755):
        self._entry(path, stat.S_IFREG | mode, data)

    def add_symlink(self, path: str, target: str):
        self._entry(path, stat.S_IFLNK | 0o777, target.encode("utf-8"))

    def finish(self) -> bytes:
        self._entry("TRAILER!!!", 0)
        return b"".join(self.chunks)


def build_initramfs():
    cpio = CpioBuilder()
    for d in [
        "bin", "etc", "proc", "sys", "dev", "tmp", "var", "var/run", "lib",
        "opt", "opt/bcm", "opt/bcm/lib", "opt/bcm/libexec", "opt/bcm/bin",
        "open", "fixtures",
    ]:
        cpio.add_dir(d)

    cpio.add_file("init", RUNNER_BIN.read_bytes(), 0o755)
    cpio.add_symlink("bin/sh", "/init")
    cpio.add_file("etc/get_rootfs_dev.sh", b"#!/bin/sh\necho /dev/ubiblock0_6\n", 0o755)
    cpio.add_file("open/bcmctl", BCMCTL_BIN.read_bytes(), 0o755)
    for t in TOOLS:
        cpio.add_symlink(f"open/{t}", "bcmctl")

    # Stock libs & binaries
    lib_dir = STOCK_ROOT / "opt" / "bcm" / "lib"
    for p in sorted(lib_dir.iterdir()):
        if p.is_file():
            cpio.add_file(f"opt/bcm/lib/{p.name}", p.read_bytes(), 0o755)
    cpio.add_symlink("lib/ld-linux.so.3", "/opt/bcm/lib/ld-linux.so.3")

    libexec_dir = STOCK_ROOT / "opt" / "bcm" / "libexec"
    for p in sorted(libexec_dir.iterdir()):
        if p.is_file():
            cpio.add_file(f"opt/bcm/libexec/{p.name}", p.read_bytes(), 0o755)

    # Real metadata backups
    for fn in ("metadata1.bin", "metadata2.bin"):
        src = ROOT / "backups" / fn
        cpio.add_file(f"fixtures/{fn}", src.read_bytes(), 0o644)

    raw = cpio.finish()
    with gzip.open(INITRAMFS, "wb", compresslevel=1) as gz:
        gz.write(raw)


def main():
    if not KERNEL.exists():
        sys.stderr.write(f"Missing kernel {KERNEL}\n")
        sys.exit(1)
    if not (STOCK_ROOT / "opt" / "bcm" / "lib" / "libc.so.6").exists():
        sys.stderr.write(f"Missing stock rootfs cache {STOCK_ROOT}\n")
        sys.exit(1)

    # Compile vm_mock_runner statically for ARMv7 musl
    RUNNER_BIN.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(
        [
            "zig", "cc", "-target", "arm-linux-musleabihf", "-mcpu=cortex_a7",
            "-std=c99", "-D_GNU_SOURCE", "-Wall", "-Wextra", "-Os",
            "-static", "-s",
            str(ROOT / "bcmopen" / "tools" / "vm_mock_runner.c"),
            "-o", str(RUNNER_BIN),
        ],
        check=True,
    )

    build_initramfs()

    cmd = [
        "qemu-system-arm",
        "-M", "virt",
        "-cpu", "cortex-a7",
        "-m", "256",
        "-nographic",
        "-no-reboot",
        "-kernel", str(KERNEL),
        "-initrd", str(INITRAMFS),
        "-append", "console=ttyAMA0 quiet panic=-1 rdinit=/init",
    ]
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
    out = p.stdout
    # Strip kernel boot noise before marker if present
    marker = "=== QEMU ARMv7 SYSCALL-MOCK DIFF SUITE ==="
    if marker in out:
        out = out[out.index(marker):]
    print(out)
    if "VM_SUMMARY:" not in out:
        sys.stderr.write("VM did not complete test suite!\n")
        if p.stderr:
            sys.stderr.write(p.stderr)
        sys.exit(1)
    for line in out.splitlines():
        if line.startswith("VM_SUMMARY:"):
            if ", 0 failed" in line:
                sys.exit(0)
            sys.exit(1)
    sys.exit(1)


if __name__ == "__main__":
    main()
