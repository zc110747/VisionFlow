// Single-config HLS latency probe (robust polling design).
// Measures the hls.js "frontier gap" = newest fragment end - video.currentTime.
// Requires a running video-server + pushed stream (see scripts/verify_e2e.py).
//   cfg A = hls.js defaults (liveSyncDurationCount 3)  - pre-tuning baseline
//   cfg B = sync2 + catchup 1.5x
//   cfg C = sync1 + catchup 2x   <- matches web/src/webrtc/player.ts
// Measured @1s GOP segments: A ~2.4s, B ~1.4s, C ~0.5-1.0s; @4s GOP: A ~10s, C ~2.4s.
// Usage: node scripts/hls_latency_probe.js A|B|C [stream] [httpPort]
// Node needs NODE_PATH to the managed playwright workspace, e.g.:
//   NODE_PATH="C:/Users/lx176/.workbuddy/binaries/node/workspace/node_modules"
const path = require('path')
const { chromium } = require('playwright')

const cfg = process.argv[2] || 'A'
const STREAM = process.argv[3] || 'camera01'
const HTTP = Number(process.argv[4] || 18081)
const HLS_JS = path.resolve(__dirname, '..', 'web', 'node_modules', 'hls.js', 'dist', 'hls.min.js')
const SAMPLE_SEC = 20

const OPTS = {
  A: { label: 'A-current(app defaults, liveSync 3)', liveSyncDurationCount: 3 },
  B: { label: 'B-tuned(sync2, catchup1.5x)', liveSyncDurationCount: 2, liveMaxLatencyDurationCount: 8, maxLiveSyncPlaybackRate: 1.5, maxBufferLength: 8 },
  C: { label: 'C-aggressive(sync1, catchup2x)', liveSyncDurationCount: 1, liveMaxLatencyDurationCount: 6, maxLiveSyncPlaybackRate: 2, maxBufferLength: 6 },
}[cfg]
if (!OPTS) { console.error('bad cfg', cfg); process.exit(2) }

function hlsOpts() {
  return {
    enableWorker: true,
    lowLatencyMode: false,
    fragLoadingMaxRetry: 6,
    manifestLoadingMaxRetry: 6,
    levelLoadingMaxRetry: 6,
    ...OPTS,
  }
}

async function ev(page, fn, args, ms = 8000) {
  return Promise.race([
    page.evaluate(fn, args),
    new Promise((_, rej) => setTimeout(() => rej(new Error('evaluate timeout')), ms)),
  ])
}

;(async () => {
  console.log('CFG', cfg, OPTS.label, 'stream', STREAM)
  const browser = await chromium.launch()
  const page = await browser.newPage()
  page.on('pageerror', (e) => console.log('[pageerror]', e.message))
  await page.goto(`http://127.0.0.1:${HTTP}/`, { waitUntil: 'domcontentloaded', timeout: 15000 })
  console.log('page loaded')
  await page.addScriptTag({ path: HLS_JS })
  await ev(page, () => { if (!window.Hls) throw new Error('Hls missing') })
  console.log('hls.js injected')

  await ev(page, (a) => {
    const video = document.createElement('video')
    video.muted = true
    video.playsInline = true
    video.style.cssText = 'position:fixed;width:320px;height:240px;left:-999px;top:-999px'
    document.body.appendChild(video)
    window.__probeVideo = video
    window.__probeErrs = []
    window.__probeHls = new window.Hls(a.opts)
    window.__probeHls.on(window.Hls.Events.ERROR, (_e, d) => {
      if (d.fatal) window.__probeErrs.push(d.type + '/' + d.details)
    })
    window.__probeHls.loadSource('/hls/' + a.stream + '/index.m3u8')
    window.__probeHls.attachMedia(video)
    video.play().catch(() => {})
  }, { opts: hlsOpts(), stream: STREAM })
  console.log('hls created & loading')

  const readState = () => {
    const v = window.__probeVideo
    const h = window.__probeHls
    let edge = NaN
    try {
      const lv = h.levels[h.currentLevel]
      const det = lv && lv.details
      if (det && det.fragments && det.fragments.length) {
        const last = det.fragments[det.fragments.length - 1]
        edge = last.start + last.duration
      }
    } catch { /* ignore */ }
    return { ct: v ? v.currentTime : -1, vw: v ? v.videoWidth : 0, vh: v ? v.videoHeight : 0, edge, errs: window.__probeErrs.slice() }
  }

  // wait for first real frame
  let t0 = Date.now()
  let st = await ev(page, readState)
  while ((st.vw <= 16 || st.ct <= 0.5) && Date.now() - t0 < 25000) {
    await new Promise((r) => setTimeout(r, 500))
    st = await ev(page, readState)
  }
  console.log('first frame', JSON.stringify({ vw: st.vw, ct: +st.ct.toFixed(2) }), 'after', Math.round((Date.now() - t0) / 1000), 's')

  const samples = []
  let stallSecs = 0
  let lastCt = -1
  const tEnd = Date.now() + SAMPLE_SEC * 1000
  while (Date.now() < tEnd) {
    const s = await ev(page, readState)
    const lat = s.edge - s.ct
    if (!Number.isNaN(lat)) samples.push(lat)
    if (lastCt >= 0 && Math.abs(s.ct - lastCt) < 0.05) stallSecs += 1
    lastCt = s.ct
    await new Promise((r) => setTimeout(r, 1000))
  }
  const sorted = [...samples].sort((a, b) => a - b)
  const med = sorted.length ? sorted[Math.floor(sorted.length / 2)] : NaN
  const p90 = sorted.length ? sorted[Math.min(sorted.length - 1, Math.floor(sorted.length * 0.9))] : NaN
  const min = sorted.length ? sorted[0] : NaN
  console.log('RESULT', JSON.stringify({
    cfg, label: OPTS.label,
    samples: samples.length, medianLatSec: +med.toFixed(2), p90LatSec: +p90.toFixed(2),
    minLatSec: +min.toFixed(2), stallSecs, fatalErrors: st.errs,
  }))
  console.log('raw', samples.map((s) => +s.toFixed(1)).join(','))

  await browser.close()
  process.exit(0)
})().catch((e) => {
  console.error('FATAL', e && e.message ? e.message : e)
  process.exit(1)
})
