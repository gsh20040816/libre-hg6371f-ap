#!/usr/bin/env python3
"""
Automated side-by-side live AP regression harness.
Uploads build/bcmctl to /tmp/bcmctl on the AP (read-only against system files)
and compares stdout, stderr, and exit codes of stock /opt/bcm/bin/<tool>
against /tmp/bcmctl <tool> across a suite of safe read-only commands.

Usage:
  python3 bcmopen/tools/ap_diff.py [--host ap.example.local] [--suite m1|m2|all]
"""
import argparse
import difflib
import re
import subprocess
import sys

M1_TESTS = [
    ("fcctl status", ["fcctl", "status"], "fcache_counters"),
    ("fcctl flwstats p", ["fcctl", "flwstats", "p"], None),
    ("fcctl mcaststats", ["fcctl", "mcaststats"], "volatile_or_exact"),
    ("fcctl invalid", ["fcctl", "nonexistent_cmd"], None),
    ("bcm_bootstate read", ["bcm_bootstate"], "bootstate_rd"),
    ("bcm_bootstate invalid", ["bcm_bootstate", "999"], None),
    ("bcm_flashutil help", ["bcm_flashutil"], None),
    ("bcm_flashutil imageversion 1", ["bcm_flashutil", "1", "-1", "-1", "imageversion"], None),
    ("bcm_flashutil imageversion 2", ["bcm_flashutil", "2", "-1", "-1", "imageversion"], None),
    ("wdtctl timeleft", ["wdtctl", "timeleft"], "volatile"),
    ("wdtctl usage", ["wdtctl"], None),
    ("nvram kget boardid", ["nvram", "kget", "boardid"], None),
    ("nvram kget 1:aa2g", ["nvram", "kget", "1:aa2g"], None),
    ("nvram kget missing", ["nvram", "kget", "__nonexistent_key__"], None),
    ("nvram kshow", ["nvram", "kshow"], None),
    ("nvram show", ["nvram", "show"], None),
    ("ethswctl help", ["ethswctl", "-h"], None),
    ("ethswctl hw-switching", ["ethswctl", "-c", "hw-switching"], None),
    ("ethswctl getifname 0 0 (miss)", ["ethswctl", "getifname", "0", "0"], None),
    ("ethswctl getifname 0 1 (lan1)", ["ethswctl", "getifname", "0", "1"], None),
    ("ethswctl getifname 0 4 (lan0)", ["ethswctl", "getifname", "0", "4"], None),
    ("ethswctl getwanonly", ["ethswctl", "-c", "getwanonly"], None),
    ("ethswctl getwanpreferred", ["ethswctl", "-c", "getwanpreferred"], None),
    ("ethswctl getlanonly", ["ethswctl", "-c", "getlanonly"], None),
    ("ethswctl getlanwan", ["ethswctl", "-c", "getlanwan"], None),
    ("ethswctl wan get", ["ethswctl", "-c", "wan"], None),
    ("ethswctl portctrl 0 1 (lan1)", ["ethswctl", "-c", "portctrl", "-n", "0", "-p", "1"], None),
    ("ethswctl portctrl 0 4 (lan0)", ["ethswctl", "-c", "portctrl", "-n", "0", "-p", "4"], None),
    ("ethswctl pause 0 1 (lan1)", ["ethswctl", "-c", "pause", "-n", "0", "-p", "1"], None),
    ("ethswctl pause 0 4 (lan0)", ["ethswctl", "-c", "pause", "-n", "0", "-p", "4"], None),
    ("ethswctl softswitch", ["ethswctl", "-c", "softswitch"], None),
    ("ethswctl phymode 0 1 (lan1)", ["ethswctl", "-c", "phymode", "-n", "0", "-p", "1"], None),
    ("ethswctl phymode 0 4 (lan0)", ["ethswctl", "-c", "phymode", "-n", "0", "-p", "4"], None),
    ("ethswctl portloopback 0 1", ["ethswctl", "-c", "portloopback", "-n", "0", "-p", "1"], None),
    ("ethswctl portloopback 0 4", ["ethswctl", "-c", "portloopback", "-n", "0", "-p", "4"], None),
    ("ethswctl portloopback 0 0 (miss)", ["ethswctl", "-c", "portloopback", "-n", "0", "-p", "0"], None),
    ("ethswctl dosctrl 0", ["ethswctl", "-c", "dosctrl", "-n", "0"], None),
    ("ethswctl snoopctrl 0", ["ethswctl", "-c", "snoopctrl", "-n", "0"], None),
    ("ethswctl snoopctrl 0 -h", ["ethswctl", "-c", "snoopctrl", "-n", "0", "-h"], None),
    ("ethswctl rxratectrl 0 1", ["ethswctl", "-c", "rxratectrl", "-n", "0", "-p", "1"], None),
    ("ethswctl txratectrl 0 1", ["ethswctl", "-c", "txratectrl", "-n", "0", "-p", "1"], "err_rc_stderr"),
    ("ethswctl cosqpriomethod 0 1", ["ethswctl", "-c", "cosqpriomethod", "-n", "0", "-p", "1"], None),
    ("ethswctl cosqsched 0", ["ethswctl", "-c", "cosqsched", "-n", "0"], None),
    ("ethswctl bondchk lan0 lan1", ["ethswctl", "-c", "bondchk", "-i", "lan0", "-j", "lan1"], None),
    ("ethswctl bondchk eth0 eth1", ["ethswctl", "-c", "bondchk", "-i", "eth0", "-j", "eth1"], None),
    ("bdmf_shell no-args", ["bdmf_shell"], None),
    ("bdmf_shell missing-sock", ["bdmf_shell", "-h"], None),
]


