export type CameraStatus = 'online' | 'offline' | 'connecting' | 'error'

export interface Camera {
  id: string
  name: string
  stream_path: string
  device_ip?: string
  status: CameraStatus
  resolution?: string
  fps: number
  bitrate: number
  created_at: string
  updated_at: string
  last_seen?: string
  rtsp_url?: string
}

export interface Health {
  status: string
  database: string
  media_server: string
}

export interface StreamInfo {
  id: string
  name: string
  stream_path: string
  status: CameraStatus
  rtsp_url: string
  resolution?: string
  fps: number
  bitrate: number
  webrtc: {
    signaling: string
    path: string
  }
  /** Same-origin HLS playlist, proxied by the server. Reserved for a future
   *  cached-recording playback feature - NOT used by the live player (live
   *  viewing is WebRTC only; HLS adds seconds of latency). */
  hls_url?: string
  /** Direct MediaMTX HLS URL, for debugging / future playback with an external player. */
  hls_direct_url?: string
}

// ---- AI mode (web-driven model selection) -----------------------------------
//
// Four modes the UI can pick. The agent polls the server for the chosen mode
// and swaps its detector at runtime.
//   ai-off    -> keep the loaded model, UI just hides the overlay
//   ai-y      -> person detection only (yolo11n)
//   ai-y-pose -> person detection + 17 COCO keypoints (yolo11n-pose)
//   ai-hand   -> single-hand 21 landmarks + 3D motion + gesture (ai-hand)
//
// AI_MODES is the UI's whitelist, and it is also the guard in loadAIMode():
// a mode the running agent reports that is NOT in here is silently ignored
// (falls back to the default). So a mode added on the server but forgotten
// here shows up as "the button never activates", not as an error.
export type AIMode = 'ai-off' | 'ai-y' | 'ai-y-pose' | 'ai-hand'
export const AI_MODES: AIMode[] = ['ai-off', 'ai-y', 'ai-y-pose', 'ai-hand']
export const DEFAULT_AI_MODE: AIMode = 'ai-y'

// ---- AI metadata (boxes drawn on the video overlay) -------------------------

export interface MetadataObject {
  class: string
  confidence: number
  track_id: number
  /** [x1, y1, x2, y2] in ORIGINAL video pixels. */
  bbox: [number, number, number, number]
  /** [x, y, conf] per joint: 17 COCO joints for pose, 21 hand landmarks for
   *  ai-hand (where the hand block also carries z - see HandState.landmarks). */
  keypoints?: [number, number, number][]
}

// ---- ai-hand: 3D hand state ------------------------------------------------
//
// Mirrors video-server/internal/metadata/model.go. Field names are the wire
// names verbatim - the server stores and re-emits the agent's own bytes, so a
// rename here is a runtime `undefined`, not a compile error.

/** Per-axis motion classification, one enum per axis. */
export interface Motion3 {
  x: 'STATIONARY' | 'LEFT' | 'RIGHT'
  y: 'STATIONARY' | 'FORWARD' | 'BACKWARD'
  z: 'STATIONARY' | 'UP' | 'DOWN'
}

/** Gesture vocabulary of Phase 1. Anything unclear stays UNKNOWN rather than
 *  being guessed, so the UI must render an unknown gesture as a real state. */
export type HandGesture = 'UNKNOWN' | 'OPEN_PALM' | 'FIST'
export type Handedness = 'unknown' | 'left' | 'right'

/**
 * The "hand" block, present ONLY on frames the agent produced in ai-hand mode.
 *
 * Three axis systems meet here and must not be conflated:
 *
 *   position / reference / delta  LOGICAL space, normalised to the frame:
 *                                 X+ right, Y+ towards the camera, Z+ up.
 *                                 Y is a depth proxy from the hand's apparent
 *                                 size, not from landmark depth.
 *   landmarks                     21 x [x, y, z] in ORIGINAL VIDEO PIXELS.
 *                                 z is the model's WRIST-RELATIVE depth on the
 *                                 same scale as x/y - NOT centimetres.
 *
 * `delta` is meaningless until reference_initialized is true: the agent only
 * latches a reference once the hand has been stable for a few frames, and that
 * is what stops a phantom jump the moment a hand enters the frame. Consumers
 * MUST honour that flag - see buildHandHud() in ../ai/hand.ts.
 *
 * NOTE ON BOUNDS: `landmarks` are deliberately NOT clamped to the frame. The
 * agent's mapping (carmera-agent/src/ai/hand_detector.cpp) clamps the bbox but
 * not the points, so a hand at the frame edge legitimately reports points
 * outside [0,w]x[0,h]. The overlay draws them as-is and lets the canvas clip -
 * that is real information, not a rendering bug.
 */
