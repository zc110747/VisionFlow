// Text derivation for the hand HUD and the side panel.
//
// Pure module - no DOM, no Vue, no fetch - for two reasons:
//   1. one source of truth: the overlay HUD and the side panel render the same
//      strings, so they can never disagree about what the agent sent;
//   2. it is testable in Node with a REAL payload. The headless probe imports
//      this file, feeds it a hand block fetched from the live server, and
//      asserts every field below is a string - which is the only cheap way to
//      catch a field renamed on the wire (that shows up as `undefined` on
//      screen, never as a build error).
//
// Everything returned is a string, always. A missing or non-finite number
// renders as "-" rather than "NaN" / "undefined", because those two strings on
// a dark overlay are indistinguishable from a broken build.

import type { HandState, Motion3 } from '@/api/client'

export interface HandHud {
  /** False when no hand block has been received yet. */
  present: boolean
  detected: boolean
  /** Gesture, or "NO HAND" when the detector saw nothing this frame. */
  title: string
  /** True when `title` is a hand actually in frame (drives the accent colour). */
  titleActive: boolean
  gesture: string
  gestureConfidence: string
  confidence: string
  handedness: string
  /** Pre-debounce gesture, only when it differs from the confirmed one. */
  candidate: string
  refLatched: boolean
  refLabel: string
  deltaUsable: boolean
  delta: string
  position: string
  motion: string
  /** Number of landmarks in the payload, "0" when landmarks are switched off. */
  landmarkCount: string
}

// Per-axis motion tokens. A Record lookup (rather than a switch with a default)
// means a value the agent adds later renders as "?" instead of silently
// falling through to a wrong label - and, more importantly, never as
// `undefined` pasted into the HUD.
const MOTION_TOKENS: Record<keyof Motion3, Record<string, string>> = {
  x: { STATIONARY: '·', LEFT: 'L', RIGHT: 'R' },
  y: { STATIONARY: '·', FORWARD: 'F', BACKWARD: 'B' },
  z: { STATIONARY: '·', UP: 'U', DOWN: 'D' }
}

const HANDEDNESS_LABELS: Record<string, string> = {
  unknown: '—',
  left: '左',
  right: '右'
}

function motionToken(axis: keyof Motion3, value: string): string {
  return MOTION_TOKENS[axis][value] ?? '?'
}

/** Fixed decimals, or "-" for anything non-finite. Never emits "NaN". */
function num(value: number | undefined, digits: number): string {
  return typeof value === 'number' && Number.isFinite(value) ? value.toFixed(digits) : '—'
}

function vec3(v: [number, number, number] | undefined): string {
  if (!Array.isArray(v) || v.length < 3) return '—'
  return `X ${signed(v[0], 3)}  Y ${signed(v[1], 3)}  Z ${signed(v[2], 3)}`
}

/** Forces an explicit sign so a row of numbers stays column-aligned and a
 *  direction change is visible without reading the digits. */
function signed(value: number, digits: number): string {
  if (typeof value !== 'number' || !Number.isFinite(value)) return '—'
  return (value >= 0 ? '+' : '') + value.toFixed(digits)
}

export const EMPTY_HAND_HUD: HandHud = {
  present: false,
  detected: false,
  title: 'NO DATA',
  titleActive: false,
  gesture: '—',
  gestureConfidence: '—',
  confidence: '—',
  handedness: '—',
  candidate: '',
  refLatched: false,
  refLabel: '—',
  deltaUsable: false,
  delta: '—',
  position: '—',
  motion: '—',
  landmarkCount: '0'
}

/**
 * Turn one hand block into display strings.
 *
 * The one piece of real protocol logic here: `delta` is reported as unusable
 * while reference_initialized is false. The agent does not latch the reference
 * until the hand has been stably visible for a few frames, and until then the
 * delta is a difference against a zero origin - i.e. it equals the absolute
 * position and reads as a huge phantom movement. The wire format says so
 * explicitly ("consumers MUST NOT consume delta before this flag"), so the HUD
 * must not show it either; showing it and hoping the operator remembers is how
 * a phantom jump gets mistaken for a tracking failure.
 */
export function buildHandHud(h: HandState): HandHud {
  const gesture = h.gesture ?? 'UNKNOWN'
  const candidate = h.gesture_candidate ?? 'UNKNOWN'
  const handedness = h.handedness ?? 'unknown'
  const refLatched = h.reference_initialized === true

  return {
    present: true,
    detected: h.detected === true,
    title: h.detected === true ? gesture : 'NO HAND',
    titleActive: h.detected === true,
    gesture,
    gestureConfidence: num(h.gesture_confidence, 2),
    confidence: num(h.confidence, 2),
    handedness: HANDEDNESS_LABELS[handedness] ?? handedness,
    // Only worth screen space while the debounce is still settling.
    candidate: candidate !== gesture ? candidate : '',
    refLatched,
    refLabel: refLatched ? 'REF LOCKED' : 'REF LATCHING',
    deltaUsable: refLatched,
    delta: refLatched ? vec3(h.delta) : '—',
    position: vec3(h.position),
    motion:
      `X ${motionToken('x', h.motion?.x ?? '')}  ` +
      `Y ${motionToken('y', h.motion?.y ?? '')}  ` +
      `Z ${motionToken('z', h.motion?.z ?? '')}`,
    landmarkCount: String(h.landmarks?.length ?? 0)
  }
}
