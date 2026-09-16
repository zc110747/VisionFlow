#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""One-shot WHEP reachability probe (sandbox-safe, self-cleaning).

Starts video-server (+ its MediaMTX child), pushes one synthetic RTSP stream,
then probes the WebRTC signaling endpoint two ways:
  1. via the server proxy  POST /api/cameras/<id>/webrtc
  2. directly at MediaMTX  POST /<id>/whep
A hand-crafted offer lacks ICE/DTLS bits, so MediaMTX answers 400 - which is
exactly the proof we want: the listener is alive and the proxy forwards.
Connection-refused anywhere means the WebRTC path is actually broken.
"""
import json
import shutil
import subprocess
import sys
import time
import urllib.error
import urllib.request

ROOT = r"D:\user_project\git\SOC-Agent\001-carmera_rtmp_net\video-server"
CFG = ROOT + r"\config\config.joint.yaml"
HTTP = "http://127.0.0.1:8081"
WHEP = "http://127.0.0.1:8889"
RTSP = "rtsp://127.0.0.1:8554/camera01"
FFMPEG = "ffmpeg"

OFFER = (
    "v=0\r\no=- 1 1 IN IP4 127.0.0.1\r\ns=-\r\nc=IN IP4 127.0.0.1\r\nt=0 0\r\n"
    "m=video 9 UDP/TLS/RTP/SAVPF 96\r\na=rtpmap:96 H264/90000\r\na=recvonly\r\n"
    "a=setup:actpass\r\na=mid:0\r\n"
)


def post(url, data, headers, timeout=10):
    req = urllib.request.Request(url, data=data, headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, r.read().decode("utf-8", "replace")
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode("utf-8", "replace")
    except Exception as e:
        return 0, f"{type(e).__name__}: {e}"


def get(url, timeout=5):
    try:
        with urllib.request.urlopen(url, timeout=timeout) as r:
            return r.status, r.read().decode("utf-8", "replace")
    except Exception as e:
        return 0, f"{type(e).__name__}: {e}"


def main():
    server = subprocess.Popen(
        [ROOT + r"\video-server.exe", CFG], cwd=ROOT,
        stdout=open(ROOT + r"\data\whep_probe_server.log", "w", encoding="utf-8",
                    errors="replace"),
        stderr=subprocess.STDOUT)
    ff = None
    try:
        # wait for health
        ok = False
        for _ in range(40):
            st, body = get(HTTP + "/api/health", 3)
            if st == 200:
                ok = True
                break
            time.sleep(0.5)
        print("server health:", "OK" if ok else "FAILED", body if not ok else body.strip())
        if not ok:
            return 1

        ffexe = shutil.which("ffmpeg") or FFMPEG
        print(f"   ffmpeg binary: {ffexe}")
        fflog = open(ROOT + r"\data\whep_probe_ff.log", "w", encoding="utf-8",
                     errors="replace")
        ff = subprocess.Popen(
            [ffexe, "-hide_banner", "-loglevel", "info", "-re",
             "-f", "lavfi", "-i", "testsrc=size=640x480:rate=25",
             "-pix_fmt", "yuv420p",
             "-c:v", "libx264", "-preset", "ultrafast", "-tune", "zerolatency",
             "-g", "25", "-b:v", "2000k",
             "-rtsp_transport", "tcp", "-f", "rtsp", RTSP],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL, stderr=fflog)
        # wait for registration
        reg = False
        for _ in range(30):
            st, body = get(HTTP + "/api/cameras", 3)
            if st == 200 and '"camera01"' in body:
                reg = True
                break
            time.sleep(1.0)
        print("camera01 auto-registered:", "OK" if reg else "FAILED",
              body[:120] if not reg else "")

        # Ground truth from MediaMTX itself: is the publisher STILL there at
        # the moment we probe WHEP? (ffmpeg may die between registration and
        # the probe, which turns any WHEP answer into "no stream available".)
        st, body = get("http://127.0.0.1:9997/v3/paths/list", 3)
        print(f"   mediamtx paths/list -> {st} body: {body[:300]}")
        try:
            items = json.loads(body).get("items", [])
        except Exception:
            items = []
        for it in items:
            print(f"   mediamtx path: {it.get('name')} online={it.get('online')} "
                  f"source={it.get('source')!r}")
        ff_alive = ff.poll() is None if ff else False
        print(f"   ffmpeg publisher alive: {ff_alive}")

        sdp_json = json.dumps({"sdp": OFFER}).encode()
        st, body = post(HTTP + "/api/cameras/camera01/webrtc", sdp_json,
                        {"Content-Type": "application/json"})
        print(f"[proxy  8081] POST /api/cameras/camera01/webrtc -> {st}")
        print(f"   body: {body[:200]}")
        st2, body2 = post(WHEP + "/camera01/whep", OFFER.encode(),
                          {"Content-Type": "application/sdp"})
        print(f"[direct 8889] POST /camera01/whep -> {st2}")
        print(f"   body: {body2[:200]}")

        verdict_proxy = st in (200, 201) or (st == 502 and "status 4" in body) or st == 400
        verdict_direct = st2 in (200, 201) or st2 == 400
        print()
        print("proxy forwards to a live WHEP listener:", "YES" if verdict_proxy else "NO")
        print("direct WHEP listener answers:", "YES" if verdict_direct else "NO")
        return 0 if (verdict_proxy and verdict_direct) else 1
    finally:
        for p in (ff, server):
            try:
                if p and p.poll() is None:
                    p.terminate()
                    p.wait(timeout=5)
            except Exception:
                try:
                    p.kill()
                except Exception:
                    pass


if __name__ == "__main__":
    sys.exit(main())
