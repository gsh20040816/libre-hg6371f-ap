#!/usr/bin/env python3
"""Unicorn 动态执行 U-Boot 的 gunzip（0x10a7530），验证 gzip 流能否被解。

给 zlib 提供一个 bump 分配器（hook zalloc/zcalloc/zfree），
其余全部执行 U-Boot 原始机器码。

警告：FUNC / ZALLOC 等地址针对中国电信版 HG6371F（2026-05-13 固件基线）提取；
其他运营商或批次固件请先反汇编自己设备 dump 出的 uboot.bin 核对地址。
"""
import hashlib
import struct
import sys
from unicorn import *
from unicorn.arm_const import *

UBOOT = sys.argv[1] if len(sys.argv) > 1 else 'build/uboot.bin'
GZ = sys.argv[2] if len(sys.argv) > 2 else 'build/kernel.gz'
KERNEL = sys.argv[3] if len(sys.argv) > 3 else 'build/kernel.bin'

BASE = 0x1000000
IMG_SIZE = 0x400000
STACK = 0x20000000
STACK_SIZE = 0x100000
SRC = 0x30000000
DST = 0x40000000
DST_SIZE = 0x1000000
LENP = 0x31000000
GD = 0x50000000
HEAP = 0x60000000
HEAP_SIZE = 0x800000
SENTINEL = 0x0FFF0000
FUNC = 0x10A7530

ZALLOC_FH = 0x10A7324      # U-Boot 传给 zlib 的 alloc
ZFREE_FH = 0x10A7334
ZCALLOC = 0x10A4DE4        # zlib 默认
ZFREE = 0x10A4DEC


def main():
    ub = open(UBOOT, 'rb').read()
    gz = open(GZ, 'rb').read()
    expect = open(KERNEL, 'rb').read()

    mu = Uc(UC_ARCH_ARM, UC_MODE_ARM)
    for addr, size in ((BASE, IMG_SIZE), (STACK, STACK_SIZE),
                       (SRC, (len(gz) + 0xFFF) & ~0xFFF), (DST, DST_SIZE),
                       (LENP, 0x1000), (GD, 0x10000), (HEAP, HEAP_SIZE),
                       (SENTINEL, 0x1000), (0xFF800000, 0x10000)):
        try:
            mu.mem_map(addr, size)
        except UcError:
            pass
    mu.mem_write(BASE, ub)
    mu.mem_write(SRC, gz)
    mu.mem_write(LENP, struct.pack('<I', len(gz)))

    state = {'hp': HEAP, 'steps': 0, 'alloc': 0}

    def alloc(size):
        size = (size + 15) & ~15
        p = state['hp']
        state['hp'] += size
        state['alloc'] += 1
        if state['hp'] > HEAP + HEAP_SIZE:
            print("  堆耗尽!")
            raise SystemExit(1)
        return p

    def hook_code(mu, address, size, user):
        state['steps'] += 1
        if address == ZALLOC_FH or address == ZCALLOC:
            n = mu.reg_read(UC_ARM_REG_R1) * mu.reg_read(UC_ARM_REG_R2)
            mu.reg_write(UC_ARM_REG_R0, alloc(n))
            mu.reg_write(UC_ARM_REG_PC, mu.reg_read(UC_ARM_REG_LR))
        elif address == ZFREE_FH or address == ZFREE:
            mu.reg_write(UC_ARM_REG_PC, mu.reg_read(UC_ARM_REG_LR))

    mu.hook_add(UC_HOOK_CODE, hook_code)

    mu.reg_write(UC_ARM_REG_SP, STACK + STACK_SIZE - 0x100)
    mu.reg_write(UC_ARM_REG_LR, SENTINEL)
    mu.reg_write(UC_ARM_REG_R0, DST)
    mu.reg_write(UC_ARM_REG_R1, DST_SIZE)
    mu.reg_write(UC_ARM_REG_R2, SRC)
    mu.reg_write(UC_ARM_REG_R3, LENP)
    mu.reg_write(UC_ARM_REG_R9, GD)

    print(f"执行 0x{FUNC:x}: src={len(gz)} dst_max={DST_SIZE}")
    try:
        mu.emu_start(FUNC, SENTINEL, timeout=120 * 1000000, count=600_000_000)
    except UcError as e:
        print(f"异常 {e} pc={mu.reg_read(UC_ARM_REG_PC):#x} steps={state['steps']}")
        return 1

    ret = mu.reg_read(UC_ARM_REG_R0)
    out_len = struct.unpack('<I', mu.mem_read(LENP, 4))[0]
    print(f"返回 r0={ret} *lenp={out_len} 指令={state['steps']} 分配={state['alloc']} 次")

    out = bytes(mu.mem_read(DST, min(out_len, DST_SIZE)))
    ok = out == expect
    print(f"输出 {len(out)} sha256 {hashlib.sha256(out).hexdigest()[:16]}")
    print(f"期望 {len(expect)} sha256 {hashlib.sha256(expect).hexdigest()[:16]}")
    print("结果:", "✅ 完全一致" if ok else "❌ 不一致")
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
