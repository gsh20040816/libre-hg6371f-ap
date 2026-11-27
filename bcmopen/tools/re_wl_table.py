#!/usr/bin/env python3
"""
re_wl_table.py — Automated ELF extractor for Broadcom stock `wl` command table.

Extracts the exact 20-byte `cmd_t` array from ARM32 `wl`:
  struct cmd_t { char *name; void *func; int32_t get; int32_t set; char *help; };

Groups commands by handler function VA and cross-references with T1/T2/T3 tiers
so simple table-driven commands (int/varint/void/mac/chanspec/hex/str) can be
auto-generated cleanly without manual guesswork.
"""
import collections
import json
import pathlib
import re
import struct
import sys

WL_ELF = pathlib.Path("build/qemu-arm/stock-root/opt/bcm/libexec/wl")
AUDIT_MD = pathlib.Path(
    "build/wl-command-audit.md"
)
OUT_JSON = pathlib.Path("build/wl_cmds_extracted.json")


def parse_elf_sections(data: bytes):
    e_shoff = struct.unpack_from("<I", data, 32)[0]
    e_shentsize, e_shnum, e_shstrndx = struct.unpack_from("<HHH", data, 46)
    shstr_off = struct.unpack_from("<I", data, e_shoff + e_shstrndx * e_shentsize + 16)[0]
    secs = {}
    for i in range(e_shnum):
        off = e_shoff + i * e_shentsize
        sh_name, _, _, sh_addr, sh_offset, sh_size, *_ = struct.unpack_from("<10I", data, off)
        end = data.index(b"\x00", shstr_off + sh_name)
        name = data[shstr_off + sh_name : end].decode("ascii", errors="replace")
        secs[name] = (sh_addr, sh_offset, sh_size)
    return secs


def extract_wl_cmds(elf_path: pathlib.Path):
    data = elf_path.read_bytes()
    secs = parse_elf_sections(data)
    ro_va, ro_off, ro_sz = secs[".rodata"]
    da_va, da_off, da_sz = secs[".data"]
    tx_va, tx_off, tx_sz = secs[".text"]

    def va_to_off(va):
        for sa, so, ss in secs.values():
            if sa and sa <= va < sa + ss:
                return so + (va - sa)
        return None

    def read_cstr(va, max_len=2048):
        if va == 0:
            return ""
        o = va_to_off(va)
        if o is None:
            return None
        e = data.find(b"\x00", o, o + max_len)
        if e < 0:
            return None
        try:
            return data[o:e].decode("ascii")
        except UnicodeDecodeError:
            return None

    # Find ALL contiguous cmd_t tables in .data (Broadcom splits wl_cmds into main + module tables!)
    tables = []
    visited = set()
    for off in range(0, da_sz - 20, 4):
        if off in visited:
            continue
        nm_va, fn_va, g, s, h_va = struct.unpack_from("<IIiiI", data, da_off + off)
        if not (ro_va <= nm_va < ro_va + ro_sz and tx_va <= fn_va < tx_va + tx_sz):
            continue
        if h_va != 0 and not (ro_va <= h_va < ro_va + ro_sz):
            continue
        if not (-1 <= g <= 1024 and -1 <= s <= 1024):
            continue
        nm = read_cstr(nm_va, 64)
        if not nm or not re.match(r"^[A-Za-z0-9_:\-]+$", nm):
            continue
        # Walk forward collecting valid 20-byte cmd_t entries
        entries = []
        pos = off
        while pos + 20 <= da_sz:
            n_va, f_va, gv, sv, hp_va = struct.unpack_from("<IIiiI", data, da_off + pos)
            if n_va == 0:
                break
            if not (ro_va <= n_va < ro_va + ro_sz and tx_va <= f_va < tx_va + tx_sz):
                break
            if hp_va != 0 and not (ro_va <= hp_va < ro_va + ro_sz):
                break
            if not (-1 <= gv <= 1024 and -1 <= sv <= 1024):
                break
            n = read_cstr(n_va, 64)
            if not n or not re.match(r"^[A-Za-z0-9_:\-]+$", n):
                break
            hp = read_cstr(hp_va, 4096) or ""
            entries.append({
                "name": n,
                "func": f"0x{f_va:06x}",
                "func_int": f_va,
                "get": gv,
                "set": sv,
                "help": hp,
                "table_va": f"0x{da_va + off:08x}",
            })
            pos += 20
        if len(entries) >= 1 and (len(entries) >= 3 or entries[0]["name"] in ("he", "twt")):
            for p_off in range(off, pos, 4):
                visited.add(p_off)
            tables.append((da_va + off, entries))

    all_cmds = {}
    for tva, entries in tables:
        for e in entries:
            if e["name"] not in all_cmds:
                all_cmds[e["name"]] = e
    return tables, all_cmds


