<template>
  <div class="hand-panel">
    <div class="hp-head">
      <span class="hp-label">HAND 3D</span>
      <span class="hp-title" :class="{ on: hud.titleActive }">{{ hud.title }}</span>
    </div>

    <div class="hp-rows">
      <div class="hp-row"><span class="k">gesture</span><span class="v">{{ hud.gesture }}</span></div>
      <div class="hp-row"><span class="k">g.conf</span><span class="v">{{ hud.gestureConfidence }}</span></div>
      <div class="hp-row"><span class="k">det.conf</span><span class="v">{{ hud.confidence }}</span></div>
      <div class="hp-row"><span class="k">handed</span><span class="v">{{ hud.handedness }}</span></div>
      <div class="hp-row">
        <span class="k">candidate</span>
        <span class="v">{{ hud.candidate || '—' }}</span>
      </div>
      <div class="hp-row">
        <span class="k">reference</span>
        <span class="v" :class="{ warn: !hud.refLatched }">{{ hud.refLabel }}</span>
      </div>
      <div class="hp-row"><span class="k">position</span><span class="v mono">{{ hud.position }}</span></div>
      <div class="hp-row">
        <span class="k">delta</span>
        <span class="v mono" :class="{ muted: !hud.deltaUsable }">{{ hud.delta }}</span>
      </div>
      <div class="hp-row"><span class="k">motion</span><span class="v mono">{{ hud.motion }}</span></div>
      <div class="hp-row"><span class="k">landmarks</span><span class="v">{{ hud.landmarkCount }}</span></div>
    </div>

    <!-- The two states that look like "the panel is broken" unless they are
         named explicitly: nothing received yet, and a delta that the protocol
         forbids consuming until the reference is latched. -->
    <div v-if="!hud.present" class="hp-hint">尚无手部数据（需 ai-hand 模式）</div>
    <div v-else-if="!hud.deltaUsable" class="hp-hint">reference 未锁定前 delta 不可用</div>
  </div>
</template>

<script setup lang="ts">
import { computed } from 'vue'
import type { HandState } from '@/api/client'
import { buildHandHud, EMPTY_HAND_HUD } from '@/ai/hand'

// Fed by VideoPlayer's hand-update event. Single poller in the app: this panel
// must never fetch the metadata endpoint itself, or the overlay and the panel
// could be showing two different frames while both look correct.
const props = defineProps<{ hand?: HandState | null }>()
const hud = computed(() => (props.hand ? buildHandHud(props.hand) : EMPTY_HAND_HUD))
</script>

<style scoped>
.hand-panel {
  padding: 10px 12px;
  border: 1px solid #2a2e36;
  border-radius: 8px;
  background: #1b1e24;
  color: #e6e6e6;
  font-size: 12px;
}

.hp-head {
  display: flex;
  align-items: baseline;
  justify-content: space-between;
  margin-bottom: 8px;
}

.hp-label {
  color: #8a8a8a;
  font-size: 11px;
  letter-spacing: 0.08em;
}

.hp-title {
  font-size: 14px;
  font-weight: 600;
  color: #9a9a9a;
}

.hp-title.on {
  color: #00dcc8;
}

.hp-rows {
  display: flex;
  flex-direction: column;
  gap: 3px;
}

.hp-row {
  display: flex;
  gap: 8px;
  align-items: baseline;
}

.hp-row .k {
  min-width: 72px;
  color: #8a8a8a;
}

.hp-row .v {
  color: #e6e6e6;
}

.hp-row .v.mono {
  font-family: ui-monospace, SFMono-Regular, Menlo, Consolas, monospace;
  font-size: 11px;
}

.hp-row .v.muted {
  color: #6f6f6f;
}

.hp-row .v.warn {
  color: #ffc400;
}

.hp-hint {
  margin-top: 8px;
  padding-top: 8px;
  border-top: 1px solid rgba(255, 255, 255, 0.08);
  color: #8a8a8a;
  font-size: 11px;
  line-height: 1.5;
}
</style>
