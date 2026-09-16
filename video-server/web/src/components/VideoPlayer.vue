<template>
  <div class="player-wrap">
    <video ref="videoEl" autoplay playsinline muted></video>

    <!-- AI tracking-box overlay. Pointer-events disabled so the WebRTC /
         fullscreen controls above stay clickable. The canvas is drawn from the
         latest GET /api/cameras/{id}/metadata frame, scaled to the displayed
         video size. -->
    <canvas ref="canvasEl" class="box-overlay"></canvas>

    <!-- Autoplay can be refused on phones (low power / data saver). Without a
         visible affordance the symptom is just "a black rectangle". -->
    <button v-if="status.needsTap" class="tap-overlay" @click="player?.resume()">
      <span>TAP TO PLAY</span>
    </button>

    <div class="player-status">
      <span class="transport">{{ transportLabel }}</span>
      <span class="state">{{ stateText }}</span>
      <span v-if="aiOn && aiMode !== 'ai-off'" class="ai-badge">AI {{ objectCount }}</span>
    </div>

    <!-- AI mode selector. ai-off only hides the overlay (the agent keeps its
         model); ai-y / ai-y-pose POST to the server so the agent polls and
         swaps its detector. The UI flips immediately on click - we do NOT wait
         for the server round-trip. On switch we clear the overlay at once and
         only resume drawing once an incoming frame matches the new mode; any
         intermediate frame from the still-swapping model (stamped with the OLD
         mode) is dropped by pollMetadata() - never composited as a stale box. -->
    <div class="ai-mode-bar">
      <button
        v-for="m in aiModeOptions"
        :key="m.value"
        class="mode-btn"
        :class="{ active: aiMode === m.value }"
        @click="selectAIMode(m.value)"
      >
        {{ m.label }}
      </button>
    </div>

    <div class="player-tools">
      <button class="tool-btn" :class="{ active: status.transport === 'webrtc' }" @click="player?.useWebRTC()" title="Reconnect via WebRTC">
        WebRTC
      </button>
      <button class="tool-btn" @click="fullscreen" title="Fullscreen">
        <el-icon><FullScreen /></el-icon>
      </button>
    </div>

    <div v-if="status.detail" class="player-detail">{{ status.detail }}</div>
  </div>
</template>

<script setup lang="ts">
import { computed, onMounted, onUnmounted, reactive, ref } from 'vue'
import { FullScreen } from '@element-plus/icons-vue'
import { StreamPlayer, type PlayerStatus } from '@/webrtc/player'
import { api, AI_MODES, DEFAULT_AI_MODE, type AIMode, type MetadataSnapshot } from '@/api/client'

const props = defineProps<{ cameraId: string }>()

// ---- AI mode (web-driven model selection) -----------------------------------
//
// ai-off      -> hide the overlay only; the agent keeps its loaded model.
// ai-y        -> person detection (yolo11n).
// ai-y-pose   -> person detection + 17 COCO keypoints (yolo11n-pose).
const aiMode = ref<AIMode>(DEFAULT_AI_MODE)
const aiModeOptions = [
  { value: 'ai-off' as AIMode, label: 'AI 关' },
  { value: 'ai-y' as AIMode, label: '人检测' },
  { value: 'ai-y-pose' as AIMode, label: '姿态' }
]

// Flip the UI selection immediately - the overlay switches at once and any
// incoming metadata frame whose embedded ai_mode does not match the newly
// selected mode is dropped by pollMetadata(). We do NOT await the server
// round-trip: the agent may take ~1s to poll and swap its detector, but the
// WebRTC <video> keeps playing and stale boxes are cleared on click. The POST
// is fire-and-forget (async); only a console error is logged if it fails, so a
// transient network blip never blocks the UI or the running video stream.
function selectAIMode(m: AIMode) {
  if (m === aiMode.value) return
  closeOverlayNow()
  aiMode.value = m
  api
    .setAIMode(props.cameraId, m)
    .catch((e) => console.error('setAIMode failed', e))
}

// Clear the overlay canvas right now (called the moment a mode switch starts so
// stale boxes disappear before the agent has finished swapping its model).
function closeOverlayNow() {
  const canvas = canvasEl.value
  if (!canvas) return
  const ctx = canvas.getContext('2d')
  if (!ctx) return
  clearOverlay(canvas, ctx)
  aiOn.value = false
  objectCount.value = 0
}

async function loadAIMode() {
  try {
    const r = await api.getAIMode(props.cameraId)
    if (AI_MODES.includes(r.mode)) aiMode.value = r.mode
  } catch {
    // Metadata disabled / not ready - fall back to the default (ai-y).
  }
}
const videoEl = ref<HTMLVideoElement | null>(null)
const canvasEl = ref<HTMLCanvasElement | null>(null)
let player: StreamPlayer | null = null
let metaTimer: number | null = null

const status = reactive<PlayerStatus>({
  state: 'idle',
  transport: null,
  detail: '',
  needsTap: false
})