export interface HandState {
  detected: boolean
  confidence: number
  handedness: Handedness
  reference_initialized: boolean
  position: [number, number, number]
  reference: [number, number, number]
  delta: [number, number, number]
  motion: Motion3
  gesture: HandGesture
  /** Pre-debounce gesture of the same frame. Differs from `gesture` while a
   *  pose is still being confirmed - showing both is what makes a flapping
   *  gesture visible as such instead of as a flickering label. */
  gesture_candidate: HandGesture
  gesture_confidence: number
  landmarks?: [number, number, number][]
}

export interface MetadataFrame {
  frame_id: number
  timestamp: number
  video_width: number
  video_height: number
  /** Mode the agent actually ran for this frame ("ai-y" / "ai-y-pose" /
   *  "ai-hand"). Empty for frames from agents that predate the field. The UI
   *  drops any frame whose mode does not match the selected mode. */
  ai_mode?: string
  object_count: number
  received_at: string
  objects: MetadataObject[]
  /** Present only on ai-hand frames; absent (not null) on detect/pose frames,
   *  which is why the type is optional rather than nullable. */
  hand?: HandState
}

export interface MetadataStatus {
  enable: boolean
  running: boolean
  fps: number
  model: string
  tracker: string
  last_frame_id: number
  last_timestamp: number
  processed: number
  wall_clock: number
  received_at: string
}

/** Everything the overlay needs for one camera in a single call. */
export interface MetadataSnapshot {
  camera_id: string
  frame?: MetadataFrame
  status?: MetadataStatus
}

const BASE = '/api'

async function getJSON<T>(path: string): Promise<T> {
  const res = await fetch(BASE + path)
  if (!res.ok) throw new Error(await res.text())
  return res.json() as Promise<T>
}

async function sendJSON<T>(path: string, method: string, body?: unknown): Promise<T> {
  const res = await fetch(BASE + path, {
    method,
    headers: { 'Content-Type': 'application/json' },
    body: body ? JSON.stringify(body) : undefined
  })
  if (!res.ok) throw new Error(await res.text())
  return res.json() as Promise<T>
}

export const api = {
  health: () => getJSON<Health>('/health'),
  listCameras: () => getJSON<Camera[]>('/cameras'),
  getCamera: (id: string) => getJSON<Camera>('/cameras/' + id),
  createCamera: (c: Partial<Camera>) => sendJSON<Camera>('/cameras', 'POST', c),
  updateCamera: (id: string, c: Partial<Camera>) => sendJSON<Camera>('/cameras/' + id, 'PUT', c),
  deleteCamera: (id: string) => sendJSON<void>('/cameras/' + id, 'DELETE'),
  cameraStatus: (id: string) => getJSON<{ id: string; status: CameraStatus }>('/cameras/' + id + '/status'),
  cameraStream: (id: string) => getJSON<StreamInfo>('/cameras/' + id + '/stream'),
  webrtcOffer: (id: string, sdp: string) =>
    sendJSON<{ type: string; sdp: string }>('/cameras/' + id + '/webrtc', 'POST', { sdp }),
  cameraMetadata: (id: string) => getJSON<MetadataSnapshot>('/cameras/' + id + '/metadata'),
  // AI mode: the UI sets the desired mode; the agent polls it and switches.
  getAIMode: (id: string) => getJSON<{ mode: AIMode }>('/cameras/' + id + '/aimode'),
  setAIMode: (id: string, mode: AIMode) =>
    sendJSON<{ mode: AIMode }>('/cameras/' + id + '/aimode', 'POST', { mode })
}
