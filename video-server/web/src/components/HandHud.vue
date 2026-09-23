<template>
  <div class="hand-hud">
    <div class="hh-title" :class="{ active: hud.titleActive }">{{ hud.title }}</div>
    <div class="hh-row">
      <span class="k">conf</span><span class="v">{{ hud.confidence }}</span>
      <span class="k">hand</span><span class="v">{{ hud.handedness }}</span>
    </div>
    <div v-if="hud.candidate" class="hh-row">
      <span class="k">cand</span><span class="v">{{ hud.candidate }}</span>
    </div>
    <div class="hh-row">
      <span class="k">ref</span>
      <span class="v" :class="{ warn: !hud.refLatched }">{{ hud.refLabel }}</span>
    </div>
    <div class="hh-row">
      <span class="k">Δ</span>
      <span class="v mono" :class="{ muted: !hud.deltaUsable }">{{ hud.delta }}</span>
    </div>
    <div class="hh-row">
      <span class="k">motion</span><span class="v mono">{{ hud.motion }}</span>
    </div>
    <div class="hh-row">
      <span class="k">pts</span><span class="v">{{ hud.landmarkCount }}</span>
    </div>
  </div>
</template>

<script setup lang="ts">
import { computed } from 'vue'
import type { HandState } from '@/api/client'
import { buildHandHud, EMPTY_HAND_HUD } from '@/ai/hand'

// Overlay HUD for the video area. An HTML layer rather than canvas text: no
// device-pixel-ratio / measureText dance, stays crisp when the video is scaled,
// and can be rendered headlessly in Node (scripts/verify_hand_overlay.py
// asserts every field it prints exists on the live payload).
//
// Separate from HandPanel.vue on purpose: the HUD is the glanceable subset
// (shown over the video), the panel is the full readout (side column). Both
// derive from the same buildHandHud(), so they cannot disagree.
const props = defineProps<{ hand?: HandState | null }>()
const hud = computed(() => (props.hand ? buildHandHud(props.hand) : EMPTY_HAND_HUD))
</script>

<style scoped>
.hand-hud {
  position: absolute;
  bottom: 10px;
  left: 8px;
  min-width: 150px;
  padding: 6px 8px;
  border-radius: 4px;
  background: rgba(0, 0, 0, 0.6);
  color: #e6e6e6;
  font-size: 11px;
  line-height: 1.5;
  letter-spacing: 0.04em;
  pointer-events: none;
}

.hh-title {
  font-size: 13px;
  font-weight: 600;
  color: #9a9a9a;
  margin-bottom: 3px;
}

.hh-title.active {
  color: #00dcc8;
}

.hh-row {
  display: flex;
  gap: 6px;
  align-items: baseline;
}

.hh-row .k {
  color: #8a8a8a;
  min-width: 34px;
}

.hh-row .v {
  color: #e6e6e6;
}

.hh-row .v.mono {
  font-family: ui-monospace, SFMono-Regular, Menlo, Consolas, monospace;
}

.hh-row .v.muted {
  color: #6f6f6f;
}

.hh-row .v.warn {
  color: #ffc400;
}
</style>
