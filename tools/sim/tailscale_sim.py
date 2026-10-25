#!/usr/bin/env python3
"""A stand-in for tailscaled's LocalAPI, for the ZeroOS simulator.

Serves the routes ZeroOS uses (GET status, PATCH prefs, POST
login-interactive) over a unix socket, with the same JSON field names as
ipnstate.Status. It walks through a real-looking login: NeedsLogin ->
AuthURL appears after login-interactive -> "approved" a few seconds later
-> Running with a handful of peers.

    python3 tools/sim/tailscale_sim.py --socket /tmp/zos-ts.sock [--state running]
    ZOS_TAILSCALE_SOCKET=/tmp/zos-ts.sock ./build/zeroos
"""
import argparse
import json
import os
import socketserver
import threading
import time
from datetime import datetime, timedelta, timezone
from http.server import BaseHTTPRequestHandler

SELF = {"ID": "nZERO11CNTRL", "HostName": "cardputer-zero", "DNSName": "cardputer-zero.tail1234.ts.net.",
        "OS": "linux", "TailscaleIPs": ["100.88.14.22", "fd7a:115c:a1e0::4f01:e16"], "Online": True}

PEERS = [
    # id, host, os, ip, online, exit option, relay, minutes since seen
    ("nNAS1CNTRL", "nas", "linux", "100.101.102.103", True, False, "ams", 0),
    ("nPIX8CNTRL", "pixel-8", "android", "100.92.17.40", True, False, "ams", 0),
    ("nEXBECNTRL", "exit-brussels", "linux", "100.70.1.9", True, True, "ams", 0),
    ("nHASSCNTRL", "homeassistant", "linux", "100.64.200.7", True, False, "fra", 0),
    ("nMACBCNTRL", "macbook", "macOS", "100.80.9.151", False, False, "ams", 190),
    ("nDESKCNTRL", "desktop-win", "windows", "100.71.33.2", False, True, "ams", 2900),
]


class State:
    def __init__(self, start):
        self.lock = threading.Lock()
        self.logged_in = start == "running"
        self.want_running = start != "stopped"
        self.auth_url = ""
        self.exit_node = ""
        self.login_started = 0.0

    def backend_state(self):
        if not self.logged_in:
            if self.login_started and time.time() - self.login_started > 6:
                self.logged_in = True          # the user "approved" in their browser
                self.auth_url = ""
                self.want_running = True
            else:
                return "NeedsLogin"
        return "Running" if self.want_running else "Stopped"

    def status(self):
        with self.lock:
            st = self.backend_state()
            now = datetime.now(timezone.utc)
            peers = {}
            if st in ("Running", "Stopped"):
                for pid, host, os_, ip, online, exit_opt, relay, mins in PEERS:
                    up = online and st == "Running"
                    peers["nodekey:" + pid.lower().ljust(64, "0")] = {
                        "ID": pid, "HostName": host, "DNSName": f"{host}.tail1234.ts.net.", "OS": os_,
                        "TailscaleIPs": [ip], "Online": up, "Active": up and host in ("nas", "exit-brussels"),
                        "ExitNode": self.exit_node == pid, "ExitNodeOption": exit_opt, "Relay": relay,
                        "RxBytes": 734003 if up else 0, "TxBytes": 120400 if up else 0,
                        "LastSeen": (now - timedelta(minutes=mins)).isoformat() if not online else "0001-01-01T00:00:00Z",
                    }
            return {
                "Version": "1.90.4-sim",
                "TUN": True,
                "BackendState": st,
                "AuthURL": self.auth_url,
                "TailscaleIPs": SELF["TailscaleIPs"] if self.logged_in else [],
                "Self": dict(SELF, Online=st == "Running") if self.logged_in else
                        {"ID": "", "HostName": "cardputer-zero", "DNSName": "", "OS": "linux", "TailscaleIPs": []},
                "Health": [] if st != "Stopped" else ["Tailscale is stopped."],
                "MagicDNSSuffix": "tail1234.ts.net",
                "CurrentTailnet": {"Name": "alice@example.com", "MagicDNSSuffix": "tail1234.ts.net",
                                   "MagicDNSEnabled": True} if self.logged_in else None,
                "Peer": peers,
                "User": {},
            }


def make_handler(state):
    class H(BaseHTTPRequestHandler):
        def log_message(self, fmt, *args):
            print("[ts-sim]", self.command, self.path, flush=True)

        def address_string(self):
            return "unix"

        def reply(self, code, obj=None, text=None):
            body = (json.dumps(obj) if obj is not None else (text or "")).encode()
            self.send_response(code)
            self.send_header("Content-Type", "application/json" if obj is not None else "text/plain")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def check(self):
            # tailscaled rejects anything that doesn't look like the CLI talking
            if self.headers.get("Host") != "local-tailscaled.sock" or self.headers.get("Origin"):
                self.reply(403, text="invalid localapi request")
                return False
            return True

        def do_GET(self):
            if not self.check():
                return
            if self.path == "/localapi/v0/status":
                self.reply(200, state.status())
            else:
                self.reply(404, text="not found")

        def do_POST(self):
            if not self.check():
                return
            if self.path == "/localapi/v0/login-interactive":
                with state.lock:
                    state.login_started = time.time()
                    state.auth_url = "https://login.tailscale.com/a/1f3e9a7c2b4d"
                self.send_response(204)
                self.end_headers()
            else:
                self.reply(404, text="not found")

        def do_PATCH(self):
            if not self.check():
                return
            n = int(self.headers.get("Content-Length") or 0)
            mp = json.loads(self.rfile.read(n) or b"{}")
            with state.lock:
                if mp.get("WantRunningSet"):
                    if mp.get("WantRunning") and not state.logged_in:
                        self.reply(400, {"error": "not logged in"})
                        return
                    state.want_running = bool(mp.get("WantRunning"))
                if mp.get("ExitNodeIDSet"):
                    want = mp.get("ExitNodeID", "")
                    if want and not any(p[0] == want and p[5] for p in PEERS):
                        self.reply(400, {"error": f"node {want} is not an exit node"})
                        return
                    state.exit_node = want
            print("[ts-sim] prefs", mp, flush=True)
            self.reply(200, {"WantRunning": state.want_running, "ExitNodeID": state.exit_node})

    return H


class UnixHTTPServer(socketserver.ThreadingMixIn, socketserver.UnixStreamServer):
    daemon_threads = True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--socket", default="/tmp/zos-tailscaled.sock")
    ap.add_argument("--state", choices=["needs-login", "running", "stopped"], default="needs-login")
    a = ap.parse_args()
    if os.path.exists(a.socket):
        os.remove(a.socket)
    srv = UnixHTTPServer(a.socket, make_handler(State(a.state)))
    print(f"[ts-sim] tailscaled LocalAPI stand-in on {a.socket} ({a.state})", flush=True)
    try:
        srv.serve_forever()
    finally:
        os.remove(a.socket)


if __name__ == "__main__":
    main()
