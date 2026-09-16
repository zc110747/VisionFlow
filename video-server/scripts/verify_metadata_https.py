#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
HTTPS metadata TLS verification (camera-agent --metadata-insecure).

The camera-agent can POST AI metadata to an https metadata server. By default
WinHTTP enforces certificate-chain validation, so a dev server fronted by a
self-signed certificate is rejected. The --metadata-insecure flag
(cfg.skip_tls_verify) relaxes that for exactly such dev setups. This script
proves BOTH behaviours with the REAL agent against a REAL self-signed HTTPS
endpoint (stdlib http.server + an openssl-minted cert):

  Run A (insecure ON):  agent --metadata --metadata-insecure -> https://...
                        => metadata arrives (>=1 frame/status POST, mock 204)
  Run B (insecure OFF): agent --metadata -> https://... (self-signed)
                        => default TLS validation BLOCKS it: the mock receives
                           nothing, the agent logs the TLS error and keeps
                           retrying WITHOUT crashing (secure default intact)

Stdlib only. openssl is required to mint the self-signed certificate.
"""

import argparse
import http.server
import json
import os
import shutil
import socket
import ssl
import subprocess
import sys
import threading
import time

PASS = 0
FAIL = 0
INFO = 0


def check(name, ok, detail=""):
    global PASS, FAIL
    if ok:
        PASS += 1
        print(f"  [PASS] {name}" + (f" :: {detail}" if detail else ""))
    else:
        FAIL += 1
        print(f"  [FAIL] {name}" + (f" :: {detail}" if detail else ""))
    return ok


def info(name, detail=""):
    global INFO
    INFO += 1
    print(f"  [INFO] {name}" + (f" :: {detail}" if detail else ""))


def tcp_free(port, host="127.0.0.1"):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(0.5)
    try:
        return s.connect_ex((host, port)) != 0
    finally:
        s.close()


def free_port(base, n=4, host="127.0.0.1"):
    for p in range(base, base + 400):
        if all(tcp_free(q, host) for q in range(p, p + n)):
            return p
    return None


def kill_tree(proc):
    if proc is None or proc.poll() is not None:
        return
    try:
        if os.name == "nt":
            subprocess.run(["taskkill", "/F", "/T", "/PID", str(proc.pid)],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=15)
        else:
            proc.terminate()
    except Exception:
        pass
    try:
        proc.wait(timeout=10)
    except Exception:
        try:
            proc.kill()
        except Exception:
            pass


class _MetaHandler(http.server.BaseHTTPRequestHandler):
    received = []  # shared across threads; list of decoded JSON payloads

    def do_POST(self):
        if self.path.rstrip("/") == "/api/metadata":
            length = int(self.headers.get("Content-Length", 0) or 0)
            body = self.rfile.read(length) if length else b""
            try:
                obj = json.loads(body)
            except Exception:
                obj = None
            _MetaHandler.received.append(obj)
            self.send_response(204)
            self.end_headers()
        else:
            self.send_response(404)
            self.end_headers()

    def log_message(self, *a):
        pass


def start_https_server(certfile, keyfile, port):
    srv = http.server.HTTPServer(("127.0.0.1", port), _MetaHandler)
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(certfile, keyfile)
    srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv


def gen_self_signed_cert(workdir):
    cert = os.path.join(workdir, "tls_cert.pem")
    key = os.path.join(workdir, "tls_key.pem")
    subprocess.run(
        ["openssl", "req", "-x509", "-newkey", "rsa:2048", "-keyout", key,
         "-out", cert, "-days", "1", "-nodes", "-subj", "/CN=localhost"],
        check=True, capture_output=True, text=True,
    )
    return cert, key


def agent_streaming(proc, log_path):
    if proc.poll() is not None:
        raise RuntimeError(f"camera-agent exited early (rc={proc.returncode})")
    try:
        with open(log_path, "r", encoding="utf-8", errors="replace") as f:
            return "STREAMING" in f.read()
    except FileNotFoundError:
        return False


def tail(path, n=15):
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            for ln in f.readlines()[-n:]:
                print("    | " + ln.rstrip())
    except Exception:
        pass


def main():
    ap = argparse.ArgumentParser()
    here = os.path.dirname(os.path.abspath(__file__))
    proj = os.path.dirname(here)
    parent = os.path.dirname(proj)
    ap.add_argument("--agent-root", default=os.path.join(parent, "carmera-agent"))
    ap.add_argument("--agent", default=None,
                    help="camera-agent exe (default <agent-root>/build-msvc/src/camera-agent.exe)")
    ap.add_argument("--camera", type=int, default=None, help="camera index")
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()

    agent = args.agent or os.path.join(args.agent_root, "build-msvc", "src", "camera-agent.exe")
    openssl = shutil.which("openssl") or shutil.which("openssl.exe")
    if not openssl:
        print("[FATAL] openssl not found on PATH (needed to mint the self-signed cert)")
        return 2
    if not os.path.exists(agent):
        print(f"[FATAL] camera-agent not found: {agent}")
        return 2

    data_dir = os.path.join(proj, "data")
    os.makedirs(data_dir, exist_ok=True)
    run_id = f"{os.getpid()}_{int(time.time()*1000)}"

    try:
        cert, key = gen_self_signed_cert(data_dir)
    except Exception as e:
        print(f"[FATAL] could not mint self-signed cert: {e}")
        return 2

    port = free_port(19080)
    if port is None:
        print("[FATAL] could not find a free TCP port")
        return 2
    srv = start_https_server(cert, key, port)
    base_url = f"https://127.0.0.1:{port}/api/metadata"
    print("== HTTPS metadata TLS acceptance (camera-agent --metadata-insecure) ==")
    print(f"   agent : {agent}")
    print(f"   https : {base_url}  (self-signed cert, CN=localhost)")
    print()

    # pick a camera index
    cam_index = args.camera
    if cam_index is None:
        try:
            out = subprocess.run([agent, "--list"], cwd=args.agent_root,
                                 capture_output=True, text=True, timeout=60)
            for ln in out.stdout.splitlines():
                if ln.strip().startswith("Camera") and ln.strip()[7:].strip().isdigit():
                    cam_index = int(ln.strip()[7:].strip())
                    break
        except Exception:
            cam_index = 0
    cam_index = cam_index or 0
    print(f"    using camera index {cam_index}")

    def run_agent(insecure):
        _MetaHandler.received.clear()
        log = os.path.join(data_dir, f"https_meta_{'insecure' if insecure else 'secure'}_{run_id}.log")
        cmd = [agent, "--camera", str(cam_index), "--stream", "camhttps",
               "--auto", "--ai",
               "--metadata", "--metadata-url", base_url,
               "--metadata-camera-id", "camhttps", "--metadata-heartbeat", "5",
               "--log-level", "info"]
        if insecure:
            cmd.append("--metadata-insecure")
        p = subprocess.Popen(cmd, cwd=args.agent_root,
                             stdout=open(log, "w", encoding="utf-8", errors="replace"),
                             stderr=subprocess.STDOUT)
        # Give the agent time to load the AI model and start POSTing.
        time.sleep(18)
        alive = p.poll() is None
        recv = list(_MetaHandler.received)
        kill_tree(p)
        return log, alive, recv

    # ---- Run A: insecure ON -> metadata must arrive ----------------------
    print("[*] Run A: agent --metadata --metadata-insecure -> https:// (self-signed)")
    log_a, alive_a, recv_a = run_agent(insecure=True)
    check("Run A agent stayed alive (model load + TLS relaxed)", alive_a,
          "camera-agent did not crash during the https run")
    valid = [r for r in recv_a if isinstance(r, dict)
             and ("frame_id" in r or "ai" in r or r.get("type") in ("frame", "status"))]
    check("Run A metadata reached the HTTPS endpoint", len(valid) > 0,
          f"{len(recv_a)} POST(s), {len(valid)} valid frame/status")
    if valid:
        info("Run A sample payload keys", ", ".join(sorted(valid[0].keys())))

    # ---- Run B: insecure OFF -> default TLS must BLOCK it -----------------
    print()
    print("[*] Run B: agent --metadata -> https:// (self-signed, NO --metadata-insecure)")
    log_b, alive_b, recv_b = run_agent(insecure=False)
    check("Run B agent stayed alive (TLS failure is non-fatal)", alive_b,
          "camera-agent did not crash on cert rejection")
    check("Run B default TLS validation BLOCKED self-signed cert", len(recv_b) == 0,
          f"{len(recv_b)} POST(s) reached the endpoint (expect 0)")
    if len(recv_b) == 0:
        info("Run B", "secure default preserved - self-signed cert rejected without the opt-in")

    srv.shutdown()

    if not args.keep:
        for f in (cert, key):
            try:
                os.remove(f)
            except Exception:
                pass

    print()
    print(f"== RESULT: PASS={PASS} FAIL={FAIL} INFO={INFO} ==")
    return 1 if FAIL > 0 else 0


if __name__ == "__main__":
    sys.exit(main())