def build_m2_tests(host):
    """Strictly read-only wl regression suite on live wl0 (2.4G) and wl1 (5G)."""
    tests = []
    for iface, band in (("wl0", "2g"), ("wl1", "5g")):
        p = ["wl", "-i", iface]
        tests.extend([
            (f"wl {iface} isup", p + ["isup"], None),
            (f"wl {iface} ap", p + ["ap"], None),
            (f"wl {iface} chanspec", p + ["chanspec"], None),
            (f"wl {iface} country", p + ["country"], None),
            (f"wl {iface} spect", p + ["spect"], None),
            (f"wl {iface} bw_cap {band}", p + ["bw_cap", band], None),
            (f"wl {iface} he", p + ["he"], None),
            (f"wl {iface} twt", p + ["twt"], None),
            (f"wl {iface} txpwr1", p + ["txpwr1"], None),
            (f"wl {iface} txpwr_percent", p + ["txpwr_percent"], None),
            (f"wl {iface} wme_ac ap", p + ["wme_ac", "ap"], None),
            (f"wl {iface} wme_apsd", p + ["wme_apsd"], None),
            (f"wl {iface} frameburst", p + ["frameburst"], None),
            (f"wl {iface} cur_etheraddr", p + ["cur_etheraddr"], None),
            (f"wl {iface} ver", p + ["ver"], None),
            (f"wl {iface} revinfo", p + ["revinfo"], None),
            (f"wl {iface} cap", p + ["cap"], None),
            (f"wl {iface} band", p + ["band"], None),
            (f"wl {iface} bands", p + ["bands"], None),
            (f"wl {iface} channel", p + ["channel"], None),
            (f"wl {iface} channels", p + ["channels"], None),
            (f"wl {iface} radio", p + ["radio"], None),
            (f"wl {iface} bss", p + ["bss"], None),
            (f"wl {iface} bssid", p + ["bssid"], None),
            (f"wl {iface} ssid", p + ["ssid"], None),
            (f"wl {iface} bssmax", p + ["bssmax"], None),
            (f"wl {iface} chanim_mode", p + ["chanim_mode"], None),
            (f"wl {iface} chan_info " + ("13" if iface == "wl0" else "36"), p + ["chan_info", "13" if iface == "wl0" else "36"], None),
            (f"wl {iface} dfs_status", p + ["dfs_status"], "mask_nums"),
            (f"wl {iface} txpwr_cap", p + ["txpwr_cap"], "err_nonzero"),
            (f"wl {iface} txpwr_target_max", p + ["txpwr_target_max"], None),
            (f"wl {iface} txpwrlimit", p + ["txpwrlimit"], "err_nonzero"),
            (f"wl {iface} country_rev CN", p + ["country_rev", "CN"], None),
            (f"wl {iface} memuse", p + ["memuse"], "err_nonzero"),
            (f"wl {iface} mbssid", p + ["mbssid"], None),
            (f"wl {iface} wnm", p + ["wnm"], None),
            (f"wl {iface} chanspecs", p + ["chanspecs"], None),
            (f"wl {iface} chanim_stats all", p + ["chanim_stats", "all"], "mask_nums"),
            (f"wl {iface} csimon", p + ["csimon"], None),
            (f"wl {iface} assoclist", p + ["assoclist"], None),
            (f"wl {iface} authe_sta_list", p + ["authe_sta_list"], None),
            (f"wl {iface} autho_sta_list", p + ["autho_sta_list"], None),
            (f"wl {iface} noise", p + ["noise"], "mask_nums"),
            (f"wl {iface} phy_tempsense", p + ["phy_tempsense"], "mask_nums"),
            (f"wl {iface} chanim_stats", p + ["chanim_stats"], "mask_nums"),
            (f"wl {iface} sta_monitor dump", p + ["sta_monitor", "dump"], "mask_nums"),
            (f"wl {iface} dump csimon", p + ["dump", "csimon"], "mask_nums"),
            (f"wl {iface} pktcnt", p + ["pktcnt"], "mask_nums"),
            (f"wl {iface} wme_counters", p + ["wme_counters"], "mask_nums"),
            (f"wl {iface} status", p + ["status"], "wl_status"),
            (f"wl {iface} counters", p + ["counters"], "wl_counters"),
        ])
        # Probe first associated STA for read-only rssi + sta_info
        al = ssh_exec(host, f"/opt/bcm/bin/wl -i {iface} assoclist")
        for line in al.stdout.splitlines():
            parts = line.strip().split()
            if len(parts) == 2 and parts[0] == "assoclist":
                mac = parts[1]
                tests.append((f"wl {iface} rssi {mac}", p + ["rssi", mac], "mask_nums"))
                tests.append((f"wl {iface} sta_info {mac}", p + ["sta_info", mac], "wl_sta_info"))
                break
    return tests


