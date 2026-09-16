#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Acceptance test for the camera-agent AI metadata ingest endpoint.

This is the SERVER side of the contract documented in
../carmera-agent/README.md ("Metadata 分支" -> "服务端对接说明"):

    POST /api/metadata           ingest frame + status messages (agent -> server)
    GET  /api/metadata           snapshot for every camera that ever pushed
    GET  /api/cameras/{id}/metadata   latest frame + heartbeat for one camera

Methodology mirrors scripts/verify_e2e.py:
  - launch the server binary with a per-run config on a free port block
  - drive it purely over HTTP (stdlib urllib, no third-party deps)
  - print a PASS/FAIL tally and exit non-zero if any core check fails

What is asserted, and why it matters:
  - frame_id / timestamp round-trip VERBATIM. They are produced on the capture
    side; renumbering them would silently break frame-accurate correlation.
  - bbox is clamped server-side too, so a buggy agent cannot paint a box
    outside the video and corrupt downstream overlays.
  - malformed input is rejected with 4xx and never stored.
  - sustained ingest at the agent's real rate (5 msg/s) stays consistent.

Usage:
  python scripts/verify_metadata.py
  python scripts/verify_metadata.py --base-url http://127.0.0.1:8081   # already running
  python scripts/verify_metadata.py --duration 20 --rate 5
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


def http_get(url, timeout=5):
    with urllib.request.urlopen(url, timeout=timeout) as r:
        return r.read().decode("utf-8"), r.status


def http_get_json(url, timeout=5):
    body, status = http_get(url, timeout)
    return json.loads(body), status


def http_post_raw(url, body_bytes, timeout=15, content_type="application/json"):
    """POST and return (status, body_text) for ANY outcome, including errors.

    urllib raises on 4xx/5xx, but the whole point of the negative checks is to
    read the status code, so HTTPError is caught and converted.
    """
    req = urllib.request.Request(url, data=body_bytes,
                                 headers={"Content-Type": content_type})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, r.read().decode("utf-8", "replace")
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode("utf-8", "replace")
    except Exception as e:  # connection reset / timeout: still a definite result
        return 0, f"{type(e).__name__}: {e}"


def post_json(url, payload, timeout=15):
    return http_post_raw(url, json.dumps(payload).encode("utf-8"), timeout=timeout)


def wait_for(predicate, timeout=45, interval=1.0):
    deadline = time.time() + timeout
    last = None
    while time.time() < deadline:
        try:
            last = predicate()
            if last:
                return last
        except Exception as e:
            last = e
        time.sleep(interval)
    return last


def tcp_free(port, host="127.0.0.1"):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(0.5)
    try:
        return s.connect_ex((host, port)) != 0
    finally:
        s.close()


def free_block(base, n, host="127.0.0.1", maxtries=400):
    """Find a base port B such that B..B+n-1 are all free.

    A contiguous block avoids colliding with a leftover MediaMTX still holding
    8554/8888/8889/9997/8189 from a previous run.
    """
    for b in range(base, base + maxtries):
        if all(tcp_free(p, host) for p in range(b, b + n)):
            return b
    return None


def patch_port(content, key_re, value, flags=0):
    return re.sub(key_re + r"(\d+)", lambda m: m.group(1) + str(value), content, flags=flags)


def ensure_port(content, key, value):
    """Set `key: <value>` inside the mediamtx block, adding it if absent.

    rtp_port/rtcp_port are optional keys, so a base config may not carry them -
    but MediaMTX then defaults to UDP 8000/8001, which on many boxes is already
    taken by a leftover instance. The child dies with
    "listen udp :8000: bind: Only one usage of each socket address" and every
    downstream check silently turns into "no cameras".
    """
    if re.search(rf"(?m)^\s*{key}\s*:", content):
        return patch_port(content, rf"({key}:\s*)", value)
    return re.sub(r"(?m)^(mediamtx:\s*)$",
                  lambda m: f"{m.group(1)}\n  {key}: {value}", content, count=1)


# ---------------------------------------------------------------------------
# Message builders - byte-compatible with the C++ agent's encoder
# ---------------------------------------------------------------------------

