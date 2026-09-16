#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Acceptance test for the POSE (keypoints) + AI-mode paths of the metadata API.

Covers the code that the git merge of 2026-09-04 had mangled (repo.go /
model.go / aimode): keypoints column storage + read-back, the pose->detect
skeleton-leak guard, frame-level ai_mode stamping, and the /aimode endpoints.

Methodology mirrors scripts/verify_metadata.py:
  - launch the server binary with a per-run config on a free port block
  - drive it purely over HTTP (stdlib urllib, no third-party deps)
  - print a PASS/FAIL tally and exit non-zero if any check fails

Usage:
  python scripts/verify_keypoints_aimode.py
  python scripts/verify_keypoints_aimode.py --base-url http://127.0.0.1:8081
"""

import argparse
import json
import os
import re
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.request

PASS = 0
FAIL = 0
CAM_ID = "posecam"


def check(name, ok, detail=""):
    global PASS, FAIL
    if ok:
        PASS += 1
        print(f"  [PASS] {name}" + (f" :: {detail}" if detail else ""))
    else:
        FAIL += 1
        print(f"  [FAIL] {name}" + (f" :: {detail}" if detail else ""))
    return ok


def http_get(url, timeout=5):
    with urllib.request.urlopen(url, timeout=timeout) as r:
        return r.read().decode("utf-8"), r.status


def http_get_json(url, timeout=5):
    body, status = http_get(url, timeout)
    return json.loads(body), status


def http_post_raw(url, body_bytes, timeout=15, content_type="application/json"):
    req = urllib.request.Request(url, data=body_bytes,
                                 headers={"Content-Type": content_type})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, r.read().decode("utf-8", "replace")
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode("utf-8", "replace")
    except Exception as e:
        return 0, f"{type(e).__name__}: {e}"


def post_json(url, payload, timeout=15):
    return http_post_raw(url, json.dumps(payload).encode("utf-8"), timeout=timeout)


def tcp_free(port, host="127.0.0.1"):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(0.5)
    try:
        return s.connect_ex((host, port)) != 0
    finally:
        s.close()


def free_block(base, n, host="127.0.0.1", maxtries=400):
    for b in range(base, base + maxtries):
        if all(tcp_free(p, host) for p in range(b, b + n)):
            return b
    return None


def patch_port(content, key_re, value, flags=0):
    return re.sub(key_re + r"(\d+)", lambda m: m.group(1) + str(value), content, flags=flags)


def ensure_port(content, key, value):
    if re.search(rf"(?m)^\s*{key}\s*:", content):
        return patch_port(content, rf"({key}:\s*)", value)
    return re.sub(r"(?m)^(mediamtx:\s*)$",
                  lambda m: f"{m.group(1)}\n  {key}: {value}", content, count=1)


# ---------------------------------------------------------------------------
# Message builders - wire format defined by carmera-agent (Metadata branch)
# ---------------------------------------------------------------------------

def pose_object(track_id=17, kps=None):
    if kps is None:
        kps = [[812, 210, 0.95], [900, 400, 0.88], [1040, 850, 0.71]]
    return {"class": "person", "confidence": 0.93, "track_id": track_id,
            "bbox": [812, 210, 1040, 850], "keypoints": kps}


def detect_object(track_id=3):
    return {"class": "person", "confidence": 0.74, "track_id": track_id,
            "bbox": [10, 20, 100, 200]}


def frame_msg(cam=CAM_ID, frame_id=1, objects=None, ai_mode=None):
    m = {"version": 1, "type": "frame", "camera_id": cam,
         "frame_id": frame_id, "timestamp": 1756773210123 + frame_id,
         "video_width": 1280, "video_height": 720, "objects": objects}
    if ai_mode is not None:
        m["ai_mode"] = ai_mode
    return m


def latest_frame(base):
    snap, _ = http_get_json(f"{base}/api/cameras/{CAM_ID}/metadata")
    return snap.get("frame") or {}


# ---------------------------------------------------------------------------
# Checks
# ---------------------------------------------------------------------------

def run_checks(base):
    # The per-camera read endpoints resolve {id} through the cameras table
    # (same contract verify_metadata.py relies on), so register a test camera
    # before any metadata is pushed.
    st, body = post_json(f"{base}/api/cameras",
                         {"id": CAM_ID, "name": "Pose test cam",
                          "stream_path": "camera01"})
    check("test camera registered", st in (200, 201, 204),
          f"status={st}" + ("" if st in (200, 201, 204) else f" body={body[:80]}"))

    print("\n== 1. keypoints round-trip (pose + detect objects in one frame) ==")
    kps_a = [[812, 210, 0.95], [900, 400, 0.88], [1040, 850, 0.71]]
    post_json(f"{base}/api/metadata", frame_msg(
        frame_id=4242, objects=[pose_object(track_id=17, kps=kps_a),
                                detect_object(track_id=3)]))
    fr = latest_frame(base)
    objs = fr.get("objects") or []
    check("both objects stored", len(objs) == 2, f"got={len(objs)}")
    if len(objs) == 2:
        pose = next((o for o in objs if o.get("track_id") == 17), None)
        det = next((o for o in objs if o.get("track_id") == 3), None)
        check("pose object keypoints round-trip verbatim",
              bool(pose) and pose.get("keypoints") == kps_a,
              f"got={None if pose is None else pose.get('keypoints')}")
        check("detection object has NO keypoints (wire-compat NULL)",
              bool(det) and not det.get("keypoints"),
              f"got={None if det is None else det.get('keypoints')}")

    print("\n== 2. pose -> detect switch must not leak skeletons ==")
    # The regression the repo is built around: ai_object is replaced (delete +
    # insert) per frame, so a reused frame_id or a mode switch can never
    # composite stale pose joints onto a detection-only frame.
    post_json(f"{base}/api/metadata", frame_msg(
        frame_id=5001, objects=[pose_object(kps=kps_a)]))
    post_json(f"{base}/api/metadata", frame_msg(
        frame_id=5002, objects=[detect_object(track_id=17)]))
    fr = latest_frame(base)
    objs = fr.get("objects") or []
    leaked = any(o.get("keypoints") for o in objs)
    check("latest detect frame carries no stale keypoints", not leaked,
          f"objects={[(o.get('track_id'), o.get('keypoints') is not None) for o in objs]}")

    print("\n== 3. pose -> pose overwrite uses the newest joints ==")
    kps_b = [[100, 200, 0.5], [300, 400, 0.6]]
    post_json(f"{base}/api/metadata", frame_msg(
        frame_id=5003, objects=[pose_object(track_id=17, kps=kps_a)]))
    post_json(f"{base}/api/metadata", frame_msg(
        frame_id=5004, objects=[pose_object(track_id=17, kps=kps_b)]))
    fr = latest_frame(base)
    objs = fr.get("objects") or []
    check("latest pose frame wins",
          bool(objs) and objs[0].get("keypoints") == kps_b,
          f"frame_id={fr.get('frame_id')} got={None if not objs else objs[0].get('keypoints')}")

    print("\n== 4. frame-level ai_mode stamping ==")
    post_json(f"{base}/api/metadata", frame_msg(
        frame_id=6001, objects=[pose_object()], ai_mode="ai-y-pose"))
    fr = latest_frame(base)
    check("ai_mode stored verbatim", fr.get("ai_mode") == "ai-y-pose",
          f"got={fr.get('ai_mode')}")
    post_json(f"{base}/api/metadata", frame_msg(
        frame_id=6002, objects=[detect_object()]))
    fr = latest_frame(base)
    check("frame without ai_mode stays empty (legacy agent)",
          not fr.get("ai_mode"), f"got={fr.get('ai_mode')}")

    print("\n== 5. /aimode endpoints ==")
    mode, st = http_get_json(f"{base}/api/cameras/{CAM_ID}/aimode")
    check("default aimode is ai-y", st == 200 and mode.get("mode") == "ai-y",
          f"got={mode.get('mode')}")

    st, _ = post_json(f"{base}/api/cameras/{CAM_ID}/aimode", {"mode": "ai-y-pose"})
    mode, _ = http_get_json(f"{base}/api/cameras/{CAM_ID}/aimode")
    check("POST ai-y-pose persisted", st == 200 and mode.get("mode") == "ai-y-pose",
          f"post={st} get={mode.get('mode')}")

    st, _ = post_json(f"{base}/api/cameras/{CAM_ID}/aimode", {"mode": "ai-off"})
    mode, _ = http_get_json(f"{base}/api/cameras/{CAM_ID}/aimode")
    check("POST ai-off persisted", st == 200 and mode.get("mode") == "ai-off",
          f"post={st} get={mode.get('mode')}")

    st, body = post_json(f"{base}/api/cameras/{CAM_ID}/aimode", {"mode": "ai-x"})
    mode, _ = http_get_json(f"{base}/api/cameras/{CAM_ID}/aimode")
    check("invalid mode rejected 400, value unchanged",
          st == 400 and mode.get("mode") == "ai-off",
          f"post={st} get={mode.get('mode')}")

    st, _ = post_json(f"{base}/api/cameras/{CAM_ID}/aimode", {})
    check("empty mode rejected 400", st == 400, f"post={st}")

    print("\n== 6. global snapshot lists the camera ==")
    overview, _ = http_get_json(f"{base}/api/metadata")
    cams = [c.get("camera_id") for c in (overview.get("cameras") or [])]
    check("GET /api/metadata lists the camera", CAM_ID in cams, f"cameras={cams}")


def main():
    ap = argparse.ArgumentParser()
    here = os.path.dirname(os.path.abspath(__file__))
    proj = os.path.dirname(here)
    ap.add_argument("--binary", default=os.path.join(proj, "video-server.exe"))
    ap.add_argument("--project-root", default=proj)
    ap.add_argument("--config", default=None)
    ap.add_argument("--base-url", default=None,
                    help="test an ALREADY RUNNING server instead of starting one")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--keep", action="store_true", help="do not stop the server at the end")
    args = ap.parse_args()

    if args.base_url:
        base = args.base_url.rstrip("/")
        print("== video-server keypoints/aimode acceptance (external server) ==")
        try:
            run_checks(base)
        except Exception as e:
            check("unhandled error", False, f"{type(e).__name__}: {e}")
        print(f"\n== RESULT: PASS={PASS} FAIL={FAIL} ==")
        sys.exit(1 if FAIL else 0)

    print("== video-server keypoints/aimode acceptance ==")
    print(f"   binary   : {args.binary}")

    base_cfg = args.config or os.path.join(args.project_root, "config", "config.yaml")
    if not os.path.exists(base_cfg):
        print(f"[FATAL] config not found: {base_cfg}")
        sys.exit(2)
    if not os.path.exists(args.binary):
        print(f"[FATAL] server binary not found: {args.binary}")
        sys.exit(2)

    run_id = f"{os.getpid()}_{int(time.time()*1000)}"
    cfg = os.path.join(args.project_root, "data", f"kp_{run_id}.yaml")
    block = free_block(18080, 8)
    if block is None:
        print("[FATAL] could not find 8 free TCP ports")
        sys.exit(2)
    http_port, rtsp_port = block, block + 1
    webrtc_port, hls_port = block + 2, block + 3
    api_port, ice_port = block + 4, block + 5
    rtp_port, rtcp_port = block + 6, block + 7

    with open(base_cfg, "r", encoding="utf-8") as f:
        content = f.read()
    db_path = f"data/video.kp_{run_id}.db"
    content = re.sub(r"(path:\s*)data/[^\s]*\.db", r"\1" + db_path, content)
    content = patch_port(content, r"(http_port:\s*)", http_port)
    content = patch_port(content, r"(rtsp:\s*\n\s*port:\s*)", rtsp_port, flags=re.DOTALL)
    content = patch_port(content, r"(webrtc:\s*\n\s*port:\s*)", webrtc_port, flags=re.DOTALL)
    content = patch_port(content, r"(hls_port:\s*)", hls_port)
    content = patch_port(content, r"(api_port:\s*)", api_port)
    content = patch_port(content, r"(ice_udp_port:\s*)", ice_port)
    content = patch_port(content, r"(ice_tcp_port:\s*)", ice_port)
    content = ensure_port(content, "rtp_port", rtp_port)
    content = ensure_port(content, "rtcp_port", rtcp_port)
    os.makedirs(os.path.join(args.project_root, "data"), exist_ok=True)
    with open(cfg, "w", encoding="utf-8") as f:
        f.write(content)

    base = f"http://{args.host}:{http_port}"
    print(f"   ports    : http={http_port} rtsp={rtsp_port} webrtc={webrtc_port} "
          f"hls={hls_port} api={api_port} ice={ice_port}")

    log_path = os.path.join(args.project_root, "data", f"verify_kp_{run_id}.log")
    print(f"[*] starting server (config={cfg}) ...")
    server = subprocess.Popen(
        [args.binary, cfg],
        cwd=args.project_root,
        stdout=open(log_path, "w", encoding="utf-8", errors="replace"),
        stderr=subprocess.STDOUT,
    )

    def cleanup():
        if args.keep:
            print(f"[*] --keep: leaving server pid={server.pid} running (log={log_path})")
            return
        server.terminate()
        try:
            server.wait(timeout=10)
        except Exception:
            server.kill()

    try:
        deadline = time.time() + 40
        healthy = False
        while time.time() < deadline:
            try:
                _, st = http_get_json(f"{base}/api/health", timeout=3)
                if st == 200:
                    healthy = True
                    break
            except Exception:
                time.sleep(0.5)
        if not healthy:
            print("[FATAL] server did not become healthy; log tail:")
            try:
                with open(log_path, "r", encoding="utf-8", errors="replace") as f:
                    print("".join(f.readlines()[-25:]))
            except Exception:
                pass
            sys.exit(3)
        print("  [PASS] server /api/health reachable")
        run_checks(base)
    finally:
        cleanup()

    print(f"\n== RESULT: PASS={PASS} FAIL={FAIL} ==")
    print(f"   server log: {log_path}")
    sys.exit(1 if FAIL else 0)


if __name__ == "__main__":
    main()
