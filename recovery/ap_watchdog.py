#!/usr/bin/env python3
"""Auto-recovery watchdog for HG6371F AP, intended to run on the N100 main router.

Checks TCP reachability of Dropbear (:22) and Go panel (:8081) on the AP.
When both are up, exits immediately. If either is down while the vendor web
(:8080) is reachable (e.g. right after an AP reboot), enables telnet via the
local IPv6 link-local /fh_tool/api endpoint, launches start.sh over telnet+su,
verifies recovery, and disables telnet again.

Required environment variables (keep in a chmod 600 env file, never in Git):
  AP_MAC          12-char uppercase MAC without separators
  AP_TELNET_PASS  telnetadmin password
  AP_SU_PASS      su password
Optional:
  AP_HOST         default 192.168.1.2
  AP_IFACE        default br-lan
  AP_TELNET_USER  default telnetadmin
"""
import fcntl
import hashlib
import json
import os
import socket
import subprocess
import sys
import time

HOST = os.environ.get("AP_HOST", "192.168.1.2")
IFACE = os.environ.get("AP_IFACE", "br-lan")
MAC = os.environ.get("AP_MAC", "").replace(":", "").replace("-", "").upper()
USER = os.environ.get("AP_TELNET_USER", "telnetadmin")
PASS = os.environ.get("AP_TELNET_PASS", "")
SU_PASS = os.environ.get("AP_SU_PASS", "")
LOCK_PATH = "/var/lock/ap-watchdog.lock"


def tcp_open(host: str, port: int, timeout: float = 2.0) -> bool:
    try:
        with socket.create_connection((host, port), timeout=timeout):
            return True
    except OSError:
        return False


def strip_iac(data: bytes) -> str:
    out = bytearray()
    i, n = 0, len(data)
    while i < n:
        if data[i] == 255 and i + 1 < n:
            if data[i + 1] in (251, 252, 253, 254) and i + 2 < n:
                i += 3
                continue
            if data[i + 1] == 250:
                j = data.find(b"\xff\xf0", i)
                i = (j + 2) if j > 0 else i + 2
                continue
            i += 2
            continue
        out.append(data[i])
        i += 1
    return bytes(out).decode("utf-8", "replace")


def fh_tool_api(payload: dict) -> str:
    digest = hashlib.sha256(MAC.encode()).hexdigest()
    key_hex = "".join(digest[2 * i + 2] for i in range(16)).encode().hex()
    iv_hex = "".join(digest[3 * i + 3] for i in range(16)).encode().hex()
    raw = json.dumps(payload, separators=(",", ":")).encode()
    body = subprocess.run(
        ["openssl", "enc", "-aes-128-cbc", "-K", key_hex, "-iv", iv_hex, "-base64", "-A"],
        input=raw,
        capture_output=True,
        check=True,
    ).stdout.decode().strip()
    url = f"http://[fe80::100%25{IFACE}]:8080/fh_tool/api"
    resp = subprocess.run(
        ["curl", "-sS", "-g", "-m", "10", "--interface", IFACE,
         "-H", "Content-Type: text/plain", "--data-binary", body, url],
        capture_output=True,
        text=True,
        check=True,
    ).stdout.strip()
    if not resp:
        return ""
    dec = subprocess.run(
        ["openssl", "enc", "-d", "-aes-128-cbc", "-K", key_hex, "-iv", iv_hex, "-base64", "-A"],
        input=resp.encode(),
        capture_output=True,
    )
    return dec.stdout.decode("utf-8", "replace").strip()


def run_start_via_telnet() -> None:
    s = socket.create_connection((HOST, 23), timeout=6.0)
    s.settimeout(2.0)

    def wait_for(pattern: str, limit: float = 8.0) -> bool:
        end = time.time() + limit
        buf = ""
        while time.time() < end:
            try:
                chunk = s.recv(4096)
                if not chunk:
                    break
                buf += strip_iac(chunk)
                if pattern in buf:
                    return True
            except socket.timeout:
                continue
        return False

    def send_cmd(cmd: str, wait: float = 1.5) -> None:
        s.sendall(cmd.encode() + b"\r\n")
        time.sleep(wait)
        try:
            while s.recv(32768):
                pass
        except socket.timeout:
            pass

    try:
        if not wait_for("Login:"):
            raise RuntimeError("telnet Login prompt timeout")
        s.sendall(USER.encode() + b"\r\n")
        if not wait_for("Password:", 5.0):
            raise RuntimeError("telnet Password prompt timeout")
        s.sendall(PASS.encode() + b"\r\n")
        time.sleep(1.5)
        send_cmd("su", 1.2)
        send_cmd(SU_PASS, 1.5)
        send_cmd("setsid sh /opt/upt/apps/ap-panel/start.sh </dev/null >/tmp/ap-watchdog-start.log 2>&1 &", 2.0)
    finally:
        s.close()


def main() -> int:
    if tcp_open(HOST, 22) and tcp_open(HOST, 8081):
        return 0

    os.makedirs(os.path.dirname(LOCK_PATH), exist_ok=True)
    lock_fd = open(LOCK_PATH, "w")
    try:
        fcntl.flock(lock_fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        return 0

    if not tcp_open(HOST, 8080):
        return 0  # AP is offline or still rebooting

    if not (MAC and PASS and SU_PASS):
        print("ap-watchdog: missing AP_MAC / AP_TELNET_PASS / AP_SU_PASS", file=sys.stderr)
        return 1

    print(f"ap-watchdog: {HOST} missing :22 or :8081, triggering recovery...")
    fh_tool_api({"index": "1", "func": "TelnetEnable", "telnet": "1"})
    time.sleep(1.5)
    try:
        run_start_via_telnet()
    finally:
        try:
            fh_tool_api({"index": "1", "func": "TelnetEnable", "telnet": "0"})
        except Exception:
            pass

    for _ in range(5):
        time.sleep(2.0)
        if tcp_open(HOST, 22) and tcp_open(HOST, 8081):
            print(f"ap-watchdog: {HOST} recovered (:22 and :8081 up)")
            return 0

    print(f"ap-watchdog: recovery attempted, :22={tcp_open(HOST, 22)} :8081={tcp_open(HOST, 8081)}", file=sys.stderr)
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
