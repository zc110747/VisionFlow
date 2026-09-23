#!/usr/bin/env python3
"""verify_web_deploy.py - is the RUNNING server serving the UI we just built?

Why this exists
---------------
The Web UI is compiled into web/dist and captured by go:embed at COMPILE time.
So a running video-server.exe can happily serve a UI that is several edits
older than web/src, and every symptom of that looks like "my frontend change
did nothing": no error, no warning, the page just behaves like the old code.

Rebuilding the frontend is therefore NOT enough - the binary must be rebuilt
AND the old process must be gone. This script closes that loop: it asks the
RUNNING process for the page, and compares it byte for byte against what is on
disk right now.

Checks
------
  [A] the server answers /api/health
  [B] GET / returns the SPA and its asset references match web/dist/index.html
  [C] every asset the running server references is BYTE-IDENTICAL to the file
      on disk (same sha256) - this is the check that catches a stale binary
  [D] the served bundle carries the UI markers we expect (frontend features)
  [E] the AI mode the server reports matches the requested one

Exit code 0 only when every check passes.

Usage
-----
  python scripts/verify_web_deploy.py
  python scripts/verify_web_deploy.py --host 127.0.0.1:8081 --ai-mode ai-hand
  python scripts/verify_web_deploy.py --wait 60      # poll for readiness first
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import sys
import time
import urllib.error
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
WEB = os.path.join(HERE, "..", "web")
DIST = os.path.join(WEB, "dist")

# Asset references live in index.html as ./assets/index-<hash>.{js,css}.
ASSET_RE = re.compile(rb"(?:\./)?assets/(index-[A-Za-z0-9_-]+\.(?:js|css))")

# Strings that only exist in the UI when the frontend features are present.
# Kept deliberately narrow - a marker that survives an unrelated refactor is a
# marker that stops detecting the thing it was added for.
DEFAULT_MARKERS = [
    "ai-hand",          # AI mode is selectable in the UI
    "REF LATCHING",     # hand HUD honours reference_initialized=false
    "REF LOCKED",       # ... and reports the latched state
    "NO HAND",          # HUD renders even when nothing is detected
    "landmarkCount",    # hand overlay code is present in the bundle
]

PASS = 0
FAIL = 0


def check(name: str, ok: bool, detail: str = "") -> bool:
    global PASS, FAIL
    if ok:
        PASS += 1
        print(f"  [PASS] {name}" + (f"  ({detail})" if detail else ""))
    else:
        FAIL += 1
        print(f"  [FAIL] {name}" + (f"  -> {detail}" if detail else ""))
    return ok


def fetch(base: str, path: str, timeout: float = 5.0):
    """GET base+path. Returns (status, body). Raises on transport failure."""
    url = base.rstrip("/") + path
    req = urllib.request.Request(url, headers={"User-Agent": "verify_web_deploy"})
    # Never let an environment proxy sit between us and a loopback address: a
    # proxy answers 502 for its own upstream failures and the report would
    # blame the wrong component.
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    with opener.open(req, timeout=timeout) as r:
        return r.status, r.read()


def sha256(b: bytes) -> str:
    return hashlib.sha256(b).hexdigest()


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1:8081", help="host:port of the running server")
    ap.add_argument("--wait", type=float, default=0.0, help="seconds to wait for /api/health")
    ap.add_argument("--ai-mode", default="", help="expected mode for camera01 (e.g. ai-hand)")
    ap.add_argument(
        "--camera",
        default="camera01",
        help="camera id used for the aimode / metadata checks",
    )
    args = ap.parse_args()

    base = "http://" + args.host
    print(f"verify_web_deploy: {base}")
    print(f"  local dist: {os.path.normpath(DIST)}")

    # ---- [0] wait for readiness if asked to -------------------------------
    deadline = time.time() + max(args.wait, 0.0)
    last_err = "not tried"
    while True:
        try:
            status, _ = fetch(base, "/api/health", timeout=3.0)
            if status == 200:
                break
            last_err = f"HTTP {status}"
        except Exception as e:  # noqa: BLE001 - transport detail is the payload
            last_err = str(e)
        if time.time() >= deadline:
            check("server is reachable", False, last_err)
            print(f"\nRESULT: PASS={PASS} FAIL={FAIL}")
            return 1
        time.sleep(1.0)

    check("server is reachable", True, base)

    # ---- [A] health -------------------------------------------------------
    try:
        status, body = fetch(base, "/api/health")
        doc = json.loads(body)
        check(
            "GET /api/health returns 200 with status=ok",
            status == 200 and doc.get("status") == "ok",
            json.dumps(doc),
        )
    except Exception as e:  # noqa: BLE001
        check("GET /api/health returns 200 with status=ok", False, str(e))

    # ---- [B] the served page vs the local dist ----------------------------
    try:
        status, html = fetch(base, "/")
        check("GET / serves the Web UI", status == 200 and len(html) > 0, f"{len(html)} bytes")
    except Exception as e:  # noqa: BLE001
        check("GET / serves the Web UI", False, str(e))
        print(f"\nRESULT: PASS={PASS} FAIL={FAIL}")
        return 1

    served_refs = [m.decode() for m in ASSET_RE.findall(html)]
    # A proxy / error page has no asset references, so an empty list here means
    # the response is not the SPA at all - do not continue and blame the build.
    if not check(
        "the served page references at least one asset",
        len(served_refs) > 0,
        f"{len(served_refs)} asset ref(s)"
        if served_refs
        else "none found - not the SPA (a proxy or error page?)",
    ):
        print(f"\nRESULT: PASS={PASS} FAIL={FAIL}")
        return 1

    index_path = os.path.normpath(os.path.join(DIST, "index.html"))
    if not check("local web/dist/index.html exists", os.path.exists(index_path), index_path):
        print(f"\nRESULT: PASS={PASS} FAIL={FAIL}")
        return 1

    with open(index_path, "rb") as f:
        local_refs = [m.decode() for m in ASSET_RE.findall(f.read())]

    missing = sorted(set(local_refs) - set(served_refs))
    check(
        "the served page references exactly the assets built on disk",
        not missing and sorted(local_refs) == sorted(served_refs),
        f"served={served_refs} local={local_refs}",
    )

    # ---- [C] byte-identical assets  (this is the stale-binary check) ------
    identical = 0
    for name in served_refs:
        disk = os.path.normpath(os.path.join(DIST, "assets", name))
        if not check(f"asset on disk: {name}", os.path.exists(disk), disk):
            continue
        try:
            _, served = fetch(base, "/assets/" + name)
        except Exception as e:  # noqa: BLE001
            check(f"serve {name}", False, str(e))
            continue
        with open(disk, "rb") as f:
            local = f.read()
        same = sha256(served) == sha256(local)
        if same:
            identical += 1
        check(
            f"{name} served byte-identical to disk",
            same,
            f"served={len(served)}B/{sha256(served)[:12]} disk={len(local)}B/{sha256(local)[:12]}"
            + ("" if same else "  <-- STALE BINARY: the running exe embeds an older dist"),
        )

    # ---- [D] feature markers in the served bundle -------------------------
    js = next((n for n in served_refs if n.endswith(".js")), "")
    if js:
        try:
            _, bundle = fetch(base, "/assets/" + js)
            text = bundle.decode("utf-8", "replace")
            absent = [m for m in DEFAULT_MARKERS if m not in text]
            check(
                "served bundle carries the expected UI markers",
                not absent,
                f"missing={absent}" if absent else f"{len(DEFAULT_MARKERS)} marker(s) present",
            )
        except Exception as e:  # noqa: BLE001
            check("served bundle carries the expected UI markers", False, str(e))

    # ---- [E] AI mode actually in force ------------------------------------
    if args.ai_mode:
        try:
            _, body = fetch(base, f"/api/cameras/{args.camera}/aimode")
            doc = json.loads(body)
            check(
                f"{args.camera} AI mode is {args.ai_mode}",
                doc.get("mode") == args.ai_mode,
                f"reported {doc.get('mode')!r}",
            )
        except Exception as e:  # noqa: BLE001
            check(f"{args.camera} AI mode is {args.ai_mode}", False, str(e))

    print(f"\nRESULT: PASS={PASS} FAIL={FAIL}")
    if FAIL:
        print("NOTE: a FAIL on the byte-identical check means the RUNNING process")
        print("      is older than web/dist - rebuild and restart:")
        print("        start.bat --ai --ai-mode hand")
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main())