def frame_msg(cam="camera01", frame_id=15230, ts=1756773210123, w=1280, h=720,
              objects=None, version=1):
    if objects is None:
        objects = [{"class": "person", "confidence": 0.93, "track_id": 17,
                    "bbox": [812, 210, 1040, 850]}]
    return {"version": version, "type": "frame", "camera_id": cam,
            "frame_id": frame_id, "timestamp": ts,
            "video_width": w, "video_height": h, "objects": objects}


def status_msg(cam="camera01", wall=1756773215000, running=True, processed=99):
    return {"version": 1, "type": "status", "camera_id": cam, "wall_clock": wall,
            "ai": {"enable": True, "running": running, "fps": 5.0,
                   "model": "models/yolov8n.onnx", "tracker": "bytetrack",
                   "last_frame_id": 15230, "last_timestamp": 1756773210123,
                   "processed": processed}}


# ---------------------------------------------------------------------------
# Checks
# ---------------------------------------------------------------------------

def run_checks(base, duration, rate):
    print("\n== 1. ingest basics ==")
    st, body = post_json(f"{base}/api/metadata", frame_msg())
    check("POST frame accepted", st in (200, 201, 202, 204),
          f"status={st}" + ("" if st == 204 else f" body={body[:80]}"))

    st, body = post_json(f"{base}/api/metadata", status_msg())
    check("POST status accepted", st in (200, 201, 202, 204), f"status={st}")

    print("\n== 2. round-trip fidelity ==")
    fid, ts = 424242, 1756773999000
    objs = [
        {"class": "person", "confidence": 0.91, "track_id": 7, "bbox": [10, 20, 100, 200]},
        {"class": "person", "confidence": 0.55, "track_id": 9, "bbox": [300, 300, 500, 600]},
    ]
    post_json(f"{base}/api/metadata",
              frame_msg(frame_id=fid, ts=ts, w=1280, h=720, objects=objs))
    snap, _ = http_get_json(f"{base}/api/cameras/{CAM_ID}/metadata")
    fr = snap.get("frame") or {}
    check("frame_id stored verbatim", fr.get("frame_id") == fid,
          f"got={fr.get('frame_id')} want={fid}")
    check("timestamp stored verbatim", fr.get("timestamp") == ts,
          f"got={fr.get('timestamp')} want={ts}")
    check("video size stored", (fr.get("video_width"), fr.get("video_height")) == (1280, 720),
          f"got={fr.get('video_width')}x{fr.get('video_height')}")
    got_objs = fr.get("objects") or []
    check("object count preserved", len(got_objs) == 2, f"got={len(got_objs)}")
    if len(got_objs) == 2:
        check("track_id preserved", [o.get("track_id") for o in got_objs] == [7, 9],
              f"got={[o.get('track_id') for o in got_objs]}")
        check("bbox preserved", got_objs[0].get("bbox") == [10, 20, 100, 200],
              f"got={got_objs[0].get('bbox')}")

    print("\n== 3. heartbeat ==")
    post_json(f"{base}/api/metadata", status_msg(processed=1234))
    snap, _ = http_get_json(f"{base}/api/cameras/{CAM_ID}/metadata")
    stt = snap.get("status") or {}
    check("status stored", stt.get("processed") == 1234, f"processed={stt.get('processed')}")
    check("model stored", stt.get("model") == "models/yolov8n.onnx", f"model={stt.get('model')}")
    check("tracker stored", stt.get("tracker") == "bytetrack", f"tracker={stt.get('tracker')}")
    check("ai running flag stored", stt.get("running") is True, f"running={stt.get('running')}")

    print("\n== 4. server-side bbox hardening ==")
    # Out-of-frame box: the agent already clamps, the server must not trust it.
    post_json(f"{base}/api/metadata", frame_msg(
        frame_id=5001, w=1280, h=720,
        objects=[{"class": "person", "confidence": 0.8, "track_id": 1,
                  "bbox": [-500, -500, 99999, 99999]}]))
    snap, _ = http_get_json(f"{base}/api/cameras/{CAM_ID}/metadata")
    box = ((snap.get("frame") or {}).get("objects") or [{}])[0].get("bbox")
    ok = bool(box) and box[0] >= 0 and box[1] >= 0 and box[2] <= 1280 and box[3] <= 720
    check("out-of-range bbox clamped into frame", ok, f"bbox={box}")

    # Degenerate box: zero area cannot be drawn nor IoU-matched, must be widened.
    post_json(f"{base}/api/metadata", frame_msg(
        frame_id=5002, w=1280, h=720,
        objects=[{"class": "person", "confidence": 0.8, "track_id": 2,
                  "bbox": [400, 400, 400, 400]}]))
    snap, _ = http_get_json(f"{base}/api/cameras/{CAM_ID}/metadata")
    box = ((snap.get("frame") or {}).get("objects") or [{}])[0].get("bbox")
    check("degenerate bbox widened", bool(box) and box[2] > box[0] and box[3] > box[1],
          f"bbox={box}")

    # Reversed corners (x2<x1): swap instead of dropping a real detection.
    post_json(f"{base}/api/metadata", frame_msg(
        frame_id=5003, w=1280, h=720,
        objects=[{"class": "person", "confidence": 0.8, "track_id": 3,
                  "bbox": [600, 600, 300, 300]}]))
    snap, _ = http_get_json(f"{base}/api/cameras/{CAM_ID}/metadata")
    box = ((snap.get("frame") or {}).get("objects") or [{}])[0].get("bbox")
    check("reversed bbox normalised", bool(box) and box[2] > box[0] and box[3] > box[1],
          f"bbox={box}")

    print("\n== 5. empty result is still a result ==")
    post_json(f"{base}/api/metadata", frame_msg(frame_id=6001, objects=[]))
    snap, _ = http_get_json(f"{base}/api/cameras/{CAM_ID}/metadata")
    fr = snap.get("frame") or {}
    check("empty objects array keeps the frame",
          fr.get("frame_id") == 6001 and fr.get("object_count") == 0,
          f"frame_id={fr.get('frame_id')} object_count={fr.get('object_count')}")

    print("\n== 6. malformed input is rejected, never stored ==")
    good_before = (http_get_json(f"{base}/api/cameras/{CAM_ID}/metadata")[0]
                   .get("frame") or {}).get("frame_id")

    st, _ = http_post_raw(f"{base}/api/metadata", b"{not json")
    check("invalid json -> 400", st == 400, f"status={st}")

    # No "type" AND no discriminating shape at all - the server must refuse to
    # guess rather than file it under a random kind.
    st, _ = post_json(f"{base}/api/metadata", {"version": 1, "camera_id": "x"})
    check("unrecognisable shape -> 400", st == 400, f"status={st}")

    # No "type" but a shape that can only be a heartbeat.
    st, _ = post_json(f"{base}/api/metadata",
                      {"type": "wat", "camera_id": "x"})
    check("unknown type -> 400", st == 400, f"status={st}")

    bad = frame_msg(frame_id=9999)
    bad["camera_id"] = ""
    st, _ = post_json(f"{base}/api/metadata", bad)
    check("frame without camera_id -> 400", st == 400, f"status={st}")

    # 2 MiB of padding: must be rejected BEFORE decoding, not buffered.
    big = frame_msg(frame_id=9999)
    big["pad"] = "A" * (2 * 1024 * 1024)
    st, _ = post_json(f"{base}/api/metadata", big, timeout=30)
    check("oversized payload -> 413", st == 413, f"status={st}")

    snap, _ = http_get_json(f"{base}/api/cameras/{CAM_ID}/metadata")
    check("rejected messages did not overwrite good data",
          (snap.get("frame") or {}).get("frame_id") == good_before,
          f"frame_id={(snap.get('frame') or {}).get('frame_id')} want={good_before}")

    print('\n== 7. legacy agent frames without the "type" field ==')
    # Regression guard. Agent builds before the frame discriminator existed
    # sent frames with NO "type" at all - and a frame with zero objects has an
    # empty "objects" array, so nothing but frame_id identifies it. Those
    # messages must still be accepted, or a deployed agent silently loses 100%
    # of its frames (seen in the field as sent=0 failed=5 dropped=90).
    legacy = {
        "version": 1,
        "camera_id": CAM_ID,
        "frame_id": 6100,
        "timestamp": 1_700_000_000_000 + 6100,
        "video_width": 1280,
        "video_height": 720,
        "objects": [],
    }
    st, body = post_json(f"{base}/api/metadata", legacy)
    check("typeless frame is accepted", st in (200, 201, 202, 204),
          f"status={st} body={body[:80]}")
    snap, _ = http_get_json(f"{base}/api/cameras/{CAM_ID}/metadata")
    got = (snap.get("frame") or {}).get("frame_id")
    check("typeless frame is stored", got == 6100, f"got={got} want=6100")

    # A heartbeat without "type" is recognised by its "ai" object.
    legacy_hb = {
        "version": 1,
        "camera_id": CAM_ID,
        "wall_clock": 1_700_000_000_000,
        "ai": {"enable": True, "running": True, "fps": 5.0,
               "model": "models/legacy.onnx", "tracker": "bytetrack",
               "last_frame_id": 6100, "last_timestamp": 1_700_000_006_100,
               "processed": 4242},
    }
    st, body = post_json(f"{base}/api/metadata", legacy_hb)
    check("typeless heartbeat is accepted", st in (200, 201, 202, 204),
          f"status={st} body={body[:80]}")
    snap, _ = http_get_json(f"{base}/api/cameras/{CAM_ID}/metadata")
    check("typeless heartbeat is stored",
          (snap.get("status") or {}).get("model") == "models/legacy.onnx",
          f"model={(snap.get('status') or {}).get('model')}")

    # An explicit "type" always wins over inference - a heartbeat carrying
    # frame-ish fields must not be mis-filed as a frame.
    confusing = dict(legacy_hb)
    confusing["type"] = "status"
    st, _ = post_json(f"{base}/api/metadata", confusing)
    check("declared type overrides shape inference",
          st in (200, 201, 202, 204), f"status={st}")

    print("\n== 8. camera identity mapping ==")
    # camera_id in the payload is the RTSP stream path; the DB row can carry a
    # different id. Both directions must resolve to the same camera.
    snap, _ = http_get_json(f"{base}/api/cameras/{CAM_ID}/metadata")
    check("metadata readable by camera id", snap.get("camera_id") == CAM_ID,
          f"camera_id={snap.get('camera_id')}")
    snap2, _ = http_get_json(f"{base}/api/cameras/{STREAM_PATH}/metadata")
    check("metadata readable by stream path",
          (snap2.get("frame") or {}).get("frame_id") == (snap.get("frame") or {}).get("frame_id"),
          f"by-path frame_id={(snap2.get('frame') or {}).get('frame_id')}")

    print("\n== 9. global overview ==")
    data, _ = http_get_json(f"{base}/api/metadata")
    cams = data.get("cameras") or []
    check("GET /api/metadata reports enabled", data.get("enabled") is True,
          f"enabled={data.get('enabled')}")
    check("GET /api/metadata lists the camera",
          any(c.get("camera_id") == CAM_ID for c in cams),
          f"cameras={[c.get('camera_id') for c in cams]}")

    print(f"\n== 10. sustained ingest at {rate} msg/s for {duration}s ==")
    interval = 1.0 / rate
    sent = 0
    failures = []
    t_end = time.time() + duration
    fid_seq = 7000
    while time.time() < t_end:
        fid_seq += 1
        fid_local = fid_seq
        st, body = post_json(f"{base}/api/metadata",
                             frame_msg(frame_id=fid_local, ts=1_700_000_000_000 + fid_local))
        sent += 1
        if st not in (200, 201, 202, 204):
            failures.append((fid_local, st, body[:60]))
        # Heartbeats interleave with frames exactly like the real agent does.
        if fid_local % 10 == 0:
            post_json(f"{base}/api/metadata", status_msg(processed=fid_local))
        time.sleep(interval)
    check(f"all {sent} sustained messages accepted", not failures,
          f"sent={sent} failures={failures[:3]}")

    snap, _ = http_get_json(f"{base}/api/cameras/{CAM_ID}/metadata")
    last = (snap.get("frame") or {}).get("frame_id")
    check("latest frame is the last one sent", last == fid_seq,
          f"got={last} want={fid_seq}")

    print("\n== 11. media path untouched ==")
    health, _ = http_get_json(f"{base}/api/health")
    check("server still healthy after ingest", health.get("status") == "ok",
          f"health={health}")


