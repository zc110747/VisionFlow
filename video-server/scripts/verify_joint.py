#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Joint end-to-end acceptance:  carmera-agent  ->  MediaMTX  ->  video-server.

This is the gate for "the two projects run together". Unlike verify_e2e.py,
which pushes SYNTHETIC streams with ffmpeg, this script drives the REAL
publisher - the C++ camera-agent with its GStreamer backend - and asserts the
whole chain:

    UVC camera
      -> camera-agent.exe   (GStreamer capture -> H264 -> rtspclientsink)
      -> MediaMTX           (spawned by video-server, RTSP :8554)
      -> video-server.exe   (monitor polls the control API, auto-registers)
      -> REST API / Web UI  (rtsp_url, resolution, WebRTC WHEP)

There is deliberately only ONE MediaMTX: the instance video-server spawns.
It is the one carrying the control API on :9997 that the monitor polls, so
the agent must push at that instance and must not start one of its own.

Stdlib only (urllib + subprocess). ffmpeg/ffprobe are resolved from PATH.

Usage:
  python scripts/verify_joint.py
  python scripts/verify_joint.py --stream camera01 --keep
  python scripts/verify_joint.py --agent D:/.../camera-agent.exe --camera 0
"""

import argparse
import json
import os
import re
import shutil
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


def http_get_json(url, timeout=5):
    with urllib.request.urlopen(url, timeout=timeout) as r:
        return json.loads(r.read().decode("utf-8")), r.status


def http_post_json(url, payload, timeout=10):
    data = json.dumps(payload).encode("utf-8")
    req = urllib.request.Request(url, data=data, headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.read().decode("utf-8"), r.status


def wait_for(predicate, timeout=40, interval=1.0):
    """Poll predicate until truthy. Returns the last value (may be an Exception)."""
    deadline = time.time() + timeout
    last = None
    while time.time() < deadline:
        try:
            last = predicate()
            if last:
                return last
        except Exception as e:  # noqa: BLE001 - reported to the caller
            last = e
        time.sleep(interval)
    return last


def kill_tree(proc):
    """Kill a process and its children.

    On Windows the server's MediaMTX child would survive a plain terminate(),
    leaving :8554 bound and breaking the next run, so kill the whole tree.
    """
    if proc is None or proc.poll() is not None:
        return
    try:
        if os.name == "nt":
            subprocess.run(
                ["taskkill", "/F", "/T", "/PID", str(proc.pid)],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=15,
            )
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


def yaml_ports(text):
    """Pull (http_port, rtsp.port) out of the server YAML.

    Ports are the single source of truth in config.yaml, so read them back
    instead of hardcoding - otherwise the agent could push at the wrong port.
    """
    http_port = None
    rtsp_port = None
    section = None
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        m_sec = re.match(r"^([A-Za-z_]\w*):\s*$", line)
        if m_sec:
            section = m_sec.group(1)
            continue
        m_kv = re.match(r"^([A-Za-z_]\w*):\s*(\d+)\s*(?:#.*)?$", line)
        if not m_kv:
            continue
        key, val = m_kv.group(1), int(m_kv.group(2))
        if key == "http_port":
            http_port = val
        elif key == "port" and section == "rtsp":
            rtsp_port = val
    return http_port, rtsp_port


def tail(path, n=25):
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            lines = f.readlines()[-n:]
        for ln in lines:
            print("    | " + ln.rstrip())
    except Exception:
        pass


def ffprobe_stream(ffprobe, url, timeout=25):
    """Return (ok, detail, width, height) for the first video stream."""
    try:
        out = subprocess.run(
            [ffprobe, "-v", "error", "-rtsp_transport", "tcp",
             "-show_entries", "stream=codec_type,codec_name,width,height",
             "-of", "json", url],
            capture_output=True, text=True, timeout=timeout,
        )
    except subprocess.TimeoutExpired:
        return False, "ffprobe timeout (stream never produced a decodable frame)", 0, 0
    except Exception as e:
        return False, str(e), 0, 0
    if out.returncode != 0:
        return False, f"ffprobe rc={out.returncode} {out.stderr.strip()[:160]}", 0, 0
    try:
        info_json = json.loads(out.stdout)
    except Exception:
        return False, "ffprobe output is not JSON", 0, 0
    vids = [s for s in info_json.get("streams", []) if s.get("codec_type") == "video"]
    if not vids:
        return False, "no video stream in ffprobe output", 0, 0
    v = vids[0]
    return True, f"codec={v.get('codec_name')}", int(v.get("width") or 0), int(v.get("height") or 0)


def main():
    ap = argparse.ArgumentParser()
    here = os.path.dirname(os.path.abspath(__file__))
    proj = os.path.dirname(here)
    parent = os.path.dirname(proj)
    ap.add_argument("--project-root", default=proj)
    ap.add_argument("--binary", default=os.path.join(proj, "video-server.exe"))
    ap.add_argument("--agent-root", default=os.path.join(parent, "carmera-agent"))
    ap.add_argument("--agent", default=None,
                    help="camera-agent executable (default: <agent-root>/build-msvc/src/camera-agent.exe)")
    ap.add_argument("--config", default=None,
                    help="server YAML (default: <project-root>/config/config.joint.yaml)")
    ap.add_argument("--stream", default="camera01")
    ap.add_argument("--camera", type=int, default=None,
                    help="camera index; auto-picked from --list when omitted")
    ap.add_argument("--host", default="localhost")
    ap.add_argument("--http-port", type=int, default=None, help="override config http_port")
    ap.add_argument("--rtsp-port", type=int, default=None, help="override config rtsp.port")
    ap.add_argument("--no-webrtc", action="store_true")
    ap.add_argument("--keep", action="store_true", help="leave processes running when done")
    args = ap.parse_args()

    agent = args.agent or os.path.join(args.agent_root, "build-msvc", "src", "camera-agent.exe")

    ffprobe = shutil.which("ffprobe") or shutil.which("ffprobe.exe")
    if not ffprobe:
        print("[FATAL] ffprobe not found on PATH")
        return 2

    for label, path in (("video-server", args.binary), ("camera-agent", agent)):
        if not os.path.exists(path):
            print(f"[FATAL] {label} not found: {path}")
            return 2

    # Default to the joint config: it moves HTTP off 8080, which the
    # "ApplicationWebServer" service on this machine grabs, so a verification
    # run does not randomly fail to bind the port.
    base_cfg = args.config or os.path.join(args.project_root, "config", "config.joint.yaml")
    try:
        with open(base_cfg, "r", encoding="utf-8") as f:
            cfg_text = f.read()
    except Exception as e:
        print(f"[FATAL] cannot read {base_cfg}: {e}")
        return 2

    cfg_http, cfg_rtsp = yaml_ports(cfg_text)
    http_port = args.http_port or cfg_http or 8080
    rtsp_port = args.rtsp_port or cfg_rtsp or 8554
    base = f"http://{args.host}:{http_port}"
    rtsp_url = f"rtsp://127.0.0.1:{rtsp_port}/{args.stream}"

    print("== joint acceptance: carmera-agent -> MediaMTX -> video-server ==")
    print(f"   video-server : {args.binary}")
    print(f"   camera-agent : {agent}")
    print(f"   config       : {base_cfg}")
    print(f"   http port    : {http_port}   rtsp port: {rtsp_port}")
    print(f"   stream       : {rtsp_url}")
    print(f"   ffprobe      : {ffprobe}")
    print()

    # ---- per-run config + DB, so runs never collide over a locked file ------
    run_id = f"{os.getpid()}_{int(time.time() * 1000)}"
    data_dir = os.path.join(args.project_root, "data")
    os.makedirs(data_dir, exist_ok=True)
    cfg_path = os.path.join(data_dir, f"joint_{run_id}.yaml")
    db_rel = f"data/video.joint_{run_id}.db"
    try:
        # Matches database.path whether it is video.db, video.joint.db, ...
        patched = re.sub(r"(path:\s*)data/video[\w.-]*\.db", r"\1" + db_rel, cfg_text)
        with open(cfg_path, "w", encoding="utf-8") as f:
            f.write(patched)
    except Exception as e:
        print(f"[FATAL] could not generate per-run config: {e}")
        return 2

    server_log = os.path.join(data_dir, f"verify_joint_server_{run_id}.log")
    agent_log = os.path.join(data_dir, f"verify_joint_agent_{run_id}.log")

    server = None
    publisher = None

    def cleanup():
        if args.keep:
            print()
            print("[*] --keep set: leaving video-server and camera-agent running")
            return
        kill_tree(publisher)
        kill_tree(server)

    try:
        # ================= stage 1: camera enumeration =====================
        print("[*] stage 1: enumerating cameras via camera-agent --list ...")
        cam_ids = []
        try:
            out = subprocess.run([agent, "--list"], cwd=args.agent_root,
                                 capture_output=True, text=True, timeout=60)
            for ln in out.stdout.splitlines():
                m = re.match(r"^Camera\s+(\d+)\s*$", ln.strip())
                if m:
                    cam_ids.append(int(m.group(1)))
        except Exception as e:
            print(f"    warning: enumeration failed: {e}")
        check("camera-agent enumerates at least one camera", bool(cam_ids),
              f"ids={cam_ids}" if cam_ids else "none found - check GStreamer and camera access")

        if args.camera is not None:
            cam_index = args.camera
        elif cam_ids:
            cam_index = cam_ids[0]
        else:
            cam_index = 0
        print(f"    using camera index {cam_index}")

        # ================= stage 2: start video-server =====================
        print(f"[*] stage 2: starting video-server (spawns MediaMTX) ...")
        server = subprocess.Popen(
            [args.binary, cfg_path],
            cwd=args.project_root,
            stdout=open(server_log, "w", encoding="utf-8", errors="replace"),
            stderr=subprocess.STDOUT,
        )

        hres = wait_for(lambda: http_get_json(f"{base}/api/health")[0], timeout=45, interval=1.0)
        if isinstance(hres, Exception):
            print("    server log tail:")
            tail(server_log)
            check("server /api/health reachable", False, str(hres))
            return _finish_with(cleanup)
        check("server /api/health reachable", True, json.dumps(hres))
        check("database healthy", hres.get("database") == "ok", f"database={hres.get('database')}")
        check("mediamtx control API healthy", hres.get("media_server") == "ok",
              f"media_server={hres.get('media_server')}")

        # ================= stage 3: start the real publisher ===============
        # --auto lets the camera pick its native format (e.g. 1280x720@30 on
        # the current UVC device) instead of a resolution that only fits one
        # specific camera - forcing caps the device cannot negotiate makes the
        # agent loop on reconnect forever. We also pass --ai --metadata so this
        # joint test exercises the Phase 3 AI metadata pipeline end-to-end.
        print(f"[*] stage 3: starting camera-agent --auto --ai --metadata -> {rtsp_url} ...")
        publisher = subprocess.Popen(
            [agent, "--camera", str(cam_index), "--stream", args.stream,
             "--server", "127.0.0.1", "--port", str(rtsp_port),
             "--auto", "--ai",
             "--metadata", "--metadata-url", f"{base}/api/metadata",
             "--metadata-camera-id", args.stream,
             "--log-level", "info"],
            cwd=args.agent_root,
            stdout=open(agent_log, "w", encoding="utf-8", errors="replace"),
            stderr=subprocess.STDOUT,
        )

        def agent_streaming():
            if publisher.poll() is not None:
                raise RuntimeError(f"camera-agent exited early (rc={publisher.returncode})")
            try:
                with open(agent_log, "r", encoding="utf-8", errors="replace") as f:
                    return "STREAMING" in f.read()
            except FileNotFoundError:
                return False

        res = wait_for(agent_streaming, timeout=45, interval=1.0)
        if isinstance(res, Exception) or not res:
            print("    agent log tail:")
            tail(agent_log)
            detail = str(res) if isinstance(res, Exception) else "no STREAMING within 45s"
            check("camera-agent reached STREAMING", False, detail)
            return _finish_with(cleanup)
        check("camera-agent reached STREAMING", True, rtsp_url)

        # report what the camera actually negotiated - the whole point of --auto
        neg_fps = None
        try:
            with open(agent_log, "r", encoding="utf-8", errors="replace") as f:
                for ln in f:
                    if "Negotiated capture format" in ln:
                        info("agent negotiated format", ln.strip().split("] ", 1)[-1])
                        m = re.search(r"@\s*(\d+)\s*fps", ln)
                        if m:
                            neg_fps = int(m.group(1))
                        break
        except Exception:
            pass

        # ================= stage 4: auto-registration ======================
        print(f"[*] stage 4: waiting for monitor to auto-register {args.stream} ...")
        cams = wait_for(lambda: _cameras_named(f"{base}/api/cameras", args.stream),
                        timeout=40, interval=2.0)
        if isinstance(cams, Exception) or not cams:
            print("    server log tail:")
            tail(server_log)
            detail = str(cams) if isinstance(cams, Exception) else "monitor never registered the path"
            check(f"stream {args.stream} auto-registered", False, detail)
            return _finish_with(cleanup)
        check(f"stream {args.stream} auto-registered", True,
              f"ids={[c.get('id') for c in cams]}")

        target = next((c for c in cams if c.get("id") == args.stream), cams[0])
        check(f"{args.stream} reports online", target.get("status") == "online",
              f"status={target.get('status')}")

        # ================= stage 5: real pixels ============================
        print("[*] stage 5: verifying the RTSP stream actually decodes ...")
        ok, detail, width, height = ffprobe_stream(ffprobe, rtsp_url)
        check(f"rtsp playback {args.stream}", ok, detail)

        # ================= stage 6: server-side metadata ===================
        print("[*] stage 6: verifying /api/cameras/{id}/stream metadata ...")
        try:
            meta, _ = http_get_json(f"{base}/api/cameras/{args.stream}/stream")
            rtsp_meta = meta.get("rtsp_url", "")
            check("stream metadata rtsp_url", args.stream in rtsp_meta, rtsp_meta)

            # The negotiated resolution must reach the API: MediaMTX reports it
            # on the media track and the monitor persists it. An empty value
            # means that propagation is broken even if the picture is fine.
            res_meta = wait_for(
                lambda: _resolution_or_none(f"{base}/api/cameras/{args.stream}/stream"),
                timeout=30, interval=2.0)
            if isinstance(res_meta, str) and res_meta:
                check("negotiated resolution propagated to API", True, res_meta)
                if width and height:
                    check("API resolution matches decoded stream",
                          res_meta == f"{width}x{height}",
                          f"api={res_meta} ffprobe={width}x{height}")
            else:
                detail = (str(res_meta) if isinstance(res_meta, Exception)
                          else "resolution empty - MediaMTX track props never reached the DB")
                check("negotiated resolution propagated to API", False, detail)

            # fps + bitrate are NOT reported by the agent; the server derives
            # fps via its own ffprobe probe and bitrate from the MediaMTX
            # bytesReceived delta. Both must reach the API for the Web UI to
            # show live stats. This is the regression guard for the 0/blank
            # stats seen before the monitor filled them in.
            meta_fps = wait_for(
                lambda: _int_field(f"{base}/api/cameras/{args.stream}/stream", "fps"),
                timeout=30, interval=2.0)
            if isinstance(meta_fps, int) and meta_fps > 0:
                check("frame rate propagated to API (ffprobe)", True, f"{meta_fps}fps")
                if neg_fps:
                    check("API fps matches agent-negotiated fps",
                          meta_fps == neg_fps,
                          f"api={meta_fps} agent={neg_fps}")
            else:
                detail = (str(meta_fps) if isinstance(meta_fps, Exception)
                          else "fps still 0 - server ffprobe probe did not run/persist")
                check("frame rate propagated to API (ffprobe)", False, detail)

            meta_br = wait_for(
                lambda: _int_field(f"{base}/api/cameras/{args.stream}/stream", "bitrate"),
                timeout=30, interval=2.0)
            if isinstance(meta_br, int) and meta_br > 0:
                check("bitrate propagated to API (bytesReceived delta)", True, f"{meta_br}kbps")
            else:
                detail = (str(meta_br) if isinstance(meta_br, Exception)
                          else "bitrate still 0 - bytesReceived delta not computed")
                check("bitrate propagated to API (bytesReceived delta)", False, detail)
        except Exception as e:
            check("stream metadata rtsp_url", False, str(e))

        # ================= stage 6b: AI metadata pipeline ==================
        # Phase 3: the agent POSTs AI frame/status JSON to /api/metadata; the
        # server persists it and serves it back. This proves the metadata chain
        # with the REAL publisher (not the synthetic mock server).
        print("[*] stage 6b: verifying AI metadata (frame/status) reaches the server ...")

        def meta_snapshot():
            try:
                return http_get_json(f"{base}/api/cameras/{args.stream}/metadata")[0]
            except Exception:
                return None

        snap = wait_for(meta_snapshot, timeout=40, interval=2.0)
        if not isinstance(snap, dict):
            check("AI metadata snapshot returned", False,
                  "no /api/cameras/{id}/metadata within 40s")
        else:
            check("AI metadata snapshot returned", True,
                  f"camera_id={snap.get('camera_id')}")
            # Heartbeat (status) and detection frames arrive on separate timers,
            # so poll each independently - arrival order must not matter.
            status = wait_for(lambda: (meta_snapshot() or {}).get("status"),
                              timeout=40, interval=2.0)
            if isinstance(status, dict):
                check("metadata status heartbeat present", True,
                      f"running={status.get('running')} fps={status.get('fps')} "
                      f"model={status.get('model')}")
            else:
                check("metadata status heartbeat present", False,
                      "no status within 40s - agent heartbeat not reaching server")
            frame = wait_for(lambda: (meta_snapshot() or {}).get("frame"),
                             timeout=30, interval=2.0)
            if isinstance(frame, dict):
                check("metadata frame present", True,
                      f"frame_id={frame.get('frame_id')} "
                      f"{frame.get('video_width')}x{frame.get('video_height')}")
                # Real detection round-trip: poll up to 20s for a frame that
                # actually carries detections (needs a target in front of the
                # camera). Empty frames are valid, so a miss is INFO not FAIL.
                # This closes the loop from synthetic bbox hardening
                # (verify_metadata) to REAL YOLO output produced by the live
                # camera-agent landing in the server snapshot.
                def frame_with_objs():
                    f = (meta_snapshot() or {}).get("frame")
                    if isinstance(f, dict) and isinstance(f.get("objects"), list) \
                            and len(f["objects"]) > 0:
                        return f
                    return None
                det_frame = wait_for(frame_with_objs, timeout=20, interval=2.0)
                if isinstance(det_frame, dict) and det_frame.get("objects"):
                    objs = det_frame["objects"]
                    w = det_frame.get("video_width") or 0
                    h = det_frame.get("video_height") or 0
                    count = det_frame.get("object_count")
                    if isinstance(count, int) and count > 0:
                        check("detected object_count matches payload",
                              count == len(objs), f"object_count={count} len={len(objs)}")
                    bad = []
                    classes = []
                    for o in objs:
                        if not isinstance(o, dict):
                            bad.append("not-an-object")
                            continue
                        c = o.get("class")
                        conf = o.get("confidence")
                        bx = o.get("bbox")
                        classes.append(str(c))
                        if not c:
                            bad.append("empty-class")
                        if not (isinstance(conf, (int, float)) and 0.0 <= conf <= 1.0):
                            bad.append("bad-confidence")
                        if not (isinstance(bx, (list, tuple)) and len(bx) == 4
                                and all(isinstance(v, int) for v in bx)):
                            bad.append("bad-bbox-shape")
                        elif w > 0 and h > 0:
                            x1, y1, x2, y2 = bx
                            if not (0 <= x1 < x2 <= w and 0 <= y1 < y2 <= h):
                                bad.append(f"bbox-out-of-frame {bx}")
                    if bad:
                        check("real detections well-formed (class/conf/bbox in-frame)",
                              False, "; ".join(bad[:5]))
                    else:
                        check("real detections well-formed (class/conf/bbox in-frame)",
                              True, f"{len(objs)} object(s): {', '.join(classes)}")
                    info("detected classes", ", ".join(classes) if classes else "(none this frame)")
                else:
                    info("no detections observed in window",
                         "no target in front of camera, or AI still warming up")
            else:
                info("metadata frame not yet present",
                     "status heartbeat arrived; frame needs the AI model + a few seconds")

        # ================= stage 7: WebRTC signaling =======================
        if not args.no_webrtc:
            print("[*] stage 7: WebRTC WHEP signaling reachability (informational) ...")
            offer = ("v=0\r\no=- 1 1 IN IP4 127.0.0.1\r\ns=-\r\n"
                     "c=IN IP4 127.0.0.1\r\nt=0 0\r\n"
                     "m=video 9 UDP/TLS/RTP/SAVPF 96\r\na=rtpmap:96 H264/90000\r\n"
                     "a=recvonly\r\na=setup:actpass\r\na=mid:0\r\n")
            try:
                body, st = http_post_json(f"{base}/api/cameras/{args.stream}/webrtc",
                                          {"sdp": offer}, timeout=10)
                if st in (200, 201) and "sdp" in body.lower():
                    check("webrtc signaling answered", True, f"status={st}")
                else:
                    info("webrtc signaling reachable",
                         f"status={st} - a hand-crafted offer cannot complete negotiation; "
                         "use a real browser for the full path")
            except Exception as e:
                info("webrtc signaling endpoint", f"error {e}")

    finally:
        cleanup()

    return _finish()


def _cameras_named(url, name):
    data, _ = http_get_json(url)
    if not isinstance(data, list):
        raise RuntimeError(f"unexpected /api/cameras body: {data!r}")
    return [c for c in data if c.get("id") == name and c.get("stream_path")]


def _resolution_or_none(url):
    data, _ = http_get_json(url)
    r = data.get("resolution")
    return r if r else None


def _int_field(url, key):
    """Return the int value of a metadata field, or None when it is 0/absent."""
    data, _ = http_get_json(url)
    v = data.get(key)
    if isinstance(v, int) and v > 0:
        return v
    return None


def _finish_with(cleanup):
    cleanup()
    return _finish()


def _finish():
    print()
    print(f"== RESULT: PASS={PASS} FAIL={FAIL} INFO={INFO} ==")
    return 1 if FAIL > 0 else 0


if __name__ == "__main__":
    sys.exit(main())
