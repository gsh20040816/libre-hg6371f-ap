#!/usr/bin/env python3
"""
Automated ARM32 ELF disassembly & ABI extractor for Broadcom /opt/bcm binaries.
Uses host llvm-objdump (/usr/bin/objdump) to dump symbol tables, annotated
disassembly with resolved literal-pool constants and ASCII strings, and
extract ioctl / netlink / command-table signatures.

Usage:
  python3 bcmopen/tools/re_disasm.py syms <elf>
  python3 bcmopen/tools/re_disasm.py func <elf> <symbol_or_hex_addr> [end_hex_addr]
  python3 bcmopen/tools/re_disasm.py strings <elf> [regex]
  python3 bcmopen/tools/re_disasm.py dump-all <elf> <out.asm>
"""
import re
import struct
import subprocess
import sys

OBJDUMP = "/usr/bin/objdump"


class ArmElf:
    def __init__(self, path):
        self.path = path
        with open(path, "rb") as f:
            self.data = f.read()
        assert self.data[:4] == b"\x7fELF" and self.data[4] == 1, "not ELF32"
        e_phoff = struct.unpack_from("<I", self.data, 28)[0]
        e_phentsize, e_phnum = struct.unpack_from("<HH", self.data, 42)
        self.segs = []
        for i in range(e_phnum):
            p_type, p_off, p_vaddr, _, p_filesz, _, p_flags, _ = struct.unpack_from(
                "<IIIIIIII", self.data, e_phoff + i * e_phentsize
            )
            if p_type == 1:
                self.segs.append((p_vaddr, p_vaddr + p_filesz, p_off, p_flags))

    def v2f(self, va):
        for s, e, o, _ in self.segs:
            if s <= va < e:
                return o + (va - s)
        return None

    def read_u32(self, va):
        fo = self.v2f(va)
        if fo is None or fo + 4 > len(self.data):
            return None
        return struct.unpack_from("<I", self.data, fo)[0]

    def read_cstr(self, va, maxlen=160):
        fo = self.v2f(va)
        if fo is None:
            return None
        end = self.data.find(b"\x00", fo, min(len(self.data), fo + maxlen))
        if end == -1 or end == fo:
            return None
        raw = self.data[fo:end]
        if all(0x20 <= b < 0x7f or b in (9, 10, 13) for b in raw):
            return raw.decode("ascii")
        return None


def annotate_asm(elf: ArmElf, text: str) -> str:
    # Annotate PC-relative ldr [pc, #imm] AND ARMv7 movw/movt pairs with resolved constants & ASCII strings
    re_ldr = re.compile(r"^\s*([0-9a-f]+):\s+[0-9a-f]+\s+ldr\s+(\w+),\s*\[pc,\s*#(-?0x[0-9a-f]+|-?\d+)\]")
    re_add_pc = re.compile(r"^\s*([0-9a-f]+):\s+[0-9a-f]+\s+add\s+(\w+),\s*pc,\s*(\w+)")
    re_movw = re.compile(r"^\s*([0-9a-f]+):\s+[0-9a-f]+\s+movw\s+(\w+),\s*#(0x[0-9a-f]+|\d+)")
    re_movt = re.compile(r"^\s*([0-9a-f]+):\s+[0-9a-f]+\s+movt\s+(\w+),\s*#(0x[0-9a-f]+|\d+)")
    reg_vals = {}
    out = []
    for line in text.splitlines():
        note = []
        m = re_ldr.match(line)
        if m:
            pc = int(m.group(1), 16)
            reg = m.group(2)
            imm = int(m.group(3), 0)
            pool_va = ((pc + 8) & ~3) + imm
            val = elf.read_u32(pool_va)
            if val is not None:
                reg_vals[reg] = val
                note.append(f"={val:#x}")
                s = elf.read_cstr(val)
                if s:
                    note.append(repr(s[:80]))
                # Also check PIC pc-relative target (pc+8 + val)
                s_pic = elf.read_cstr((pc + 8 + val) & 0xffffffff)
                if s_pic and not s:
                    note.append(f"pic:{repr(s_pic[:80])}")
        elif (mw := re_movw.match(line)):
            reg = mw.group(2)
            reg_vals[reg] = int(mw.group(3), 0) & 0xffff
        elif (mt := re_movt.match(line)):
            reg = mt.group(2)
            hi = int(mt.group(3), 0) & 0xffff
            val = (hi << 16) | (reg_vals.get(reg, 0) & 0xffff)
            reg_vals[reg] = val
            note.append(f"={val:#x}")
            s = elf.read_cstr(val)
            if s:
                note.append(repr(s[:80]))
        else:
            m2 = re_add_pc.match(line)
            if m2:
                pc = int(m2.group(1), 16)
                dst, src = m2.group(2), m2.group(3)
                if src in reg_vals:
                    target = (pc + 8 + reg_vals[src]) & 0xffffffff
                    reg_vals[dst] = target
                    s = elf.read_cstr(target)
                    note.append(f"-> {target:#x}" + (f" {repr(s[:80])}" if s else ""))
        if note:
            out.append(f"{line}    ; {' | '.join(note)}")
        else:
            out.append(line)
    return "\n".join(out) + "\n"


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)
    cmd, path = sys.argv[1], sys.argv[2]
    elf = ArmElf(path)
    if cmd == "syms":
        p = subprocess.run([OBJDUMP, "-T", path], capture_output=True, text=True, check=True)
        print(p.stdout)
    elif cmd == "func":
        target = sys.argv[3]
        args = [OBJDUMP, "-d", path]
        if target.startswith("0x"):
            args.append(f"--start-address={target}")
            if len(sys.argv) >= 5:
                args.append(f"--stop-address={sys.argv[4]}")
        else:
            args.append(f"--disassemble={target}")
        p = subprocess.run(args, capture_output=True, text=True, check=True)
        print(annotate_asm(elf, p.stdout))
    elif cmd == "strings":
        pat = re.compile(sys.argv[3]) if len(sys.argv) >= 4 else None
        p = subprocess.run(["strings", "-t", "x", path], capture_output=True, text=True, check=True)
        for line in p.stdout.splitlines():
            if not pat or pat.search(line):
                print(line)
    elif cmd == "dump-all":
        out_path = sys.argv[3]
        p = subprocess.run([OBJDUMP, "-d", path], capture_output=True, text=True, check=True)
        with open(out_path, "w") as f:
            f.write(annotate_asm(elf, p.stdout))
        print(f"wrote annotated disassembly to {out_path}")
    else:
        print(f"unknown cmd: {cmd}", file=sys.stderr)
        sys.exit(2)


if __name__ == "__main__":
    main()
