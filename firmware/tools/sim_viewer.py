#!/usr/bin/env python3
"""sim_viewer.py — REAL-TIME 3D viewer of the kart simulation.

Builds (if needed) the simulation binary, serves the 3D page (sim_viewer.html) and relays
the JSON stream of the chosen scenario over Server-Sent Events — same control logic and same
physics as the automated tests, but paced in real time and visible on screen.

    python3 tools/sim_viewer.py                     # listen on ALL interfaces (0.0.0.0)
    python3 tools/sim_viewer.py --port 9000         # custom port
    python3 tools/sim_viewer.py --host 127.0.0.1    # restrict to the local machine
"""
import json
import pathlib
import subprocess
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

HERE = pathlib.Path(__file__).resolve().parent
FW = HERE.parent
SIM_BIN = pathlib.Path("/tmp/kart_sim_viewer_bin")


def sim_stale() -> bool:
    """Binary missing or older than one of the sources → rebuild (avoids driving
    stale physics on an up-to-date scene…)."""
    if not SIM_BIN.exists():
        return True
    bin_mtime = SIM_BIN.stat().st_mtime
    sources = [FW / "test_host/sim_main.cpp", FW / "main/controller_core.cpp",
               FW / "main/controller_core.hpp", FW / "main/config_params.cpp",
               FW / "main/control_types.hpp", FW / "main/control_math.hpp",
               FW / "main/pid.hpp"]
    sources += (FW / "test_host/sim").glob("*.hpp")
    return any(s.stat().st_mtime > bin_mtime for s in sources)


def build_sim() -> None:
    """(Re)builds the simulation binary from the SAME sources as the tests."""
    cmd = [
        "g++", "-std=c++17", "-O2", "-I", str(FW / "main"), "-I", str(FW / "test_host/sim"),
        str(FW / "test_host/sim_main.cpp"),
        str(FW / "main/controller_core.cpp"),
        str(FW / "main/config_params.cpp"),
        "-o", str(SIM_BIN),
    ]
    print("compiling:", " ".join(cmd))
    subprocess.run(cmd, check=True)


def scenario_list():
    out = subprocess.run([str(SIM_BIN), "--list"], capture_output=True, text=True, check=True)
    scen = []
    for line in out.stdout.splitlines():
        name, _, desc = line.partition(" ")
        scen.append({"name": name.strip(), "desc": desc.strip()})
    return scen


DRIVE = {"proc": None}   # manual driving process currently running (only one at a time)


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a):   # silent
        pass

    def do_POST(self):
        # Browser keyboard inputs → simulator stdin in driving mode.
        if urlparse(self.path).path != "/input":
            self.send_error(404)
            return
        n = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(n).decode()
        proc = DRIVE["proc"]
        ok = False
        if proc and proc.poll() is None:
            try:
                proc.stdin.write(body + "\n")
                proc.stdin.flush()
                ok = True
            except (BrokenPipeError, OSError):
                pass
        self.send_response(200 if ok else 409)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Content-Length", "0")
        self.end_headers()

    def do_GET(self):
        url = urlparse(self.path)
        if url.path in ("/", "/index.html"):
            body = (HERE / "sim_viewer.html").read_bytes()
            self.send_response(200)
            self.send_header("Access-Control-Allow-Origin", "*")
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        elif url.path == "/chart.js":
            body = (FW / "main/assets/chart.min.js").read_bytes()
            self.send_response(200)
            self.send_header("Content-Type", "text/javascript")
            self.send_header("Cache-Control", "max-age=86400")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        elif url.path == "/scenarios":
            body = json.dumps(scenario_list()).encode()
            self.send_response(200)
            self.send_header("Access-Control-Allow-Origin", "*")
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        elif url.path == "/stream":
            name = parse_qs(url.query).get("scenario", ["virage_pleine_vitesse"])[0]
            self.stream(name)
        else:
            self.send_error(404)

    def stream(self, name: str):
        """SSE: relays each JSON line from the simulator to the browser.
        The pseudo-scenario __drive__ = MANUAL driving: stdin open for the keyboard."""
        if name == "__drive__":
            proc = subprocess.Popen([str(SIM_BIN), "--drive"],
                                    stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
            old = DRIVE["proc"]
            if old and old.poll() is None:
                old.kill()
            DRIVE["proc"] = proc
        else:
            proc = subprocess.Popen([str(SIM_BIN), "--stream", name, "--realtime"],
                                    stdout=subprocess.PIPE, text=True)
        self.send_response(200)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-cache")
        self.end_headers()
        try:
            for line in proc.stdout:
                self.wfile.write(b"data: " + line.strip().encode() + b"\n\n")
                self.wfile.flush()
            self.wfile.write(b"event: end\ndata: {}\n\n")
            self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError):
            pass   # the browser switched scenario / closed the tab
        finally:
            proc.kill()


def main():
    import argparse
    ap = argparse.ArgumentParser(description="3D viewer of the kart simulation")
    ap.add_argument("--host", default="0.0.0.0", help="listening interface (default: all)")
    ap.add_argument("--port", type=int, default=8650)
    ap.add_argument("--rebuild", action="store_true", help="rebuild the simulation binary")
    args = ap.parse_args()

    if args.rebuild or sim_stale():
        build_sim()

    print(f"Viewer listening on {args.host}:{args.port} — open at:")
    print(f"  http://localhost:{args.port}/")
    if args.host == "0.0.0.0":
        ips = subprocess.run(["hostname", "-I"], capture_output=True, text=True).stdout.split()
        for ip in ips:
            print(f"  http://{ip}:{args.port}/")
    print("(Ctrl-C to quit)")
    ThreadingHTTPServer((args.host, args.port), Handler).serve_forever()


if __name__ == "__main__":
    main()
