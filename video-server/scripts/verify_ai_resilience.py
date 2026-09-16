#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
AI resilience acceptance -- Phase 2 spec §22, tests 2/3/4/5/6.

These cases exercise every branch of the project's two iron laws:
  1. an AI anomaly MUST NOT break the video stream;
  2. AI disabled MUST NOT break the agent (video + metadata still healthy).

Both are driven by the REAL publisher (the C++ camera-agent with its GStreamer
backend) against a REAL video-server, exactly like scripts/verify_joint.py:

  Test 5 (AI exception, video unaffected):
      launch camera-agent --ai --metadata --ai-model <a path that cannot load>
      -> assert the agent still reaches STREAMING, the H264/RTSP stream still
         decodes, AND the metadata heartbeat still arrives reporting
         ai.enable=true, ai.running=false. The agent log must contain the
         "video stream is unaffected" marker proving the AI failure was handled
         gracefully rather than tearing down the pipeline.

  Test 6 (AI off, agent normal):
      launch camera-agent --no-ai --metadata
      -> assert STREAMING + video decode + a healthy metadata heartbeat with
         ai.enable=false, ai.running=false. Proves the agent is fully usable
         with the AI branch disabled.

  Test 7 (server down / network down / recovery) closes the remaining gap in
  §22 (tests 2/3/4): launch camera-agent --ai --metadata, let metadata flow,
  then KILL the video-server (which also takes its MediaMTX child down). Assert
  the agent process survives the blackout and shows reconnect/backoff handling,
  then restart the server and assert the metadata heartbeat auto-resumes AND the
  stored frame_id advances past the pre-outage baseline.

Stdlib only (urllib + subprocess). ffmpeg/ffprobe resolved from PATH.
One video-server serves both scenarios to keep the run fast and the port block
unique per run.

Usage:
  python scripts/verify_ai_resilience.py
  python scripts/verify_ai_resilience.py --stream camera01 --keep
  python scripts/verify_ai_resilience.py --agent D:/.../camera-agent.exe
