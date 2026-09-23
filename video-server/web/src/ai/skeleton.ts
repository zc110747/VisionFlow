// Skeleton topologies for the AI overlay.
//
// Pure data + one lookup: no DOM, no Vue, no fetch. The headless probe
// (scripts/verify_hand_overlay.py) imports THIS file, so the pair counts it
// asserts are literally the ones the renderer walks.
//
// WHY THIS FILE EXISTS
// The overlay used to hardcode `if (keypoints.length === 17) drawSkeleton(...)`.
// When the hand mode arrived it emits 21 points, so every hand frame fell
// through that condition and the operator saw a bbox with no skeleton - a
// silent failure that only a real camera reveals. Keying the topology off the
// point COUNT is what makes a new keypoint model a data change here rather
// than an if-branch hunt at the call site.

/** COCO 17-joint body skeleton (YOLO11n-pose keypoint order). */
export const COCO_KEYPOINT_COUNT = 17
export const COCO_CONNECTIONS: readonly (readonly [number, number])[] = [
  [15, 13], [13, 11], [16, 14], [14, 12], [11, 12], [5, 11], [6, 12], [5, 6],
  [5, 7], [6, 8], [7, 9], [8, 10], [1, 2], [0, 1], [0, 2], [1, 3], [2, 4]
]

/** MediaPipe hand landmark count. */
export const HAND_KEYPOINT_COUNT = 21

/**
 * MediaPipe HAND_CONNECTIONS over the 21-point hand layout.
 *
 * Index layout (see carmera-agent/include/camera_agent/ai/keypoint.h - the
 * agent emits this exact order, and the overlay has no way to recover from a
 * mismatch, so the two lists must be changed together):
 *
 *   0            wrist
 *   1  2  3  4   thumb  (CMC, MCP, IP, TIP)
 *   5  6  7  8   index  (MCP, PIP, DIP, TIP)
 *   9 10 11 12   middle
 *  13 14 15 16   ring
 *  17 18 19 20   pinky
 *
 * 21 connections: 4 per finger (chained from the palm) + 1 closing the palm
 * side (wrist -> pinky MCP). The thumb chains from the WRIST, not from the
 * index MCP - drawing it from 5 produces a visible kink that looks like a
 * tracking error but is really a wrong topology.
 */
export const HAND_CONNECTIONS: readonly (readonly [number, number])[] = [
  // thumb: wrist -> CMC -> MCP -> IP -> TIP
  [0, 1], [1, 2], [2, 3], [3, 4],
  // index: wrist -> MCP -> PIP -> DIP -> TIP
  [0, 5], [5, 6], [6, 7], [7, 8],
  // middle: MCP branches off the index MCP (the knuckle line)
  [5, 9], [9, 10], [10, 11], [11, 12],
  // ring
  [9, 13], [13, 14], [14, 15], [15, 16],
  // pinky
  [13, 17], [17, 18], [18, 19], [19, 20],
  // palm base
  [0, 17]
]

/** Fingertip indices, for a slightly larger dot on the overlay. */
export const HAND_TIP_INDICES: readonly number[] = [4, 8, 12, 16, 20]
/** Wrist index - drawn last (on top) and highlighted. */
export const HAND_WRIST_INDEX = 0

/**
 * Pick the connection list for a keypoint array of `count` points.
 *
 * Returns null when no topology is known: an unknown arity must draw NOTHING
 * rather than guess. Guessing pairs into a shorter array silently produces
 * scattered lines across the frame, which reads as a tracking bug and sends
 * the investigation to the wrong layer.
 */
export function skeletonFor(count: number): readonly (readonly [number, number])[] | null {
  if (count === COCO_KEYPOINT_COUNT) return COCO_CONNECTIONS
  if (count === HAND_KEYPOINT_COUNT) return HAND_CONNECTIONS
  return null
}