def ssh_exec(host, cmd):
    return subprocess.run(
        ["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=5", host, cmd],
        capture_output=True,
        text=True,
    )


def upload(host, local_bin):
    with open(local_bin, "rb") as f:
        p = subprocess.run(
            ["ssh", "-o", "BatchMode=yes", host, "cat > /tmp/bcmctl.new && chmod 755 /tmp/bcmctl.new && mv /tmp/bcmctl.new /tmp/bcmctl"],
            stdin=f,
            capture_output=True,
        )
    if p.returncode != 0:
        sys.stderr.write(f"upload failed: {p.stderr.decode(errors='replace')}\n")
        sys.exit(1)


def run_pair(host, argv):
    # Quote safely for POSIX sh
    def q(s):
        return "'" + s.replace("'", "'\\''") + "'"
    tool = argv[0]
    rest = " ".join(q(a) for a in argv[1:])
    # Run stock
    r_stock = ssh_exec(host, f"/opt/bcm/bin/{tool} {rest}")
    # Run open-source via argv[0] symlink in /tmp/bcmopen-bin/<tool>
    r_open = ssh_exec(host, f"mkdir -p /tmp/bcmopen-bin && ln -sf /tmp/bcmctl /tmp/bcmopen-bin/{tool} && /tmp/bcmopen-bin/{tool} {rest}")
    return r_stock, r_open


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="root@192.168.1.2")
    ap.add_argument("--bin", default="build/bcmctl")
    ap.add_argument("--suite", default="m1")
    args = ap.parse_args()

    upload(args.host, args.bin)
    if args.suite == "m1":
        tests = M1_TESTS
    elif args.suite == "m2":
        tests = build_m2_tests(args.host)
    else:
        tests = M1_TESTS + build_m2_tests(args.host)
    passed = 0
    failed = 0
    for name, argv, mode in tests:
        rs, ro = run_pair(args.host, argv)
        ok_rc = (rs.returncode == ro.returncode)
        # Normalize argv[0] paths in usage messages
        s_out = rs.stdout.replace("/opt/bcm/libexec/", "<BIN>/").replace("/opt/bcm/bin/", "<BIN>/")
        o_out = ro.stdout.replace("/tmp/bcmopen-bin/", "<BIN>/").replace("/tmp/bcmctl", "<BIN>/" + argv[0])
        s_err = rs.stderr.replace("/opt/bcm/libexec/", "<BIN>/").replace("/opt/bcm/bin/", "<BIN>/")
        o_err = ro.stderr.replace("/tmp/bcmopen-bin/", "<BIN>/").replace("/tmp/bcmctl", "<BIN>/" + argv[0])
        if mode == "fcache_counters":
            pat = re.compile(r"Active<\d+>, Cumulative \[ \d+ - \d+ \]")
            s_out = pat.sub("Active<N>, Cumulative [ C - E ]", s_out)
            o_out = pat.sub("Active<N>, Cumulative [ C - E ]", o_out)
        elif mode == "bootstate_rd":
            # Strip stock's redundant debug rd_metadata lines (even mid-line after 'Boot image state: ')
            s_out = re.sub(r"rd_metadata:[^\n]*\n", "", s_out)
            o_out = re.sub(r"rd_metadata:[^\n]*\n", "", o_out)
        elif mode == "volatile_or_exact":
            # Mask live packet/byte counters that tick between back-to-back invocations
            pat = re.compile(r"=\d+")
            s_out = pat.sub("=N", s_out)
            o_out = pat.sub("=N", o_out)
        elif mode == "mask_nums":
            # Mask live ticking hex/integers (hex FIRST so 0x3e -> N, not NxNe)
            pat = re.compile(r"0x[0-9a-fA-F]+|-?\d+")
            s_out = pat.sub("N", s_out)
            o_out = pat.sub("N", o_out)
        elif mode == "wl_status":
            # Mask volatile noise/RSSI/QBSS utilization numbers; check full output structure
            pat = re.compile(r"(noise:\s*)-?\d+|(QBSS Channel Utilization:\s*)0x[0-9a-fA-F]+\s*\(\d+\s*%\)")
            s_out = pat.sub(lambda m: (m.group(1) + "N") if m.group(1) else (m.group(2) + "0xN (N %)"), s_out)
            o_out = pat.sub(lambda m: (m.group(1) + "N") if m.group(1) else (m.group(2) + "0xN (N %)"), o_out)
        elif mode == "wl_counters":
            # Verify all core counter keys present and non-empty
            s_keys = set(re.findall(r"\b([a-zA-Z][a-zA-Z0-9_]+)\s+\d+", s_out))
            o_keys = set(re.findall(r"\b([a-zA-Z][a-zA-Z0-9_]+)\s+\d+", o_out))
            must = {"txframe", "txbyte", "txretrans", "txerror", "rxframe", "rxbyte", "rxerror", "txfail"}
            ok_out = must.issubset(o_keys) and (s_err == o_err)
            if ok_rc and ok_out:
                print(f"[PASS] {name} (rc={ro.returncode}, keys={len(o_keys)}/{len(s_keys)})")
                passed += 1
            else:
                print(f"[FAIL] {name}: stock rc={rs.returncode}, open rc={ro.returncode}, missing={must - o_keys}")
                failed += 1
            continue
        elif mode == "wl_sta_info":
            # Verify all fields consumed by panel/server/wifi.go + core STA fields match structure
            must = ["idle", "in network", "flags", "smoothed rssi", "rate of last tx pkt", "rate of last rx pkt",
                    "tx total pkts", "rx data pkts", "tx total bytes", "rx data bytes"]
            missing = [k for k in must if k not in o_out]
            ok_out = (len(missing) == 0) and (s_err == o_err)
            if ok_rc and ok_out:
                print(f"[PASS] {name} (rc={ro.returncode}, all {len(must)} key fields present)")
                passed += 1
            else:
                print(f"[FAIL] {name}: stock rc={rs.returncode}, open rc={ro.returncode}, missing={missing}")
                print("  -- stock --\n" + s_out[:600])
                print("  -- open --\n" + o_out[:600])
                failed += 1
            continue
        if mode == "err_nonzero":
            ok_rc = (rs.returncode == ro.returncode) or (rs.returncode != 0 and ro.returncode != 0)
        if mode == "volatile":
            ok_out = True
        elif mode == "err_rc_stderr":
            # When kernel returns error (rc != 0), stock prints uninitialized stack garbage on stdout
            ok_out = (s_err == o_err) if ro.returncode != 0 else (s_out == o_out and s_err == o_err)
        else:
            ok_out = (s_out == o_out and s_err == o_err)
        if ok_rc and ok_out:
            print(f"[PASS] {name} (rc={ro.returncode})")
            passed += 1
        else:
            print(f"[FAIL] {name}: stock rc={rs.returncode}, open rc={ro.returncode}")
            if not ok_out:
                for line in difflib.unified_diff(
                    s_out.splitlines(), o_out.splitlines(), fromfile="stock.stdout", tofile="open.stdout", lineterm=""
                ):
                    print("  " + line)
                for line in difflib.unified_diff(
                    s_err.splitlines(), o_err.splitlines(), fromfile="stock.stderr", tofile="open.stderr", lineterm=""
                ):
                    print("  " + line)
            failed += 1
    ssh_exec(args.host, "rm -rf /tmp/bcmctl /tmp/bcmopen-bin")
    print(f"\nSummary: {passed} passed, {failed} failed")
    sys.exit(0 if failed == 0 else 1)


if __name__ == "__main__":
    main()