const aiOn = ref(false)
const objectCount = ref(0)

const stateText = computed(() => {
  switch (status.state) {
    case 'connected':
      return 'CONNECTED'
    case 'connecting':
      return 'CONNECTING'
    case 'reconnecting':
      return 'RECONNECTING'
    case 'disconnected':
      return 'DISCONNECTED'
    default:
      return 'IDLE'
  }
})

// Naming the transport in the UI makes a failure debuggable at a glance. Live
// viewing is WebRTC only - there is no HLS fallback, so a problem shows up as a
// WEBRTC error rather than silently dropping to a high-latency stream.
const transportLabel = computed(() => {
  if (status.transport === 'webrtc') return 'WEBRTC'
  return '—'
})

function fullscreen() {
  videoEl.value?.requestFullscreen?.()
}

// ---- AI metadata overlay ------------------------------------------------

// COCO 17-joint skeleton (0-based indices), used to connect pose keypoints.
// Order matches YOLO11n-pose output:
// 0 nose, 1 leye, 2 reye, 3 lear, 4 rear, 5 lshoulder, 6 rshoulder, 7 lelbow,
// 8 relbow, 9 lwrist, 10 rwrist, 11 lhip, 12 rhip, 13 lknee, 14 rknee,
// 15 lankle, 16 rankle.
const COCO_SKELETON: [number, number][] = [
  [15, 13], [13, 11], [16, 14], [14, 12], [11, 12], [5, 11], [6, 12], [5, 6],
  [5, 7], [6, 8], [7, 9], [8, 10], [1, 2], [0, 1], [0, 2], [1, 3], [2, 4]
]
// Keypoints below this confidence are drawn faintly / skipped for connections
// so a spurious joint does not draw a stray line across the frame.
const KP_MIN_CONF = 0.2

// Poll the metadata endpoint and repaint the box overlay. The metadata frame
// carries bbox in ORIGINAL video pixels; we scale by the video element's
// actual rendered size so the boxes track the displayed stream regardless of
// layout. Missing frame / 0 detected objects simply clears the canvas.
const META_INTERVAL_MS = 200

async function pollMetadata() {
  const canvas = canvasEl.value
  const video = videoEl.value
  if (!canvas || !video) return
  const ctx = canvas.getContext('2d')
  if (!ctx) return

  let snap: MetadataSnapshot | undefined
  try {
    snap = await api.cameraMetadata(props.cameraId)
  } catch {
    // Metadata disabled / not ready yet - keep the overlay empty.
    clearOverlay(canvas, ctx)
    aiOn.value = false
    objectCount.value = 0
    return
  }

  // Unified UI rule: only composite a metadata frame whose embedded ai_mode
  // equals the selected mode. Each frame is stamped by the agent with the mode
  // it ACTUALLY ran, so during a runtime detector swap (the agent keeps
  // emitting the previous model for ~1s) those transition frames carry the OLD
  // mode and are dropped here - never composited as a stale box/skeleton.
  // "ai-off" is unified by the same rule: no frame is ever stamped "ai-off", so
  // every frame mismatches and is discarded. This only touches the overlay
  // <canvas>; the WebRTC <video> keeps playing, so the video stream never
  // stutters or drops frames.
  const frameMode = snap.frame?.ai_mode
  if (!frameMode || frameMode !== aiMode.value) {
    clearOverlay(canvas, ctx)
    aiOn.value = false
    objectCount.value = 0
    return
  }

  const vw = video.videoWidth
  const vh = video.videoHeight
  if (!snap.frame || vw === 0 || vh === 0 || snap.frame.objects.length === 0) {
    clearOverlay(canvas, ctx)
    aiOn.value = !!snap.status?.running
    objectCount.value = snap.frame?.object_count ?? 0
    return
  }

  const cw = video.clientWidth
  const ch = video.clientHeight
  const dpr = window.devicePixelRatio || 1
  const bw = Math.max(1, Math.round(cw * dpr))
  const bh = Math.max(1, Math.round(ch * dpr))
  if (canvas.width !== bw || canvas.height !== bh) {
    canvas.width = bw
    canvas.height = bh
  }
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0)
  ctx.clearRect(0, 0, cw, ch)

  const sx = cw / vw
  const sy = ch / vh
  ctx.lineWidth = 2
  ctx.font = '12px ui-monospace, SFMono-Regular, Menlo, Consolas, monospace'
  ctx.textBaseline = 'alphabetic'

  for (const o of snap.frame.objects) {
    const [x1, y1, x2, y2] = o.bbox
    const rx = x1 * sx
    const ry = y1 * sy
    const rw = (x2 - x1) * sx
    const rh = (y2 - y1) * sy

    ctx.strokeStyle = 'rgba(0, 220, 200, 0.95)'
    ctx.lineWidth = 2
    ctx.strokeRect(rx, ry, rw, rh)

    const label = `${o.class} ${(o.confidence * 100).toFixed(0)}%`
    const padX = 4
    const lh = 16
    const tw = ctx.measureText(label).width
    const ly = ry - lh
    ctx.fillStyle = 'rgba(0, 0, 0, 0.55)'
    ctx.fillRect(rx, ly, tw + padX * 2, lh)
    ctx.fillStyle = '#e6f7f4'
    ctx.fillText(label, rx + padX, ly + lh - 4)

    if (o.track_id > 0) {
      const tid = '#' + o.track_id
      ctx.fillStyle = 'rgba(0, 0, 0, 0.45)'
      ctx.fillText(tid, rx + rw - ctx.measureText(tid).width - padX, ly + lh - 4)
    }

    // Pose model: draw the 17-joint skeleton on top of the box.
    if (o.keypoints && o.keypoints.length === 17) {
      drawSkeleton(ctx, o.keypoints, sx, sy)
    }
  }

  aiOn.value = !!snap.status?.running
  objectCount.value = snap.frame.objects.length
}

