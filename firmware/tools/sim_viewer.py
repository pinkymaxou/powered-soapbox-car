#!/usr/bin/env python3
"""sim_viewer.py — Visualisateur 3D TEMPS RÉEL de la simulation du kart.

Compile (si besoin) le binaire de simulation, sert la page 3D (sim_viewer.html) et relaie
le flux JSON du scénario choisi en Server-Sent Events — même logique de contrôle et même
physique que les tests automatisés, mais cadencées au temps réel et visibles à l'écran.

    python3 tools/sim_viewer.py                     # écoute sur TOUTES les interfaces (0.0.0.0)
    python3 tools/sim_viewer.py --port 9000         # port personnalisé
    python3 tools/sim_viewer.py --host 127.0.0.1    # restreindre à la machine locale
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


def build_sim() -> None:
    """(Re)compile le binaire de simulation depuis les MÊMES sources que les tests."""
    cmd = [
        "g++", "-std=c++17", "-O2", "-I", str(FW / "main"), "-I", str(FW / "test_host/sim"),
        str(FW / "test_host/sim_main.cpp"),
        str(FW / "main/controller_core.cpp"),
        str(FW / "main/config_params.cpp"),
        "-o", str(SIM_BIN),
    ]
    print("compilation :", " ".join(cmd))
    subprocess.run(cmd, check=True)


def scenario_list():
    out = subprocess.run([str(SIM_BIN), "--list"], capture_output=True, text=True, check=True)
    scen = []
    for line in out.stdout.splitlines():
        name, _, desc = line.partition(" ")
        scen.append({"name": name.strip(), "desc": desc.strip()})
    return scen


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a):   # silencieux
        pass

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
        """SSE : relaie chaque ligne JSON du simulateur --realtime vers le navigateur."""
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
            pass   # le navigateur a changé de scénario / fermé l'onglet
        finally:
            proc.kill()


def main():
    import argparse
    ap = argparse.ArgumentParser(description="Visualisateur 3D de la simulation du kart")
    ap.add_argument("--host", default="0.0.0.0", help="interface d'écoute (défaut : toutes)")
    ap.add_argument("--port", type=int, default=8650)
    ap.add_argument("--rebuild", action="store_true", help="recompiler le binaire de simulation")
    args = ap.parse_args()

    if args.rebuild or not SIM_BIN.exists():
        build_sim()

    print(f"Visualisateur en écoute sur {args.host}:{args.port} — ouvrables :")
    print(f"  http://localhost:{args.port}/")
    if args.host == "0.0.0.0":
        ips = subprocess.run(["hostname", "-I"], capture_output=True, text=True).stdout.split()
        for ip in ips:
            print(f"  http://{ip}:{args.port}/")
    print("(Ctrl-C pour quitter)")
    ThreadingHTTPServer((args.host, args.port), Handler).serve_forever()


if __name__ == "__main__":
    main()