def parse_tiers(md_path: pathlib.Path):
    if not md_path.exists():
        return {}, set(), set(), set()
    txt = md_path.read_text()
    t1 = {
        "up", "down", "isup", "ap", "cur_etheraddr", "country", "spect", "bw_cap",
        "he", "chanspec", "status", "txpwr1", "chanim_stats", "counters", "assoclist",
        "sta_info", "rssi", "scan", "scanresults", "frameburst", "wme_ac",
        "txpwr_percent", "twt", "wme_apsd", "csimon", "dump", "sta_monitor",
    }
    # Extract T2 from section 5 and T3 from section 6
    s5 = txt.split("## 5. T2")[1].split("## 6. T3")[0]
    s6 = txt.split("## 6. T3")[1].split("## 7. T4")[0]
    t2 = set(re.findall(r"`([A-Za-z0-9_:\-]+)`", s5)) - {"wl", "wl0", "wl1", "hostapd", "tcpdump", "CN/947"}
    t3 = set(re.findall(r"`([A-Za-z0-9_:\-]+)`", s6)) - {"wl", "hostapd"}
    # Keep only real command tokens (no uppercase-only prose tokens)
    t2 -= t1
    t3 -= (t1 | t2)
    tier_map = {}
    for c in t1: tier_map[c] = "T1"
    for c in t2: tier_map[c] = "T2"
    for c in t3: tier_map[c] = "T3"
    return tier_map, t1, t2, t3