// Draw COCO keypoint dots + skeleton connections for one object. kps are
// [x, y, conf] in ORIGINAL video pixels; sx/sy scale them to the canvas.
function drawSkeleton(
  ctx: CanvasRenderingContext2D,
  kps: [number, number, number][],
  sx: number,
  sy: number
) {
  // Connections first (under the dots).
  ctx.strokeStyle = 'rgba(255, 196, 0, 0.9)'
  ctx.lineWidth = 2
  for (const [a, b] of COCO_SKELETON) {
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

function clearOverlay(canvas: HTMLCanvasElement, ctx: CanvasRenderingContext2D) {
  if (canvas.width === 0 || canvas.height === 0) return
  ctx.clearRect(0, 0, canvas.width, canvas.height)
}

onMounted(() => {
  if (!videoEl.value) return
  player = new StreamPlayer(videoEl.value, props.cameraId)
  player.onStatus = (s) => {
    Object.assign(status, s)
  }
  player.start()
  metaTimer = window.setInterval(pollMetadata, META_INTERVAL_MS)
  loadAIMode()
})

onUnmounted(() => {
  player?.stop()
  player = null
  if (metaTimer !== null) {
    window.clearInterval(metaTimer)
    metaTimer = null
  }
})
</script>

<style scoped>
.player-wrap {
  position: relative;
  width: 100%;
  background: #000;
  border-radius: 6px;
  overflow: hidden;
}

video {
  width: 100%;
  display: block;
  background: #000;
}

.box-overlay {
  position: absolute;
  inset: 0;
  width: 100%;
  height: 100%;
  pointer-events: none;
}

.player-status {
  position: absolute;
  top: 8px;
  left: 8px;
  display: flex;
  gap: 8px;
  padding: 3px 8px;
  border-radius: 4px;
  background: rgba(0, 0, 0, 0.55);
  color: #e6e6e6;
  font-size: 11px;
  letter-spacing: 0.06em;
}

.transport {
  opacity: 0.75;
}

.state {
  opacity: 0.95;
}

.ai-badge {
  opacity: 0.95;
  color: #00dcc8;
}

.ai-mode-bar {
  position: absolute;
  top: 34px;
  left: 8px;
  display: flex;
  gap: 4px;
}

.mode-btn {
  padding: 3px 9px;
  font-size: 11px;
  color: #cfcfcf;
  background: rgba(0, 0, 0, 0.55);
  border: 1px solid rgba(255, 255, 255, 0.14);
  border-radius: 4px;
  cursor: pointer;
}

.mode-btn.active {
  border-color: rgba(0, 220, 200, 0.7);
  color: #00dcc8;
  background: rgba(0, 220, 200, 0.12);
}

.mode-btn:disabled {
  opacity: 0.5;
  cursor: default;
}

.player-tools {
  position: absolute;
  top: 8px;
  right: 8px;
  display: flex;
  gap: 4px;
}

.tool-btn {
  padding: 3px 8px;
  font-size: 11px;
  color: #d8d8d8;
  background: rgba(0, 0, 0, 0.55);
  border: 1px solid rgba(255, 255, 255, 0.14);
  border-radius: 4px;
  cursor: pointer;
}

.tool-btn.active {
  border-color: rgba(255, 255, 255, 0.45);
  color: #fff;
}

.player-detail {
  position: absolute;
  bottom: 0;
  left: 0;
  right: 0;
  padding: 6px 10px;
  background: rgba(0, 0, 0, 0.55);
  color: #bdbdbd;
  font-size: 11px;
  line-height: 1.4;
}

.tap-overlay {
  position: absolute;
  inset: 0;
  display: flex;
  align-items: center;
  justify-content: center;
  background: rgba(0, 0, 0, 0.45);
  border: none;
  cursor: pointer;
}

.tap-overlay span {
  padding: 8px 16px;
  border: 1px solid rgba(255, 255, 255, 0.5);
  border-radius: 4px;
  color: #fff;
  font-size: 12px;
  letter-spacing: 0.08em;
}
</style>
