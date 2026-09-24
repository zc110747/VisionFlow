#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
P4 acceptance: the hand pipeline, end to end, against a running joint stack.

This is the whole-chain script the P1-P3 stages were building towards. It talks
only to the deployed server over HTTP (never to the agent, never to the DB) and
asserts the chain one layer at a time:

  [A] link            server healthy, MediaMTX actually publishing the stream
  [B] mode applied    the AGENT runs ai-hand, not merely "the server was told to"
  [C] frame contract  the hand block the API serves matches what the UI reads
  [D] recorder        append-only log + export shape + BYTE fidelity of storage
  [E] session split   gap_ms takes effect in BOTH directions
  [F] state machine   reference latch, delta gating, motion vocabulary
  [G] mode round trip ai-hand -> ai-off -> ai-hand -> ai-y, with no hand leak
  [H] error contract  an invalid mode is rejected AND the message is not stale

Two design notes that are easy to get wrong, both measured rather than assumed:

  * ai-off does NOT stop the agent. carmera-agent's AIMode enum has no "Off"
    member and aimode_from_string() rejects the token on purpose - the agent
    keeps its model and sends no notify-back (ai_types.h). So the frames keep
    reporting ai_mode == "ai-hand" after the server is told "ai-off". Check G2
    pins that down, because "the mode read back as ai-off" is the whole of the
    web-side contract and anyone reading it as a bug would "fix" the agent.

  * ai_mode on a frame is stamped by the AGENT from its own current_mode_
    (ai_pipeline.cpp), so B3 is real evidence. A server that merely stored a
    preference cannot make this check pass.

No hand needed for [A] [D] [E] [G] [H]. [B] [C] [F] need one in view; with no
hand those report SKIP and the exit code becomes 2, which is a setup condition
and not a defect - pass --require-hand to turn them into failures instead.

Methodology follows scripts/verify_metadata.py: print a PASS/FAIL tally, exit
non-zero if anything failed.

Every request goes through a proxy-free opener. A host HTTP proxy answers
502 with "upstream connect failed" - a string that exists nowhere in this
repository - so a default opener makes a healthy server look broken.

Usage:
  python scripts/verify_hand.py
  python scripts/verify_hand.py --base-url http://127.0.0.1:8081 --camera camera01
  python scripts/verify_hand.py --require-hand
  python scripts/verify_hand.py --fast          # skip [E] and [F] (archive work)
