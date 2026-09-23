// SSR entry used by scripts/verify_hand_overlay.py.
//
// Loaded THROUGH Vite (server.ssrLoadModule) rather than bundled by hand, so
// the .vue files, the `@/` alias and the Vue instance are all resolved by the
// same resolver the real build uses. A hand-rolled bundler here would test a
// different module graph than the shipped one.
import { createSSRApp, h } from 'vue'
import { renderToString } from '@vue/server-renderer'
import HandHud from '@/components/HandHud.vue'
import HandPanel from '@/components/HandPanel.vue'
import type { HandState } from '@/api/client'

export interface SsrProbeResult {
  hud: string
  panel: string
}

/** Render both hand views for one hand block (or null for the empty state).
 *
 *  createSSRApp + h() rather than calling the component as a function: a
 *  function call runs outside the renderer, so any composable inside
 *  (computed, onMounted) throws "Cannot read properties of null" instead of
 *  rendering. */
export async function renderHandViews(hand: HandState | null): Promise<SsrProbeResult> {
  const hud = await renderToString(createSSRApp({ render: () => h(HandHud, { hand }) }))
  const panel = await renderToString(createSSRApp({ render: () => h(HandPanel, { hand }) }))
  return { hud, panel }
}