"""

import argparse
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.request

PASS = 0
FAIL = 0
INFO = 0

# A model path that provably cannot load on any machine -- relative to the
# agent's cwd, so the ONNX loader fails and init() returns false.
BAD_MODEL = "___nonexistent_ai_model_xyz.onnx"


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


def wait_for(predicate, timeout=40, interval=1.0):
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
    """Kill a process and its children (Windows: keep MediaMTX child from lingering)."""
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


def yaml_ports(text):
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


def patch_port(content, key_re, value, flags=0):
    return re.sub(key_re + r"(\d+)", lambda m: m.group(1) + str(value), content, flags=flags)


def ffprobe_stream(ffprobe, url, timeout=25):
    # MediaMTX may need a few seconds after the agent publishes before its path
    # answers RTSP DESCRIBE, so retry instead of failing on the first 404.
    last = ("", 0, 0)
    for attempt in range(4):
        try:
            out = subprocess.run(
                [ffprobe, "-v", "error", "-rtsp_transport", "tcp",
                 "-show_entries", "stream=codec_type,codec_name,width,height",
                 "-of", "json", url],
                capture_output=True, text=True, timeout=timeout,
            )
        except subprocess.TimeoutExpired:
            return False, "ffprobe timeout", 0, 0
        except Exception as e:
            return False, str(e), 0, 0
        if out.returncode == 0:
            try:
                info_json = json.loads(out.stdout)
            except Exception:
                return False, "ffprobe output is not JSON", 0, 0
            vids = [s for s in info_json.get("streams", []) if s.get("codec_type") == "video"]
            if not vids:
                return False, "no video stream", 0, 0
            v = vids[0]
            return True, f"codec={v.get('codec_name')}", int(v.get("width") or 0), int(v.get("height") or 0)
        last = (f"ffprobe rc={out.returncode} {out.stderr.strip()[:120]}", 0, 0)
        if attempt < 3:
            time.sleep(3)
    return False, last[0], 0, 0


def wait_camera_registered(base, stream, timeout=40, interval=2.0):
    """The video-server monitor auto-registers the camera by polling MediaMTX.
    Until that happens, /api/cameras/{id}/metadata returns 404, so the metadata
    assertions below would read None. Poll for the camera to appear first."""
    def present():
        data, _ = http_get_json(f"{base}/api/cameras")
        if not isinstance(data, list):
            raise RuntimeError(f"unexpected /api/cameras body: {data!r}")
        return any(c.get("id") == stream for c in data)
    return wait_for(present, timeout=timeout, interval=interval) is not None and True


def agent_streaming(proc, log_path):
    """Return truthy once the agent reaches STREAMING; raise if it died early."""
    if proc.poll() is not None:
        raise RuntimeError(f"camera-agent exited early (rc={proc.returncode})")
    try:
        with open(log_path, "r", encoding="utf-8", errors="replace") as f:
            return "STREAMING" in f.read()
    except FileNotFoundError:
        return False


def tail(path, n=25):
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            lines = f.readlines()[-n:]
        for ln in lines:
            print("    | " + ln.rstrip())
    except Exception:
        pass


def meta_snapshot(base, stream, timeout=40, interval=2.0):
    def snap():
        try:
            return http_get_json(f"{base}/api/cameras/{stream}/metadata")[0]
        except Exception:
            return None
    return wait_for(snap, timeout=timeout, interval=interval)


def wait_status(base, stream, timeout=30, interval=2.0):
    """Poll until the metadata heartbeat (status object) is actually present.

    The default heartbeat period is 10s, so the first status only lands ~10s
    after the sender starts. Waiting for any snapshot dict is not enough - it
    would return immediately with status=None and we'd mis-report a failure.
    Polling for the status object specifically keeps the agent alive long
    enough for the first heartbeat to arrive.
    """
    def pred():
        try:
            snap, _ = http_get_json(f"{base}/api/cameras/{stream}/metadata")
        except Exception:
            return None
        stt = snap.get("status")
        return stt if isinstance(stt, dict) else None
    return wait_for(pred, timeout=timeout, interval=interval)


def meta_frame_id(base, stream, timeout=30, interval=2.0):
    """Latest stored frame_id for a stream, or None if not yet present."""
    def pred():
        try:
            snap, _ = http_get_json(f"{base}/api/cameras/{stream}/metadata")
        except Exception:
            return None
        fr = snap.get("frame")
        if isinstance(fr, dict) and isinstance(fr.get("frame_id"), int):
            return fr.get("frame_id")
        return None
    return wait_for(pred, timeout=timeout, interval=interval)


def main():
    ap = argparse.ArgumentParser()
    here = os.path.dirname(os.path.abspath(__file__))
    proj = os.path.dirname(here)
    parent = os.path.dirname(proj)
    ap.add_argument("--project-root", default=proj)
    ap.add_argument("--binary", default=os.path.join(proj, "video-server.exe"))
    ap.add_argument("--agent-root", default=os.path.join(parent, "carmera-agent"))
    ap.add_argument("--agent", default=None,
                    help="camera-agent exe (default <agent-root>/build-msvc/src/camera-agent.exe)")
    ap.add_argument("--config", default=None,
                    help="server YAML (default <project-root>/config/config.joint.yaml)")
    ap.add_argument("--stream", default="camera01")
    ap.add_argument("--camera", type=int, default=None, help="camera index")
    ap.add_argument("--host", default="localhost")
    ap.add_argument("--keep", action="store_true")
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

    base_cfg = args.config or os.path.join(args.project_root, "config", "config.joint.yaml")
    try:
        with open(base_cfg, "r", encoding="utf-8") as f:
            cfg_text = f.read()
    except Exception as e:
        print(f"[FATAL] cannot read {base_cfg}: {e}")
        return 2

    cfg_http, cfg_rtsp = yaml_ports(cfg_text)
    # Pick a unique port block so a leftover MediaMTX cannot hijack :8554.
    block = free_block(19080, 8)
    if block is None:
        print("[FATAL] could not find 8 free TCP ports")
        return 2
    http_port, rtsp_port = block, block + 1
    content = re.sub(r"(path:\s*)data/[^\s]*\.db",
                     r"\1" + f"data/ai_res_{os.getpid()}.db", cfg_text)
    content = patch_port(content, r"(http_port:\s*)", http_port)
    content = patch_port(content, r"(rtsp:\s*\n\s*port:\s*)", rtsp_port, flags=re.DOTALL)
    run_id = f"{os.getpid()}_{int(time.time()*1000)}"
    cfg_path = os.path.join(args.project_root, "data", f"ai_res_{run_id}.yaml")
    os.makedirs(os.path.join(args.project_root, "data"), exist_ok=True)
    with open(cfg_path, "w", encoding="utf-8") as f:
        f.write(content)

    base = f"http://{args.host}:{http_port}"
    rtsp_url = f"rtsp://127.0.0.1:{rtsp_port}/{args.stream}"

    print("== AI resilience acceptance (spec 22, tests 2/3/4/5/6) ==")
    print(f"   video-server : {args.binary}")
    print(f"   camera-agent : {agent}")
    print(f"   http={http_port} rtsp={rtsp_port}  stream={rtsp_url}")
    print()

    # ---- pick a camera index ---------------------------------------------
    cam_index = args.camera
    if cam_index is None:
        try:
            out = subprocess.run([agent, "--list"], cwd=args.agent_root,
                                 capture_output=True, text=True, timeout=60)
            for ln in out.stdout.splitlines():
                m = re.match(r"^Camera\s+(\d+)\s*$", ln.strip())
                if m:
                    cam_index = int(m.group(1))
                    break
        except Exception:
            cam_index = 0
    cam_index = cam_index or 0
    print(f"    using camera index {cam_index}")

    server_log = os.path.join(args.project_root, "data", f"ai_res_server_{run_id}.log")
    server = subprocess.Popen(
        [args.binary, cfg_path], cwd=args.project_root,
        stdout=open(server_log, "w", encoding="utf-8", errors="replace"),
        stderr=subprocess.STDOUT,
    )

    def cleanup():
        if args.keep:
            print("\n[*] --keep: leaving video-server running")
            return
        kill_tree(agent_proc_holder[0])
        kill_tree(server)

    agent_proc_holder = [None]

    try:
        hres = wait_for(lambda: http_get_json(f"{base}/api/health")[0], timeout=45, interval=1.0)
        if isinstance(hres, Exception) or not isinstance(hres, dict):
            print("    server log tail:"); tail(server_log)
            check("server /api/health reachable", False, str(hres))
            return _finish_with(cleanup)
        check("server /api/health reachable", True, json.dumps(hres))

        # ===================================================================
        # Scenario 5: AI enabled but the model cannot load -> video survives.
        # ===================================================================
        print()
        print("[*] scenario 5: --ai --metadata --ai-model <missing>  (AI anomaly)")
        agent_log = os.path.join(args.project_root, "data", f"ai_res_bad_{run_id}.log")
        pub = subprocess.Popen(
            [agent, "--camera", str(cam_index), "--stream", args.stream,
             "--server", "127.0.0.1", "--port", str(rtsp_port),
             "--auto", "--ai", f"--ai-model", BAD_MODEL,
             "--metadata", "--metadata-url", f"{base}/api/metadata",
             "--metadata-camera-id", args.stream, "--metadata-heartbeat", "5",
             "--log-level", "info"],
            cwd=args.agent_root,
            stdout=open(agent_log, "w", encoding="utf-8", errors="replace"),
            stderr=subprocess.STDOUT,
        )
        agent_proc_holder[0] = pub

        def streaming():
            return agent_streaming(pub, agent_log)

        res = wait_for(streaming, timeout=45, interval=1.0)
        if isinstance(res, Exception) or not res:
            print("    agent log tail:"); tail(agent_log)
            detail = str(res) if isinstance(res, Exception) else "no STREAMING within 45s"
            check("scenario5 agent reached STREAMING", False, detail)
            return _finish_with(cleanup)
        check("scenario5 agent reached STREAMING", True,
              "(AI failed but pipeline still came up)")

        # Wait for the server monitor to auto-register the published path before
        # probing the stream or the metadata endpoint.
        reg5 = wait_camera_registered(base, args.stream)
        check("scenario5 camera auto-registered", bool(reg5),
              f"stream={args.stream}")

        # The exact log line is the contract that AI failure is non-fatal.
        try:
            with open(agent_log, "r", encoding="utf-8", errors="replace") as f:
                log_txt = f.read()
            check("scenario5 AI failure handled gracefully (video unaffected)",
                  "video stream is unaffected" in log_txt,
                  "agent logged the non-fatal AI init failure")
        except Exception as e:
            check("scenario5 AI failure handled gracefully (video unaffected)", False, str(e))

        # The video itself must decode despite the AI branch being dead.
        ok, detail, width, height = ffprobe_stream(ffprobe, rtsp_url)
        check("scenario5 video stream decodes (AI down)", ok,
              f"{detail} {width}x{height}")

        # Metadata heartbeat must still flow, reporting enable=true / running=false.
        stt = wait_status(base, args.stream)
        if not isinstance(stt, dict):
            check("scenario5 metadata heartbeat present", False,
                  "no status heartbeat within 30s (AI down but link alive)")
        else:
            got_enable = stt.get("enable")
            got_running = stt.get("running")
            check("scenario5 metadata heartbeat present", True,
                  f"enable={got_enable} running={got_running}")
            check("scenario5 metadata status.enable == true (AI was requested)",
                  got_enable is True, f"enable={got_enable}")
            check("scenario5 metadata status.running == false (model failed)",
                  got_running is False, f"running={got_running}")
            info("scenario5 AI status", f"enable={got_enable} running={got_running}")

        # Server-derived fps must be > 0 => encoder is producing frames.
        fps = wait_for(lambda: _int_field(f"{base}/api/cameras/{args.stream}/stream", "fps"),
                       timeout=30, interval=2.0)
        if isinstance(fps, int) and fps > 0:
            check("scenario5 live fps > 0 (encoder alive despite AI down)", True, f"{fps}fps")
        else:
            detail = (str(fps) if isinstance(fps, Exception)
                      else "fps still 0 - encoder stalled when AI died")
            check("scenario5 live fps > 0 (encoder alive despite AI down)", False, detail)

        # Tear the bad-model agent down before scenario 6.
        kill_tree(pub)
        agent_proc_holder[0] = None

        # ===================================================================
        # Scenario 6: AI disabled by config -> agent fully normal.
        # ===================================================================
        print()
        print("[*] scenario 6: --no-ai --metadata  (AI disabled)")
        # Distinct stream id from scenario 5 so the two runs do not share the
        # server-side ai_status row (otherwise scenario 6 would read scenario 5's
        # stale enable=true until its own heartbeat overwrites it).
        s6 = args.stream + "off"
        rtsp_url6 = f"rtsp://127.0.0.1:{rtsp_port}/{s6}"
        agent_log2 = os.path.join(args.project_root, "data", f"ai_res_off_{run_id}.log")
        pub2 = subprocess.Popen(
            [agent, "--camera", str(cam_index), "--stream", s6,
             "--server", "127.0.0.1", "--port", str(rtsp_port),
             "--auto", "--no-ai",
             "--metadata", "--metadata-url", f"{base}/api/metadata",
             "--metadata-camera-id", s6, "--metadata-heartbeat", "5",
             "--log-level", "info"],
            cwd=args.agent_root,
            stdout=open(agent_log2, "w", encoding="utf-8", errors="replace"),
            stderr=subprocess.STDOUT,
        )
        agent_proc_holder[0] = pub2

        res2 = wait_for(lambda: agent_streaming(pub2, agent_log2),
                        timeout=45, interval=1.0)
        if isinstance(res2, Exception) or not res2:
            print("    agent log tail:"); tail(agent_log2)
            detail = str(res2) if isinstance(res2, Exception) else "no STREAMING within 45s"
            check("scenario6 agent reached STREAMING", False, detail)
            return _finish_with(cleanup)
        check("scenario6 agent reached STREAMING", True, "(AI disabled, pipeline up)")

        reg6 = wait_camera_registered(base, s6)
        check("scenario6 camera auto-registered", bool(reg6),
              f"stream={s6}")

        ok2, detail2, w2, h2 = ffprobe_stream(ffprobe, rtsp_url6)
        check("scenario6 video stream decodes (AI off)", ok2, f"{detail2} {w2}x{h2}")

        snap2 = wait_status(base, s6)
        if not isinstance(snap2, dict):
            check("scenario6 metadata heartbeat present", False,
                  "no status heartbeat within 30s")
        else:
            check("scenario6 metadata heartbeat present", True,
                  f"enable={snap2.get('enable')} running={snap2.get('running')}")
            check("scenario6 metadata status.enable == false",
                  snap2.get("enable") is False, f"enable={snap2.get('enable')}")
            check("scenario6 metadata status.running == false",
                  snap2.get("running") is False, f"running={snap2.get('running')}")
            info("scenario6 AI status",
                 f"enable={snap2.get('enable')} running={snap2.get('running')}")

        fps2 = wait_for(lambda: _int_field(f"{base}/api/cameras/{s6}/stream", "fps"),
                        timeout=30, interval=2.0)
        if isinstance(fps2, int) and fps2 > 0:
            check("scenario6 live fps > 0 (normal operation)", True, f"{fps2}fps")
        else:
            detail = (str(fps2) if isinstance(fps2, Exception) else "fps still 0")
            check("scenario6 live fps > 0 (normal operation)", False, detail)

        kill_tree(pub2)
        agent_proc_holder[0] = None

        # ===================================================================
        # Scenario 7: server down then back (spec §22 tests 2, 3, 4).
        # The video-server also hosts MediaMTX, so killing it removes the RTSP
        # peer too -- the assertion is that the AGENT process survives the
        # outage and auto-resumes metadata when the server returns, NOT that
        # playback continues through the blackout. That is exactly what the
        # iron law requires ("server down -> AI/video keep running, metadata
        # auto-reconnects").
        # ===================================================================
        print()
        print("[*] scenario 7: server down -> agent survives -> server back (reconnect)")
        s7 = args.stream + "down"
        rtsp_url7 = f"rtsp://127.0.0.1:{rtsp_port}/{s7}"
        agent_log7 = os.path.join(args.project_root, "data", f"ai_res_down_{run_id}.log")
        pub7 = subprocess.Popen(
            [agent, "--camera", str(cam_index), "--stream", s7,
             "--server", "127.0.0.1", "--port", str(rtsp_port),
             "--auto", "--ai",
             "--metadata", "--metadata-url", f"{base}/api/metadata",
             "--metadata-camera-id", s7, "--metadata-heartbeat", "5",
             "--log-level", "info"],
            cwd=args.agent_root,
            stdout=open(agent_log7, "w", encoding="utf-8", errors="replace"),
            stderr=subprocess.STDOUT,
        )
        agent_proc_holder[0] = pub7

        res7 = wait_for(lambda: agent_streaming(pub7, agent_log7),
                        timeout=45, interval=1.0)
        if isinstance(res7, Exception) or not res7:
            print("    agent log tail:"); tail(agent_log7)
            detail = str(res7) if isinstance(res7, Exception) else "no STREAMING within 45s"
            check("scenario7 agent reached STREAMING", False, detail)
            return _finish_with(cleanup)
        check("scenario7 agent reached STREAMING", True, "(baseline before outage)")

        reg7 = wait_camera_registered(base, s7)
        check("scenario7 camera auto-registered", bool(reg7), f"stream={s7}")

        snap7 = wait_status(base, s7)
        if not isinstance(snap7, dict):
            check("scenario7 metadata heartbeat present (baseline)", False,
                  "no status heartbeat within 30s")
        else:
            check("scenario7 metadata heartbeat present (baseline)", True,
                  f"enable={snap7.get('enable')} running={snap7.get('running')}")

        # Baseline frame id while the server is still healthy.
        baseline = meta_frame_id(base, s7)
        if isinstance(baseline, int):
            info("scenario7 baseline frame_id", str(baseline))
        else:
            info("scenario7 baseline frame_id", "not available (will only assert resume)")

        # ----- simulate server / network down -----
        print("    [sim] killing video-server (takes MediaMTX + metadata API down)")
        kill_tree(server)
        time.sleep(6)  # let the agent notice the lost peer
        alive = pub7.poll() is None
        check("scenario7 agent survives server-down (pipeline alive)", alive,
              "camera-agent process still running after server kill")
        try:
            with open(agent_log7, "r", encoding="utf-8", errors="replace") as f:
                log7 = f.read()
            reacting = any(k in log7 for k in
                          ("reconnect", "DISCONNECTED", "send failed", "offline",
                           "REFUSED", "timed out", "connection lost"))
            check("scenario7 agent reacts to outage (reconnect/backoff)", reacting,
                  "log shows disconnect/reconnect handling")
        except Exception as e:
            check("scenario7 agent reacts to outage (reconnect/backoff)", False, str(e))

        # ----- bring the server back -----
        print("    [sim] restarting video-server")
        server = subprocess.Popen(
            [args.binary, cfg_path], cwd=args.project_root,
            stdout=open(server_log, "a", encoding="utf-8", errors="replace"),
            stderr=subprocess.STDOUT,
        )
        hres2 = wait_for(lambda: http_get_json(f"{base}/api/health")[0],
                         timeout=45, interval=1.0)
        if isinstance(hres2, Exception) or not isinstance(hres2, dict):
            check("scenario7 server recovered (/api/health)", False, str(hres2))
        else:
            check("scenario7 server recovered (/api/health)", True, json.dumps(hres2))

        stt7 = wait_status(base, s7, timeout=40, interval=2.0)
        if not isinstance(stt7, dict):
            check("scenario7 metadata auto-resumed after recovery", False,
                  "no status heartbeat after server restart")
        else:
            check("scenario7 metadata auto-resumed after recovery", True,
                  f"enable={stt7.get('enable')} running={stt7.get('running')}")

        # frame_id advancing past the pre-outage baseline proves the agent kept
        # producing frames through the blackout and flushed the latest one once
        # the link returned (test 4: auto-recovery of sending).
        recovered = meta_frame_id(base, s7, timeout=30, interval=2.0)
        if isinstance(recovered, int) and isinstance(baseline, int) and recovered > baseline:
            check("scenario7 metadata frame_id advanced past outage", True,
                  f"{baseline} -> {recovered}")
        elif isinstance(recovered, int) and not isinstance(baseline, int):
            check("scenario7 metadata frame_id readable after recovery", True,
                  f"recovered={recovered}")
        else:
            detail = f"baseline={baseline} recovered={recovered}"
            check("scenario7 metadata frame_id advanced past outage", False, detail)

        kill_tree(pub7)
        agent_proc_holder[0] = None

    finally:
        cleanup()

    return _finish()


def _int_field(url, key):
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
