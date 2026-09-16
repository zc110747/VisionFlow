<template>
  <span class="status-badge">
    <span class="status-dot" :style="{ background: color }"></span>
    <span>{{ label }}</span>
  </span>
</template>

<script setup lang="ts">
import { computed } from 'vue'
import type { CameraStatus } from '@/api/client'

const props = defineProps<{ status: CameraStatus }>()

const color = computed(() => {
  switch (props.status) {
    case 'online':
      return '#3fb950'
    case 'connecting':
      return '#d29922'
    case 'error':
      return '#f85149'
    default:
      return '#6e7681'
  }
})
const label = computed(() => props.status.toUpperCase())
</script>

<style scoped>
.status-badge {
  display: inline-flex;
  align-items: center;
  font-size: 12px;
  letter-spacing: 0.5px;
}
</style>