"""

from __future__ import annotations

import argparse
import json
import sys
import time
import urllib.error
import urllib.request
from datetime import datetime, timezone

PASS = 0
FAIL = 0
SKIP = 0
HAND_SKIPS = 0

# A camera id that no agent ever publishes to, used for the isolated fidelity
# probe in [D]. createCamera only writes a repository row - it does not touch
# MediaMTX - so creating and deleting this leaves no trace on the real stack.
PROBE_CAM = "vf-hand-probe"

# Every path the UI reads off a hand block. Verified against
# internal/metadata/model.go (HandState), not guessed from the field names.
HAND_PATHS = [
    "detected",
    "confidence",
    "handedness",
    "reference_initialized",
    "position",
    "reference",
    "delta",
    "motion.x",
    "motion.y",
    "motion.z",
    "gesture",
    "gesture_candidate",
    "gesture_confidence",
]

# The top-level keys of a hand block on the wire. HAND_PATHS above uses dotted
# paths for nested lookups, so `motion` appears here where motion.x/y/z do
# there. Used to check a RECORDED block, which must carry the whole set whether
# or not a hand was detected in that particular frame.
HAND_KEYS = [
    "detected", "confidence", "handedness", "reference_initialized",
    "position", "reference", "delta", "motion",
    "gesture", "gesture_candidate", "gesture_confidence", "landmarks",
]

# The hand block goes on the wire with 4 decimal places, so an identity that is
# exact in the agent can be off by up to 1.5e-4 once each term is rounded
# independently. Measured worst case on the archive: 1.000e-04.
TOL = 2e-4

# The full vocabulary the server accepts. Kept here so [H] can assert that the
# rejection message names all of it.
VALID_MODES = ["ai-off", "ai-y", "ai-y-pose", "ai-hand"]

MOTION_TOKENS = {
    "STATIONARY", "LEFT", "RIGHT", "FORWARD", "BACKWARD", "UP", "DOWN",
}

# No proxy: see the module docstring.
OPENER = urllib.request.build_opener(urllib.request.ProxyHandler({}))


# ---------------------------------------------------------------------------
# plumbing
# ---------------------------------------------------------------------------
def check(name, ok, detail=""):
    global PASS, FAIL
    if ok:
        PASS += 1
        print(f"  [PASS] {name}" + (f" :: {detail}" if detail else ""))
    else:
        FAIL += 1
        print(f"  [FAIL] {name}" + (f" :: {detail}" if detail else ""))
    return ok


def note(name, detail=""):
    global SKIP
    SKIP += 1
    print(f"  [SKIP] {name}" + (f" :: {detail}" if detail else ""))


def note_hand(name, detail=""):
    """A skip that is specifically "no hand in view"."""
    global SKIP, HAND_SKIPS
    HAND_SKIPS += 1
    note(name, detail)


def http(method, url, body=None, timeout=20):
    """Returns (status, headers, text). Never raises for an HTTP error status:
    a 4xx here is a result to assert on, not an exception."""
    data = None
    if body is not None:
        data = json.dumps(body).encode("utf-8")
    req = urllib.request.Request(url, data=data, method=method)
    if data is not None:
        req.add_header("Content-Type", "application/json")
    try:
        with OPENER.open(req, timeout=timeout) as r:
            return r.status, dict(r.headers), r.read().decode("utf-8", "replace")
    except urllib.error.HTTPError as e:
        return e.code, dict(e.headers or {}), e.read().decode("utf-8", "replace")


def get_json(url, timeout=20):
    st, _, txt = http("GET", url, timeout=timeout)
    if st != 200:
        raise RuntimeError(f"GET {url} -> {st}: {txt[:200]}")
    return json.loads(txt)


def get(obj, dotted):
    """Dotted lookup returning (found, value), so a legitimate false/0 is not
    confused with an absent field."""
    cur = obj
    for key in dotted.split("."):
        if not isinstance(cur, dict) or key not in cur:
            return False, None
        cur = cur[key]
    return True, cur


def parse_ts(s):
    if not s:
        return None
    try:
        return datetime.fromisoformat(str(s).replace("Z", "+00:00"))
    except ValueError:
        return None


def wait_for_frame(base, cam, pred, timeout):
    """Poll the live snapshot until pred(frame) holds. Returns (frame, waited)."""
    t0 = time.time()
    last = {}
    while True:
        try:
            snap = get_json(f"{base}/api/cameras/{cam}/metadata")
            last = snap.get("frame") or {}
        except Exception:
            last = {}
        if last and pred(last):
            return last, time.time() - t0
        if time.time() - t0 >= timeout:
            return last, time.time() - t0
        time.sleep(0.5)


def wait_for_mode(base, cam, want, timeout):
    """Wait for a frame the agent produced AFTER this call started, whose mode
    is `want`. The received_at floor is what makes it evidence of a switch
    rather than of a stale snapshot."""
    floor = datetime.now(timezone.utc)

    def pred(fr):
        ra = parse_ts(fr.get("received_at"))
        return fr.get("ai_mode") == want and ra is not None and ra >= floor

    return wait_for_frame(base, cam, pred, timeout)


def set_mode(base, cam, mode):
    return http("POST", f"{base}/api/cameras/{cam}/aimode", {"mode": mode})


def describe(fr):
    ra = parse_ts(fr.get("received_at"))
    age = f"{(datetime.now(timezone.utc) - ra).total_seconds():.1f}s ago" if ra else "?"
    return (f"frame_id={fr.get('frame_id')} ai_mode={fr.get('ai_mode')!r} "
            f"age={age} hand={'yes' if fr.get('hand') else 'no'}")


# ---------------------------------------------------------------------------
# layers
# ---------------------------------------------------------------------------
def layer_a(base, cam, mtx):
    print("\n[A] link")
    try:
        h = get_json(f"{base}/api/health")
    except Exception as e:
        check("GET /api/health", False, str(e))
        return False
    check("server reports healthy (db + media server)",
          h.get("status") == "ok" and h.get("database") == "ok"
          and h.get("media_server") == "ok", json.dumps(h))

    try:
        pl = get_json(f"{mtx}/v3/paths/list")
    except Exception as e:
        check("MediaMTX path is published", False, f"GET {mtx}/v3/paths/list: {e}")
        return True
    items = pl.get("items") or []
    names = [i.get("name") for i in items]
    live = [i for i in items if i.get("name") == cam]
    # The decisive readiness probe. /api/cameras carries the DB row and is
    # happy to list a camera that has never published a byte.
    check("the camera's MediaMTX path is online", bool(live) and live[0].get("online") is True,
          f"paths={names} online={[i.get('online') for i in live] or None}")
    check("the stream is ready to serve", bool(live) and live[0].get("ready") is True,
          f"ready={[i.get('ready') for i in live] or None}")

    st, _, txt = http("GET", f"{base}/api/cameras/{cam}")
    check("GET /api/cameras/{id} returns the camera", st == 200,
          f"status={st} {txt[:120]}")
    return True


def layer_b(base, cam, timeout, require_hand):
    print("\n[B] the agent actually applied ai-hand")
    st, _, txt = set_mode(base, cam, "ai-hand")
    if not check("POST aimode ai-hand is accepted", st == 200, f"status={st} {txt[:120]}"):
        return None

    st, _, txt = http("GET", f"{base}/api/cameras/{cam}/aimode")
    try:
        cur = json.loads(txt).get("mode")
    except Exception:
        cur = None
    check("the stored mode reads back as ai-hand", st == 200 and cur == "ai-hand",
          f"status={st} mode={cur!r}")

    fr, waited = wait_for_mode(base, cam, "ai-hand", timeout)
    ok = fr.get("ai_mode") == "ai-hand"
    check("the AGENT switches to ai-hand (frame is stamped by the agent)", ok,
          f"after {waited:.1f}s :: {describe(fr)}")
    if not ok:
        return fr

    hand = fr.get("hand")
    if not hand:
        msg = f"{describe(fr)} - no hand in view"
        if require_hand:
            check("the ai-hand frame carries a hand block", False, msg)
        else:
            note_hand("the ai-hand frame carries a hand block", msg)
        return fr

    check("the ai-hand frame carries a hand block", True, describe(fr))
    return fr


def layer_c(fr, cam):
    print("\n[C] frame contract")
    hand = fr.get("hand")
    if not hand:
        note_hand("hand block contract", "no hand block in the live frame")
        return None

    missing = [p for p in HAND_PATHS if not get(hand, p)[0]]
    check("every field the UI reads exists on the block", not missing,
          ", ".join(missing) if missing else f"{len(HAND_PATHS)} paths")

    if not hand.get("detected"):
        # The block is emitted whenever the agent runs ai-hand; `detected` says
        # whether a hand was actually found. With nothing found there are no
        # landmarks to check, so these are setup conditions, not defects.
        note_hand("landmark + object checks",
                  "hand block present but detected=false - nothing to check")
        return hand

    lm = hand.get("landmarks")
    check("the block carries 21 landmarks", isinstance(lm, list) and len(lm) == 21,
          f"len={len(lm) if isinstance(lm, list) else None}")

    objs = fr.get("objects") or []
    hand_objs = [o for o in objs if o.get("class") == "hand"]
    if hand_objs:
        kps = hand_objs[0].get("keypoints")
        same = (isinstance(kps, list) and isinstance(lm, list) and len(kps) == len(lm)
                and all(abs(a[0] - b[0]) < 1e-6 and abs(a[1] - b[1]) < 1e-6
                        for a, b in zip(kps, lm)))
        check("objects[0].keypoints mirrors hand.landmarks (skeleton drawn once)",
              same, f"objects={len(kps) if isinstance(kps, list) else None} "
                    f"hand={len(lm) if isinstance(lm, list) else None}")
        bbox = hand_objs[0].get("bbox")
        check("the hand object carries a sane bbox",
              isinstance(bbox, list) and len(bbox) == 4
              and bbox[0] < bbox[2] and bbox[1] < bbox[3], f"bbox={bbox}")
        if isinstance(bbox, list) and len(bbox) == 4 and fr.get("video_height"):
            # Evidence, not a gate. The agent sizes the hand box to the frame
            # height, so the box itself paints two full-height vertical lines on
            # the overlay; a painted-pixel assertion is therefore a UNION check
            # (box + label + skeleton), never a skeleton-only check.
            full = bbox[1] == 0 and bbox[3] == fr.get("video_height")
            print(f"  note: hand bbox spans the frame height (union-overlay trap): {full}")
    else:
        note("objects[] carries the hand object",
             "no class=='hand' object in this frame")

    if isinstance(lm, list) and lm and fr.get("video_width") and fr.get("video_height"):
        w, h = fr["video_width"], fr["video_height"]
        oob = sum(1 for k in lm if k[0] < 0 or k[0] > w or k[1] < 0 or k[1] > h)
        # The agent clamps the BOX, deliberately not the points (hand_detector.cpp).
        print(f"  note: {oob}/{len(lm)} landmarks outside the {w}x{h} frame "
              f"(allowed by design; the magnitude is what matters)")
    return hand


def layer_d(base, cam, sessions):
    print("\n[D] recorder + export fidelity")
    check("the recorder has rows", sessions.get("total_rows", 0) > 0,
          f"total_rows={sessions.get('total_rows')} gap_ms={sessions.get('gap_ms')}")
    sess = sessions.get("sessions") or []
    check("the split produces at least one session", bool(sess),
          f"{len(sess)} session(s)")
    if not sess:
        return

    best = max(sess, key=lambda s: s.get("detected_count", 0))
    idx = best.get("index")
    st, hdr, txt = http("GET", f"{base}/api/cameras/{cam}/hand/sessions/{idx}")
    if not check(f"GET /hand/sessions/{idx} returns the export", st == 200,
                 f"status={st} {txt[:120]}"):
        return
    cd = hdr.get("Content-Disposition", "")
    check("the download is named hand_session_<camera>_<index>.json",
          f"hand_session_{cam}_{idx}.json" in cd, cd or "(no Content-Disposition)")

    try:
        doc = json.loads(txt)
    except Exception as e:
        check("the export is valid JSON", False, str(e))
        return
    want = {"version", "camera_id", "exported_at", "session", "gap_ms", "frames"}
    got = set(doc.keys())
    check("the export carries the documented top-level keys", want <= got,
          f"missing={sorted(want - got)} extra={sorted(got - want)}")

    rows = doc.get("frames") or []
    check("the export carries frames", bool(rows), f"{len(rows)} row(s)")
    if not rows:
        return

    ids = [r.get("id") for r in rows]
    mono = all(isinstance(i, int) for i in ids) and ids == sorted(ids) \
        and len(set(ids)) == len(ids)
    check("rows are ordered by the append-only key, strictly increasing", mono,
          f"first={ids[0]} last={ids[-1]} n={len(ids)}")
    check("the row keeps the segmenting columns promoted out of the payload",
          all(r.get("frame_id") is not None and r.get("timestamp") is not None
              for r in rows), "frame_id + timestamp on every row")

    hand_rows = [r for r in rows if r.get("hand")]
    if hand_rows:
        # Checked against the field list rather than against the live frame: the
        # live block carries no landmarks while nothing is detected, so a
        # live-vs-saved diff would report a difference on every idle sample.
        # The strong fidelity proof is the sentinel probe below.
        saved = set(hand_rows[-1]["hand"].keys())
        expect = set(HAND_KEYS)
        check("a saved hand block carries the whole documented field set",
              expect <= saved, f"missing={sorted(expect - saved)}")
    else:
        note_hand("a saved row carries a hand block", "no recorded hand block")


def probe_fidelity(base):
    """The one check that proves byte fidelity rather than trusting the comment.

    A dedicated camera id keeps this out of the real camera's log, so it runs
    with the agent live and needs no hand. The sentinel is a field the server's
    HandState does NOT model: if storage re-serialised the struct instead of
    keeping the producer's bytes, it would vanish from the export.
    """
    nonce = f"keep-me-{int(time.time())}"
    created = ensure_probe(base)
    try:
        http("DELETE", f"{base}/api/cameras/{PROBE_CAM}/hand/log")
        row = probe_frame(1, 1, True)
        row["hand"]["vf_probe_sentinel"] = nonce
        st, _, txt = http("POST", f"{base}/api/metadata", row)
        if not check("isolated fidelity probe: a synthetic frame is accepted",
                     st in (200, 202, 204), f"status={st} {txt[:120]}"):
            return
        doc = get_json(f"{base}/api/cameras/{PROBE_CAM}/hand/sessions/1")
        got = None
        for r in doc.get("frames") or []:
            if r.get("hand", {}).get("vf_probe_sentinel"):
                got = r["hand"]["vf_probe_sentinel"]
        # This is the assertion the P2 design note makes. Storing a
        # re-serialisation of the known struct would drop the field.
        check("an unknown field survives storage verbatim (no re-serialisation)",
              got == nonce, f"got={got!r} want={nonce!r}")
    finally:
        drop_probe(base, created)


def layer_e(base, cam, sessions):
    print("\n[E] session split responds to gap_ms")
    default_n = len(sessions.get("sessions") or [])
    try:
        tiny = get_json(f"{base}/api/cameras/{cam}/hand/sessions?gap_ms=1")
        huge = get_json(f"{base}/api/cameras/{cam}/hand/sessions?gap_ms=3600000")
    except Exception as e:
        check("gap_ms accepts an override", False, str(e))
        return
    n_tiny = len(tiny.get("sessions") or [])
    n_huge = len(huge.get("sessions") or [])
    check("a 1 ms gap splits at least as finely as the default",
          n_tiny >= default_n, f"tiny={n_tiny} default={default_n}")
    check("a 1 h gap merges at least as much as the default",
          n_huge <= default_n, f"huge={n_huge} default={default_n}")
    check("the split is monotonic in gap_ms: tiny >= default >= huge",
          n_tiny >= default_n >= n_huge,
          f"tiny={n_tiny} default={default_n} huge={n_huge}")
    if n_huge == default_n:
        # Deliberate, not a defect: a session boundary is EITHER a time gap OR a
        # row that says the hand left the frame, and the two are independent -
        # no gap is wide enough to merge across a loss (see the split tests).
        # A log accumulated over days is split by losses, not by time, so the
        # merge direction cannot be shown on it. It is shown on the fixture.
        note("the merge direction on this live log",
             f"huge == default == {n_huge}: every boundary here is a lost-hand "
             "boundary, which no gap can merge. The controlled fixture below "
             "proves the merge direction.")
    else:
        check("widening the gap merged runs on the live log", True,
              f"huge={n_huge} < default={default_n}")
    st, _, txt = http("GET", f"{base}/api/cameras/{cam}/hand/sessions?gap_ms=abc")
    check("a non-numeric gap_ms is a 400, not a silent default", st == 400,
          f"status={st} {txt[:120]}")

    probe_split(base)


def probe_frame(fid, ts, detected):
    """One synthetic ai-hand frame, used by the controlled fixtures."""
    return {
        "version": 1, "type": "frame", "camera_id": PROBE_CAM,
        "frame_id": fid, "timestamp": ts,
        "video_width": 1280, "video_height": 720,
        "ai_mode": "ai-hand", "objects": [],
        "hand": {
            "detected": detected, "confidence": 0.9, "handedness": "Left",
            "reference_initialized": False,
            "position": [0.5, 0.5, 0.5], "reference": [0.0, 0.0, 0.0],
            "delta": [0.0, 0.0, 0.0],
            "motion": {"x": "STATIONARY", "y": "STATIONARY", "z": "STATIONARY"},
            "gesture": "UNKNOWN", "gesture_candidate": "UNKNOWN",
            "gesture_confidence": 0.0, "landmarks": [],
        },
    }


def ensure_probe(base):
    """Create the dedicated probe camera. Returns True if this call made it.

    createCamera only writes a repository row - it does not touch MediaMTX - so
    this leaves no trace on the real stack and runs while the agent is live.
    """
    st, _, txt = http("POST", f"{base}/api/cameras",
                      {"id": PROBE_CAM, "name": "hand probe",
                       "stream_path": PROBE_CAM})
    if st == 201:
        return True
    if st in (200, 409, 500):
        return False          # already there; the callers clear the log anyway
    check(f"probe camera {PROBE_CAM} is available", False,
          f"status={st} {txt[:120]}")
    return False


def drop_probe(base, created):
    http("DELETE", f"{base}/api/cameras/{PROBE_CAM}/hand/log")
    if created:
        http("DELETE", f"{base}/api/cameras/{PROBE_CAM}")


def probe_split(base):
    """Both directions of gap_ms, and the rule no gap can override, on frames
    placed exactly.

    Asserting the merge direction against the live log is a trap: it holds three
    weeks of recordings, so its boundaries are lost-hand boundaries and no gap
    merges them. A fixture whose timestamps are chosen makes the rule visible.
    """
    created = ensure_probe(base)
    try:
        http("DELETE", f"{base}/api/cameras/{PROBE_CAM}/hand/log")
        for f in (probe_frame(1, 0, True), probe_frame(2, 5000, True)):
            http("POST", f"{base}/api/metadata", f)
        n_small = len(get_json(
            f"{base}/api/cameras/{PROBE_CAM}/hand/sessions?gap_ms=1000"
        ).get("sessions") or [])
        n_big = len(get_json(
            f"{base}/api/cameras/{PROBE_CAM}/hand/sessions?gap_ms=10000"
        ).get("sessions") or [])
        check("fixture: two detections 5 s apart split at gap_ms=1000",
              n_small == 2, f"{n_small} session(s)")
        check("fixture: widening the gap to 10000 merges them into one",
              n_big == 1, f"{n_big} session(s)")

        # The independent criterion: a row that says the hand left the frame
        # ends a run whatever the gap is.
        http("DELETE", f"{base}/api/cameras/{PROBE_CAM}/hand/log")
        for f in (probe_frame(1, 0, True), probe_frame(2, 100, False),
                  probe_frame(3, 200, True)):
            http("POST", f"{base}/api/metadata", f)
        n_loss = len(get_json(
            f"{base}/api/cameras/{PROBE_CAM}/hand/sessions?gap_ms=100000"
        ).get("sessions") or [])
        check("fixture: no gap merges across a LOST hand (criteria are independent)",
              n_loss == 2, f"{n_loss} session(s) at gap_ms=100000")
    finally:
        drop_probe(base, created)


def layer_f(base, cam, sessions):
    print("\n[F] state machine, read back out of the archive")
    sess = [s for s in (sessions.get("sessions") or []) if s.get("detected_count", 0) > 0]
    if not sess:
        note_hand("state machine checks", "no recorded session with a detected hand")
        return
    best = max(sess, key=lambda s: s.get("detected_count", 0))
    try:
        doc = get_json(f"{base}/api/cameras/{cam}/hand/sessions/{best['index']}")
    except Exception as e:
        check("the archive is readable for state analysis", False, str(e))
        return
    rows = [r for r in (doc.get("frames") or []) if r.get("hand")]
    check("the archive is readable for state analysis", bool(rows), f"{len(rows)} row(s)")
    if not rows:
        return

    latch = None
    for i, r in enumerate(rows):
        if r["hand"].get("reference_initialized"):
            latch = i
            break
    if latch is None:
        note_hand("a reference latch is observed (false -> true)",
                  "every sampled row is already latched or none ever latched")
    else:
        check("a reference latch is observed (false -> true)", latch > 0,
              f"latched at row {latch} of {len(rows)}")
        flapped = [i for i, r in enumerate(rows)
                   if i > latch and not r["hand"].get("reference_initialized")]
        check("once latched, it does not flap back inside the session", not flapped,
              f"unlatched again at rows {flapped[:5]}")

    bad = []
    worst = 0.0
    for r in rows:
        h = r["hand"]
        if not h.get("reference_initialized"):
            continue
        pos, ref, dlt = h.get("position"), h.get("reference"), h.get("delta")
        if not (isinstance(pos, list) and isinstance(ref, list)
                and isinstance(dlt, list) and len(pos) == len(ref) == len(dlt) == 3):
            bad.append(r.get("id"))
            continue
        for k in range(3):
            d = abs((pos[k] - ref[k]) - dlt[k])
            worst = max(worst, d)
            if d > TOL:
                bad.append(r.get("id"))
                break
    # The payload carries 4 decimal places, so round4(a) - round4(b) differs
    # from round4(a - b) by up to 1.5e-4 even when the relationship is exact.
    # Measured on the archive: the largest discrepancy is 1.000e-04. A tighter
    # tolerance than that reports the wire precision as a defect.
    check("delta == position - reference once the reference is latched", not bad,
          f"{len(bad)} inconsistent row(s), worst |diff| = {worst:.3e} "
          f"(tolerance {TOL:.1e} = the 4-decimal wire precision)")

    toks = set()
    for r in rows:
        for ax in ("x", "y", "z"):
            v = r["hand"].get("motion", {}).get(ax)
            if v is not None:
                toks.add(v)
    unknown = sorted(toks - MOTION_TOKENS)
    check("every motion token is in the documented vocabulary", not unknown,
          f"seen={sorted(toks)} unknown={unknown}")


def layer_g(base, cam, timeout, require_hand):
    print("\n[G] mode round trip")
    floor = datetime.now(timezone.utc)
    st, _, _ = set_mode(base, cam, "ai-off")
    st2, _, txt2 = http("GET", f"{base}/api/cameras/{cam}/aimode")
    try:
        cur = json.loads(txt2).get("mode")
    except Exception:
        cur = None
    check("ai-off is accepted and reads back on the control plane",
          st == 200 and cur == "ai-off", f"POST={st} GET mode={cur!r}")

    # By design the agent keeps its model and sends no notify-back, so the
    # frames must keep reporting ai-hand. Asserting this pins the documented
    # semantics: a future change that started reporting ai-off here would break
    # the web's gate, and a "fix" that stopped the agent would break the stream.
    # Freshness is asserted explicitly rather than folded into the poll: a stale
    # snapshot also says ai-hand, so accepting one would prove nothing about
    # whether the agent is still running.
    def fresh(f):
        ra = parse_ts(f.get("received_at"))
        return ra is not None and ra >= floor

    fr, waited = wait_for_frame(base, cam, fresh, min(timeout, 6))
    ra = parse_ts(fr.get("received_at"))
    check("while ai-off, the agent keeps publishing and still stamps ai-hand",
          (ra is not None and ra >= floor) and fr.get("ai_mode") == "ai-hand",
          f"after {waited:.1f}s :: {describe(fr)} (documented: ai-off means the "
          "web stops drawing, not that the agent unloads)")

    st, _, _ = set_mode(base, cam, "ai-hand")
    fr, waited = wait_for_mode(base, cam, "ai-hand", timeout)
    check("switching back to ai-hand is applied by the agent",
          fr.get("ai_mode") == "ai-hand", f"after {waited:.1f}s :: {describe(fr)}")
    if fr.get("hand"):
        check("the hand block comes back after the round trip", True, describe(fr))
    else:
        msg = f"{describe(fr)} - no hand in view"
        if require_hand:
            check("the hand block comes back after the round trip", False, msg)
        else:
            note_hand("the hand block comes back after the round trip", msg)

    set_mode(base, cam, "ai-y")
    fr, waited = wait_for_mode(base, cam, "ai-y", timeout)
    check("switching to ai-y is applied by the agent",
          fr.get("ai_mode") == "ai-y", f"after {waited:.1f}s :: {describe(fr)}")
    # The pose->detect leak guard has a hand-side twin: a detect frame must not
    # carry a hand block, or the UI would draw a stale skeleton over ai-y.
    check("an ai-y frame carries NO hand block (no leak)", fr.get("hand") is None,
          f"hand={'present' if fr.get('hand') else 'absent'}")

    st, _, txt = set_mode(base, cam, "ai-hand")
    fr, waited = wait_for_mode(base, cam, "ai-hand", timeout)
    check("the stack is left in ai-hand for the operator", fr.get("ai_mode") == "ai-hand",
          f"after {waited:.1f}s :: {describe(fr)}")


def layer_h(base, cam):
    print("\n[H] error contract")
    st, _, txt = http("POST", f"{base}/api/cameras/{cam}/aimode", {"mode": "ai-bogus"})
    check("an unknown mode is rejected with 400", st == 400, f"status={st} {txt[:120]}")
    omitted = [m for m in VALID_MODES if m not in txt]
    # A rejection message that lists only the modes this build remembers sends
    # an API client down the wrong path: it reads as "ai-hand is not a thing".
    check("the rejection message names every mode the server accepts", not omitted,
          f"omitted={omitted} message={txt.strip()[:160]}")
    st, _, txt = http("POST", f"{base}/api/cameras/{cam}/aimode", {})
    check("a missing mode is a 400", st == 400, f"status={st} {txt[:120]}")


def finish():
    print("\n" + "=" * 70)
    print(f" PASS={PASS}  FAIL={FAIL}  SKIP={SKIP}  (hand-dependent skips={HAND_SKIPS})")
    print("=" * 70)
    if FAIL:
        return 1
    if HAND_SKIPS:
        print(" Exit 2: the stack is fine, but a hand was never in view, so the")
        print(" hand-dependent layers could not be judged. Re-run with a hand up,")
        print(" or add --require-hand to make those hard failures.")
        return 2
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--base-url", default="http://127.0.0.1:8081")
    ap.add_argument("--camera", default="camera01")
    ap.add_argument("--mtx-api", default="http://127.0.0.1:9997")
    ap.add_argument("--mode-timeout", type=float, default=30.0,
                    help="seconds to wait for the agent to apply a mode")
    ap.add_argument("--require-hand", action="store_true",
                    help="treat 'no hand in view' as a failure instead of a skip")
    ap.add_argument("--fast", action="store_true",
                    help="skip [E] and [F], which need archived frames")
    args = ap.parse_args()

    base = args.base_url.rstrip("/")
    mtx = args.mtx_api.rstrip("/")
    cam = args.camera

    print("=" * 70)
    print(" P4 hand pipeline acceptance (end to end)")
    print(f" server : {base}")
    print(f" camera : {cam}")
    print(f" mtx    : {mtx}")
    print("=" * 70)

    if not layer_a(base, cam, mtx):
        return finish()

    live = layer_b(base, cam, args.mode_timeout, args.require_hand)
    layer_c(live or {}, cam)

    try:
        sessions = get_json(f"{base}/api/cameras/{cam}/hand/sessions")
    except Exception as e:
        check("GET /hand/sessions", False, str(e))
        sessions = {}
    if sessions:
        layer_d(base, cam, sessions)
        if args.fast:
            note("session split + state machine layers", "--fast")
        else:
            layer_e(base, cam, sessions)
            layer_f(base, cam, sessions)

    probe_fidelity(base)
    layer_g(base, cam, args.mode_timeout, args.require_hand)
    layer_h(base, cam)
    return finish()


if __name__ == "__main__":
    sys.exit(main())
