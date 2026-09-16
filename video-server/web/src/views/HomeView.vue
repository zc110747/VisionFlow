<template>
  <div>
    <div class="stat-row">
      <div class="stat-card">
        <div class="num">{{ cameras.length }}</div>
        <div class="label">Cameras</div>
      </div>
      <div class="stat-card">
        <div class="num" style="color: #3fb950">{{ onlineCount }}</div>
        <div class="label">Online</div>
      </div>
      <div class="stat-card">
        <div class="num" style="color: #6e7681">{{ offlineCount }}</div>
        <div class="label">Offline</div>
      </div>
      <div class="stat-card">
        <div class="num">{{ mediaStatus }}</div>
        <div class="label">Media Server</div>
      </div>
    </div>

    <div class="section-title">Cameras</div>
    <div v-if="error" class="error-box">{{ error }}</div>
    <div v-else class="camera-grid">
      <CameraCard v-for="c in cameras" :key="c.id" :camera="c" />
      <div v-if="cameras.length === 0" class="empty">
        No cameras yet. Push an RTSP stream to rtsp://server:8554/&lt;path&gt;.
      </div>
    </div>
  </div>
</template>

<script setup lang="ts">
import { computed, onMounted, onUnmounted, ref } from 'vue'
import { api, type Camera, type Health } from '@/api/client'
import CameraCard from '@/components/CameraCard.vue'

const cameras = ref<Camera[]>([])
const health = ref<Health | null>(null)
const error = ref('')
let timer: number | null = null

const onlineCount = computed(() => cameras.value.filter((c) => c.status === 'online').length)
const offlineCount = computed(() => cameras.value.filter((c) => c.status === 'offline').length)
const mediaStatus = computed(() => (health.value?.media_server === 'ok' ? 'OK' : 'ERR'))

async function load() {
  try {
    const [cs, h] = await Promise.all([api.listCameras(), api.health()])
    cameras.value = cs
    health.value = h
    error.value = ''
  } catch (e) {
    error.value = 'Failed to load: ' + (e as Error).message
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
