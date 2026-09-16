#!/usr/bin/env python3
"""Minimal metadata receiver used to verify camera-agent's metadata branch.

Standard library only - no pip install needed.

What it does
  * Accepts POST on any path (default endpoint: /api/metadata).
  * Prints a one-line summary per message (type, camera_id, frame_id, objects).
  * Replies 200 {"ok": true} so the agent counts the send as successful.

Fault injection (for spec-22 acceptance tests)
  --fail-after N   reply HTTP 500 once N messages have been received
  --die-after  N   exit the process after N messages (simulates a server crash)

Usage
  python scripts/metadata-mock-server.py --port 8000
  python scripts/metadata-mock-server.py --port 8000 --dump        # full JSON
  python scripts/metadata-mock-server.py --port 8000 --fail-after 20
"""

import argparse
import json
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

STATE = {
    "start": time.time(),
    "frames": 0,
    "status": 0,
    "objects": 0,
    "total": 0,
}


class Server(ThreadingHTTPServer):
    daemon_threads = True

    # The agent tears down its keep-alive socket on shutdown. That aborts the
    # read of the NEXT request line inside http.server, i.e. before do_POST is
    # even reached, so the try/except in do_POST cannot catch it. Without this
    # override socketserver prints a full ConnectionResetError traceback.
    def handle_error(self, request, client_address):
        exc = sys.exc_info()[1]
        if isinstance(exc, (ConnectionResetError, BrokenPipeError, OSError)):
            return
        ThreadingHTTPServer.handle_error(self, request, client_address)


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    # Keep the test output readable.
    def log_message(self, fmt, *args):
        pass

    def _reply(self, code, payload):
        body = json.dumps(payload).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        self._reply(200, {
            "ok": True,
            "service": "metadata-mock",
            "received": STATE["total"],
            "uptime_sec": round(time.time() - STATE["start"], 1),
        })

    def do_POST(self):
        # The agent closes the keep-alive connection abruptly on shutdown; an
        # OSError here is expected and must not produce a stack trace.
        try:
            self._handle_post()
        except (ConnectionResetError, BrokenPipeError, OSError):
            self.close_connection = True

    def _handle_post(self):
        length = int(self.headers.get("Content-Length") or 0)
        raw = self.rfile.read(length) if length else b""

        STATE["total"] += 1
        seq = STATE["total"]

        if seq > ARGS.fail_after > 0:
            self._reply(500, {"ok": False, "error": "simulated server error"})
            print("[mock] #%d -> HTTP 500 (simulated failure)" % seq, flush=True)
        else:
            self._reply(200, {"ok": True})

        try:
            msg = json.loads(raw.decode("utf-8"))
        except Exception as exc:
            print("[mock] #%d unparsable body (%d bytes): %s" % (seq, len(raw), exc),
                  flush=True)
            self._maybe_die()
            return

        if ARGS.dump:
            print("[mock] #%d %s" % (seq, json.dumps(msg, ensure_ascii=False)), flush=True)
        else:
            mtype = msg.get("type", "frame")
            if mtype == "status":
                STATE["status"] += 1
                ai = msg.get("ai", {})
                print("[mock] #%d status  camera=%s ai_enable=%s running=%s fps=%s "
                      "last_frame=%s processed=%s"
                      % (seq, msg.get("camera_id"), ai.get("enable"), ai.get("running"),
                         ai.get("fps"), ai.get("last_frame_id"), ai.get("processed")),
                      flush=True)
            else:
                objs = msg.get("objects", [])
                STATE["frames"] += 1
                STATE["objects"] += len(objs)
                ids = ",".join("%s#%s(%.2f)" % (o.get("class"), o.get("track_id"),
                                                o.get("confidence") or 0.0) for o in objs)
                print("[mock] #%d frame   camera=%s frame_id=%s ts=%s %sx%s objects=%d %s"
                      % (seq, msg.get("camera_id"), msg.get("frame_id"),
                         msg.get("timestamp"), msg.get("video_width"),
                         msg.get("video_height"), len(objs), ids),
                      flush=True)

        self._maybe_die()

    def _maybe_die(self):
        if 0 < ARGS.die_after <= STATE["total"]:
            print("[mock] reached --die-after %d, exiting to simulate a server crash"
                  % ARGS.die_after, flush=True)
            # Shutdown from a handler thread, then let the main thread exit.
            import threading
            threading.Thread(target=SERVER.shutdown, daemon=True).start()


def main():
    global ARGS, SERVER

    parser = argparse.ArgumentParser(description="camera-agent metadata mock server")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8000)
    parser.add_argument("--dump", action="store_true", help="print the full JSON body")
    parser.add_argument("--fail-after", type=int, default=0,
                        help="reply HTTP 500 after N messages (0 = never)")
    parser.add_argument("--die-after", type=int, default=0,
                        help="exit after N messages (0 = never)")
    ARGS = parser.parse_args()

    SERVER = Server((ARGS.host, ARGS.port), Handler)
    print("[mock] listening on http://%s:%d (POST /api/metadata)"
          % (ARGS.host, ARGS.port), flush=True)
    try:
        SERVER.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        SERVER.server_close()
        print("[mock] stopped. frames=%d status=%d objects=%d total=%d"
              % (STATE["frames"], STATE["status"], STATE["objects"], STATE["total"]),
              flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
