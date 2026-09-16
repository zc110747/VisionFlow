<template>
  <div class="camera-card" @click="open">
    <div class="card-head">
      <span class="name">{{ camera.name }}</span>
      <StatusBadge :status="camera.status" />
    </div>
    <div class="video-ph">VIDEO</div>
    <div class="meta">
      <span>{{ resolution }}</span>
      <span>{{ fpsLabel }}</span>
    </div>
  </div>
</template>

<script setup lang="ts">
import { computed } from 'vue'
import { useRouter } from 'vue-router'
import type { Camera } from '@/api/client'
import StatusBadge from './StatusBadge.vue'

const props = defineProps<{ camera: Camera }>()
const router = useRouter()

const resolution = computed(() => props.camera.resolution || '—')
const fpsLabel = computed(() => (props.camera.fps > 0 ? props.camera.fps + ' FPS' : '—'))

function open() {
  router.push({ name: 'camera', params: { id: props.camera.id } })
}
</script>

<style scoped>
.card-head {
  display: flex;
  justify-content: space-between;
  align-items: center;
}
.card-head .name {
  font-weight: 600;
}
</style>
