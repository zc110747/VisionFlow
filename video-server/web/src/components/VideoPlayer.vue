<template>
  <div class="player-wrap">
    <video ref="videoEl" autoplay playsinline muted></video>

    <!-- AI overlay. Pointer-events disabled so the WebRTC / fullscreen controls
         above stay clickable. The canvas is drawn from the latest
         GET /api/cameras/{id}/metadata frame, scaled to the displayed video
         size. -->
    <canvas ref="canvasEl" class="box-overlay"></canvas>

    <!-- Hand HUD. Shown only in hand mode: in any other mode there is no hand
         block, so it would be a permanent "NO DATA" box. -->
    <HandHud v-if="aiMode === 'ai-hand'" :hand="handState" />

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
         model); ai-y / ai-y-pose / ai-hand POST to the server so the agent
         polls and swaps its detector. The UI flips immediately on click - we
         do NOT wait for the server round-trip. On switch we clear the overlay
         at once and only resume drawing once an incoming frame matches the new
         mode; any intermediate frame from the still-swapping model (stamped
         with the OLD mode) is dropped by pollMetadata() - never composited as
         a stale box.

         NOTE: ai-hand is a hard server-side prerequisite. The agent polls the
         selected mode and reverts to detect within ~1s unless the server
         accepts "ai-hand" (see ValidAIMode in internal/metadata/model.go). A
         UI button alone does not enable it. -->
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
import {
  api,
  AI_MODES,
  DEFAULT_AI_MODE,
  type AIMode,
  type HandState,
  type MetadataSnapshot
} from '@/api/client'
import { drawSkeleton, drawHandLandmarks } from '@/ai/draw'
import HandHud from '@/components/HandHud.vue'

const props = defineProps<{ cameraId: string }>()
// The parent (CameraView) owns the side panel, so the hand block is pushed up
// instead of being polled twice - two pollers would show two slightly
// different frames and any disagreement would look like a rendering bug.
const emit = defineEmits<{ (e: 'hand-update', hand: HandState | null): void }>()

// ---- AI mode (web-driven model selection) -----------------------------------
//
// ai-off      -> hide the overlay only; the agent keeps its loaded model.
// ai-y        -> person detection (yolo11n).
// ai-y-pose   -> person detection + 17 COCO keypoints (yolo11n-pose).
// ai-hand     -> single hand: 21 landmarks + 3D motion + gesture.
const aiMode = ref<AIMode>(DEFAULT_AI_MODE)
const aiModeOptions = [
  { value: 'ai-off' as AIMode, label: 'AI 关' },
  { value: 'ai-y' as AIMode, label: '人检测' },
  { value: 'ai-y-pose' as AIMode, label: '姿态' },
  { value: 'ai-hand' as AIMode, label: '手势' }
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
  resetOverlay()
  aiMode.value = m
  api
    .setAIMode(props.cameraId, m)
    .catch((e) => console.error('setAIMode failed', e))
}

async function loadAIMode() {
  try {
    const r = await api.getAIMode(props.cameraId)
    // AI_MODES is the whitelist: a mode this build does not know is ignored
    // (default stays). That is intentional - an unknown mode string would
    // otherwise leave no button highlighted and no frame ever matching.
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

// Latest hand block, or null outside ai-hand mode / when no frame matched yet.
// Held as state because the HUD is a Vue-rendered layer (HandHud.vue); the
// display strings are derived inside that component, not here.
const handState = ref<HandState | null>(null)

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

// Poll the metadata endpoint and repaint the overlay. The metadata frame
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
    resetOverlay()
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
  const frame = snap.frame
  const frameMode = frame?.ai_mode
  if (!frame || !frameMode || frameMode !== aiMode.value) {
    resetOverlay()
    return
  }

  const vw = video.videoWidth
  const vh = video.videoHeight
  if (vw === 0 || vh === 0) {
    resetOverlay()
    return
  }

  // A hand block only ever rides an ai-hand frame, and the gate above already
  // proved this IS one - so the presence of the block, not the mode string, is
  // what decides whether the hand path runs.
  const hand = frame.hand ?? null
  handState.value = hand
  emit('hand-update', hand)

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

  // On a hand frame the objects[] entry ("hand", 21 keypoints) and the hand
  // block carry the SAME landmarks, so the skeleton is drawn once, from the
  // hand block, and the object loop skips it. Drawing both would stack two
  // identical skeletons - invisible until the two ever disagree, at which
  // point it would look like a double-detection.
  const handFrame = hand !== null

  for (const o of frame.objects) {
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

    // Skeleton, when the model produced one this build has a topology for.
    if (!handFrame && o.keypoints) {
      drawSkeleton(ctx, o.keypoints, sx, sy)
    }
  }

  if (hand) drawHandLandmarks(ctx, hand.landmarks, sx, sy)

  aiOn.value = !!snap.status?.running
  objectCount.value = frame.objects.length
}

function clearOverlay(canvas: HTMLCanvasElement, ctx: CanvasRenderingContext2D) {
  if (canvas.width === 0 || canvas.height === 0) return
  ctx.clearRect(0, 0, canvas.width, canvas.height)
}

// Blank the overlay and drop every piece of derived state. Called on any path
// where the selected mode and the incoming frame disagree - including the mode
// switch itself, so stale boxes AND a stale hand block disappear together
// before the agent has finished swapping models.
function resetOverlay() {
  const canvas = canvasEl.value
  if (canvas) {
    const ctx = canvas.getContext('2d')
    if (ctx) clearOverlay(canvas, ctx)
  }
  aiOn.value = false
  objectCount.value = 0
  if (handState.value !== null) {
    handState.value = null
    emit('hand-update', null)
  }
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
