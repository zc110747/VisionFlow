<template>
  <div>
    <div class="breadcrumb">
      <router-link :to="{ name: 'home' }">← Cameras</router-link>
      <span class="cam-name">{{ camera?.name || id }}</span>
      <StatusBadge v-if="camera" :status="camera.status" />
    </div>

    <div v-if="error" class="error-box">{{ error }}</div>

    <div v-else class="detail-grid">
      <div>
        <VideoPlayer :cameraId="id" @hand-update="hand = $event" />
      </div>
      <div class="info-list">
        <div class="row">
          <span class="k">Resolution</span>
          <span>{{ resolution }}</span>
        </div>
        <div class="row"><span class="k">FPS</span><span>{{ fps }}</span></div>
        <div class="row"><span class="k">Bitrate</span><span>{{ bitrate }}</span></div>
        <div class="row">
          <span class="k">RTSP URL</span>
          <span class="rtsp">{{ camera?.rtsp_url || '—' }}</span>
        </div>
        <div class="row"><span class="k">Stream Path</span><span>{{ camera?.stream_path }}</span></div>
        <div class="row"><span class="k">Last Seen</span><span>{{ camera?.last_seen || '—' }}</span></div>

        <!-- Hand state, fed by the player's own metadata poll. The panel is
             always mounted (showing its empty state outside ai-hand mode) so
             the layout does not jump when a mode is switched. -->
        <HandPanel class="hand-block" :hand="hand" />
      </div>
    </div>
  </div>
</template>

<script setup lang="ts">
import { computed, onMounted, onUnmounted, ref } from 'vue'
import { api, type Camera, type HandState, type StreamInfo } from '@/api/client'
import StatusBadge from '@/components/StatusBadge.vue'
import VideoPlayer from '@/components/VideoPlayer.vue'
import HandPanel from '@/components/HandPanel.vue'

const props = defineProps<{ id: string }>()
const camera = ref<Camera | null>(null)
const stream = ref<StreamInfo | null>(null)
const error = ref('')
// Latest hand block pushed up by the player. Null whenever the selected mode
// is not ai-hand, or no matching frame has arrived yet - i.e. "no data" is the
// default state, not an error.
const hand = ref<HandState | null>(null)
let timer: number | null = null

const resolution = computed(
  () => stream.value?.resolution || camera.value?.resolution || '—'
)
const fps = computed(() => {
  const v = camera.value?.fps ?? stream.value?.fps ?? 0
  return v > 0 ? String(v) : '—'
})
const bitrate = computed(() => {
  const b = camera.value?.bitrate ?? stream.value?.bitrate ?? 0
  return b > 0 ? (b / 1000).toFixed(0) + ' Mbps' : '—'
})

async function load() {
  try {
    const [c, s] = await Promise.all([api.getCamera(props.id), api.cameraStream(props.id)])
    camera.value = c
    stream.value = s
    error.value = ''
  } catch (e) {
    error.value = 'Failed to load camera: ' + (e as Error).message
  }
}

onMounted(() => {
  load()
  timer = window.setInterval(load, 3000)
})
onUnmounted(() => {
  if (timer) window.clearInterval(timer)
})
</script>

<style scoped>
/* Sits below the camera rows inside the same card, so it needs its own
   separation - and the card's `row:last-child` rule no longer applies once
   this element follows the last row. */
.hand-block {
  margin-top: 12px;
}
</style>
