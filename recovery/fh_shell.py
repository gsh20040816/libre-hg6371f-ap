#!/usr/bin/env python3
"""Emergency telnet + su command runner for HG6371F.

Reads credentials from environment variables so secrets stay out of Git:
  AP_HOST        (default: 192.168.1.2)
  AP_TELNET_USER (default: telnetadmin)
  AP_TELNET_PASS (required)
  AP_SU_PASS     (required)
"""
import os
import socket
import sys
import time

HOST = os.environ.get("AP_HOST", "192.168.1.2")
USER = os.environ.get("AP_TELNET_USER", "telnetadmin")
PASS = os.environ.get("AP_TELNET_PASS", "")
SU_PASS = os.environ.get("AP_SU_PASS", "")


def strip_iac(data: bytes) -> str:
    out = bytearray()
    i = 0
    n = len(data)
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


class Shell:
    def __init__(self, host: str = HOST, timeout: float = 6.0):
        if not PASS or not SU_PASS:
            raise SystemExit("set AP_TELNET_PASS and AP_SU_PASS in environment")
        self.s = socket.create_connection((host, 23), timeout=timeout)
        self.s.settimeout(2.0)
        self._wait("Login:")
        self.s.sendall(USER.encode() + b"\r\n")
        self._wait("Password:", 5.0)
        self.s.sendall(PASS.encode() + b"\r\n")
        time.sleep(1.5)
        self.cmd("su", 1.2)
        self.cmd(SU_PASS, 1.5)

    def _wait(self, pattern: str, limit: float = 8.0) -> bool:
        end = time.time() + limit
        buf = ""
        while time.time() < end:
            try:
                chunk = self.s.recv(4096)
                if not chunk:
                    break
                buf += strip_iac(chunk)
                if pattern in buf:
                    return True
            except socket.timeout:
                continue
        return False

    def cmd(self, command: str, wait: float = 2.0) -> str:
        self.s.sendall(command.encode() + b"\r\n")
        time.sleep(wait)
        out = ""
        try:
            while True:
                chunk = self.s.recv(32768)
                if not chunk:
                    break
                out += strip_iac(chunk)
        except socket.timeout:
            pass
        lines = out.split("\r\n")
        if lines and lines[0].strip().startswith(command.split()[0]):
            lines = lines[1:]
        return "\r\n".join(lines).strip()

    def close(self) -> None:
        try:
            self.s.close()
        except Exception:
            pass


if __name__ == "__main__":
    if len(sys.argv) < 2:
        raise SystemExit("usage: fh_shell.py '<cmd>' [...]")
    sh = Shell()
    try:
        for cmd in sys.argv[1:]:
            print(sh.cmd(cmd))
    finally:
        sh.close()
