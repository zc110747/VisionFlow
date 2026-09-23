// Canvas drawing for the AI overlay.
//
// Pure 2D-context calls: no Vue, no DOM lookup, no module state. It lives
// outside the component for one reason - the headless probe
// (scripts/verify_hand_overlay.py) drives these exact functions with a
// RECORDING context and asserts what was drawn (how many bones, which
// coordinates, whether anything was clamped). That is the only way to check
// overlay geometry without a browser, and it deliberately tests the shipped
// code rather than a re-implementation of it.

import {
  skeletonFor,
  HAND_CONNECTIONS,
  HAND_KEYPOINT_COUNT,
  HAND_TIP_INDICES,
  HAND_WRIST_INDEX
} from './skeleton'

/** Keypoints below this confidence are skipped (pose only). A spurious joint
 *  would otherwise draw a stray line right across the frame. */
export const KP_MIN_CONF = 0.2

/** Draw pose keypoint dots + skeleton connections for one object.
 *
 *  `kps` are [x, y, conf] in ORIGINAL video pixels; sx/sy scale to the canvas.
 *  An arity with no known topology draws nothing - see skeletonFor(). */
export function drawSkeleton(
  ctx: CanvasRenderingContext2D,
  kps: [number, number, number][],
  sx: number,
  sy: number
) {
  const connections = skeletonFor(kps.length)
  if (!connections) return

  // Connections first (under the dots).
  ctx.strokeStyle = 'rgba(255, 196, 0, 0.9)'
  ctx.lineWidth = 2
  for (const [a, b] of connections) {
    const ka = kps[a]
    const kb = kps[b]
    if (!ka || !kb) continue
    if (ka[2] < KP_MIN_CONF || kb[2] < KP_MIN_CONF) continue
    ctx.beginPath()
    ctx.moveTo(ka[0] * sx, ka[1] * sy)
    ctx.lineTo(kb[0] * sx, kb[1] * sy)
    ctx.stroke()
  }
  // Dots on top.
  ctx.fillStyle = 'rgba(255, 240, 170, 0.95)'
  for (const k of kps) {
    if (k[2] < KP_MIN_CONF) continue
    ctx.beginPath()
    ctx.arc(k[0] * sx, k[1] * sy, 2.5, 0, Math.PI * 2)
    ctx.fill()
  }
}

/** Draw the 21 hand landmarks. Only x/y are used: z is the model's wrist-
 *  relative depth, and the overlay is a 2D projection of it.
 *
 *  NO CLAMPING - on purpose, and the probe pins it. The agent clamps the hand
 *  bbox but NOT the points (carmera-agent/src/ai/hand_detector.cpp), so a hand
 *  at the frame edge really does report points outside [0,w]x[0,h]; the canvas
 *  clips them. Pulling them back inside would draw a skeleton that no longer
 *  matches the hand, and the mismatch would be blamed on the tracker. */
export function drawHandLandmarks(
  ctx: CanvasRenderingContext2D,
  lm: [number, number, number][] | undefined,
  sx: number,
  sy: number
) {
  // Landmarks can be switched off on the agent (--no-ai-hand-landmarks). The
  // bbox and the HUD still render, so this is not an error path.
  if (!lm || lm.length !== HAND_KEYPOINT_COUNT) return

  ctx.strokeStyle = 'rgba(0, 220, 200, 0.85)'
  ctx.lineWidth = 2
  for (const [a, b] of HAND_CONNECTIONS) {
    const pa = lm[a]
    const pb = lm[b]
    if (!pa || !pb) continue
    ctx.beginPath()
    ctx.moveTo(pa[0] * sx, pa[1] * sy)
    ctx.lineTo(pb[0] * sx, pb[1] * sy)
    ctx.stroke()
  }

  ctx.fillStyle = 'rgba(255, 240, 170, 0.95)'
  for (const k of lm) {
    ctx.beginPath()
    ctx.arc(k[0] * sx, k[1] * sy, 2.5, 0, Math.PI * 2)
    ctx.fill()
  }

  // Fingertips a little larger, wrist highlighted and on top: in a 2D
  // projection those two cues give the hand's direction at a glance.
  ctx.fillStyle = 'rgba(255, 196, 0, 0.95)'
  for (const i of HAND_TIP_INDICES) {
    const k = lm[i]
    if (!k) continue
    ctx.beginPath()
    ctx.arc(k[0] * sx, k[1] * sy, 3.5, 0, Math.PI * 2)
    ctx.fill()
  }
  const wrist = lm[HAND_WRIST_INDEX]
  if (wrist) {
    ctx.fillStyle = '#00dcc8'
    ctx.beginPath()
    ctx.arc(wrist[0] * sx, wrist[1] * sy, 4.5, 0, Math.PI * 2)
    ctx.fill()
  }
}