def main():
    tables, all_cmds = extract_wl_cmds(WL_ELF)
    tier_map, t1, t2, t3 = parse_tiers(AUDIT_MD)

    print(f"Found {len(tables)} cmd_t tables in .data, total unique commands: {len(all_cmds)}")
    for tva, entries in tables:
        print(f"  table @ 0x{tva:08x}: {len(entries):3d} cmds (first={entries[0]['name']}, last={entries[-1]['name']})")

    # Filter tiers to commands actually in all_cmds
    t1_in = sorted(t1 & set(all_cmds))
    t2_in = sorted(t2 & set(all_cmds))
    t3_in = sorted(t3 & set(all_cmds))
    print(f"\nTier hits in ELF: T1={len(t1_in)}/{len(t1)}, T2={len(t2_in)}/{len(t2)}, T3={len(t3_in)}/{len(t3)}")
    missing_t1 = sorted(t1 - set(all_cmds))
    if missing_t1:
        print("  T1 missing from cmd_t (generic IOVARs?):", missing_t1)

    # Group ALL commands by handler VA (top shared handlers!)
    by_fn_all = collections.defaultdict(list)
    by_fn_t12 = collections.defaultdict(list)
    by_fn_t123 = collections.defaultdict(list)
    for nm, e in all_cmds.items():
        by_fn_all[e["func"]].append(nm)
        if nm in t1 or nm in t2:
            by_fn_t12[e["func"]].append(nm)
        if nm in t1 or nm in t2 or nm in t3:
            by_fn_t123[e["func"]].append(nm)

    print(f"\nUnique handler VAs: all={len(by_fn_all)}, T1+T2={len(by_fn_t12)}, T1+T2+T3={len(by_fn_t123)}")
    print("\nTop 25 shared handler VAs across ALL commands (and their T1/T2/T3 counts):")
    for fn, names in sorted(by_fn_all.items(), key=lambda kv: -len(kv[1]))[:25]:
        t12_n = len(by_fn_t12[fn])
        t123_n = len(by_fn_t123[fn])
        sample = [(n, all_cmds[n]["get"], all_cmds[n]["set"]) for n in sorted(by_fn_t123[fn] or names)[:5]]
        print(f"  {fn}: all={len(names):3d}  T1+T2={t12_n:3d}  T1-T3={t123_n:3d}  sample={sample}")

    # Generate bcmopen/lib/wl_table.h covering ALL generic table-driven T1+T2+T3 commands
    GEN_KIND = {
        "0x0423f8": "WL_K_VARINT",
        "0x02edb0": "WL_K_INT",
        "0x03f43c": "WL_K_VAR_VOID",
        "0x02ef14": "WL_K_BSSCFG_INT",
        "0x041030": "WL_K_VOID",
        "0x0419d8": "WL_K_ETHERADDR",
        "0x03e490": "WL_K_VAR_GETSTR",
        "0x02b774": "WL_K_VAR_STR",
        "0x036de4": "WL_K_CHANSPECS",
        "0x041b24": "WL_K_MAC_IOCTL",
    }
    hdr = pathlib.Path("bcmopen/lib/wl_table.h")
    rows = []
    for nm, e in sorted(all_cmds.items()):
        tier = tier_map.get(nm, "T4-T7")
        if tier not in ("T1", "T2", "T3"):
            continue
        kind = GEN_KIND.get(e["func"])
        if not kind:
            continue
        first_line = (e["help"].splitlines()[0] if e["help"] else "").strip()
        first_line = first_line.replace("\\", "\\\\").replace('"', '\\"')[:96]
        rows.append((nm, kind, e["get"], e["set"], tier, first_line))

    with hdr.open("w") as f:
        f.write("/* Auto-generated by bcmopen/tools/re_wl_table.py from stock ARM32 wl ELF.\n")
        f.write(" * Covers ALL generic table-driven T1+T2+T3 commands with exact stock ioctl IDs.\n")
        f.write(" */\n")
        f.write("#ifndef BCMOPEN_WL_TABLE_H\n#define BCMOPEN_WL_TABLE_H\n\n")
        f.write("typedef enum {\n")
        f.write("    WL_K_VARINT = 1,\n    WL_K_INT,\n    WL_K_VAR_VOID,\n    WL_K_BSSCFG_INT,\n")
        f.write("    WL_K_VOID,\n    WL_K_ETHERADDR,\n    WL_K_VAR_GETSTR,\n    WL_K_VAR_STR,\n")
        f.write("    WL_K_CHANSPECS,\n    WL_K_MAC_IOCTL,\n} wl_gen_kind_t;\n\n")
        f.write("typedef struct {\n")
        f.write("    const char   *name;\n    wl_gen_kind_t kind;\n    int           get_cmd;\n")
        f.write("    int           set_cmd;\n    const char   *tier;\n    const char   *help;\n")
        f.write("} wl_gen_entry_t;\n\n")
        f.write("static const wl_gen_entry_t WL_GEN_TABLE[] = {\n")
        for nm, kind, g, s, tier, hp in rows:
            f.write(f'    {{ "{nm}", {kind}, {g}, {s}, "{tier}", "{hp}" }},\n')
        f.write("};\n\n#endif /* BCMOPEN_WL_TABLE_H */\n")
    print(f"Generated {hdr} with {len(rows)} generic T1+T2+T3 commands")

    # Save full extracted table
    OUT_JSON.parent.mkdir(parents=True, exist_ok=True)
    export = {
        "tables": len(tables),
        "total_unique": len(all_cmds),
        "commands": {
            nm: {**e, "tier": tier_map.get(nm, "T4-T7")}
            for nm, e in sorted(all_cmds.items())
        },
    }
    OUT_JSON.write_text(json.dumps(export, indent=2))
    print(f"\nSaved {OUT_JSON}")


if __name__ == "__main__":
    main()