CAM_ID = ""
STREAM_PATH = ""


def main():
    global CAM_ID, STREAM_PATH

    ap = argparse.ArgumentParser()
    here = os.path.dirname(os.path.abspath(__file__))
    proj = os.path.dirname(here)
    ap.add_argument("--binary", default=os.path.join(proj, "video-server.exe"))
    ap.add_argument("--project-root", default=proj)
    ap.add_argument("--config", default=None)
    ap.add_argument("--base-url", default=None,
                    help="test an ALREADY RUNNING server instead of starting one "
                         "(e.g. http://127.0.0.1:8081 during a joint run)")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--duration", type=int, default=10, help="sustained-ingest seconds")
    ap.add_argument("--rate", type=int, default=5, help="messages per second")
    ap.add_argument("--keep", action="store_true", help="do not stop the server at the end")
    args = ap.parse_args()

    if args.base_url:
        base = args.base_url.rstrip("/")
        CAM_ID = "camera01"
        STREAM_PATH = "camera01"
        print("== video-server metadata acceptance (external server) ==")
        print(f"   base url : {base}")
        try:
            run_checks(base, args.duration, args.rate)
        except Exception as e:
            check("unhandled error", False, f"{type(e).__name__}: {e}")
        print(f"\n== RESULT: PASS={PASS} FAIL={FAIL} ==")
        sys.exit(1 if FAIL else 0)

    print("== video-server metadata acceptance ==")
    print(f"   binary   : {args.binary}")
    print(f"   project  : {args.project_root}")

    base_cfg = args.config or os.path.join(args.project_root, "config", "config.yaml")
    if not os.path.exists(base_cfg):
        print(f"[FATAL] config not found: {base_cfg}")
        sys.exit(2)
    if not os.path.exists(args.binary):
        print(f"[FATAL] server binary not found: {args.binary}")
        sys.exit(2)

    run_id = f"{os.getpid()}_{int(time.time()*1000)}"
    cfg = os.path.join(args.project_root, "data", f"meta_{run_id}.yaml")
    # 8 ports: the two extra ones are MediaMTX's RTP/RTCP UDP ports. They are
    # easy to forget because they are UDP - a leftover MediaMTX holding 8000
    # makes the child die with "bind: Only one usage of each socket address",
    # which then shows up as media_server:error and a monitor that sees nothing.
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
    db_path = f"data/video.meta_{run_id}.db"
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
          f"hls={hls_port} api={api_port} ice={ice_port} "
          f"rtp={rtp_port} rtcp={rtcp_port}")

    log_path = os.path.join(args.project_root, "data", f"verify_metadata_{run_id}.log")
    print(f"[*] starting server (config={cfg}) ...")
    server = subprocess.Popen(
        [args.binary, cfg],
        cwd=args.project_root,
        stdout=open(log_path, "w", encoding="utf-8", errors="replace"),
        stderr=subprocess.STDOUT,
    )

    try:
        h = wait_for(lambda: http_get_json(f"{base}/api/health")[0], timeout=45)
        if isinstance(h, Exception) or not h:
            print(f"[FATAL] server did not become healthy: {h}")
            print("    server log tail:")
            try:
                with open(log_path, "r", encoding="utf-8", errors="replace") as f:
                    print("\n".join("      " + l.rstrip() for l in f.readlines()[-25:]))
            except Exception:
                pass
            sys.exit(2)
        check("server /api/health reachable", True, str(h))

        # A camera with a DB id that differs from its stream path, so the
        # camera_id -> camera mapping is actually exercised in both directions.
        CAM_ID = "metacam"
        STREAM_PATH = "camera01"
        post_json(f"{base}/api/cameras",
                  {"id": CAM_ID, "name": "Metadata test cam", "stream_path": STREAM_PATH})
        print(f"[*] test camera: id={CAM_ID} stream_path={STREAM_PATH}")

        run_checks(base, args.duration, args.rate)
    except Exception as e:
        check("unhandled error", False, f"{type(e).__name__}: {e}")
    finally:
        if args.keep:
            info("server left running", base)
        else:
            try:
                server.terminate()
                server.wait(timeout=10)
            except Exception:
                try:
                    server.kill()
                except Exception:
                    pass

    print(f"\n== RESULT: PASS={PASS} FAIL={FAIL} INFO={INFO} ==")
    print(f"   server log: {log_path}")
    sys.exit(1 if FAIL else 0)


if __name__ == "__main__":
    main()
