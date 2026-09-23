// Node side of scripts/verify_hand_overlay.py.
//
// Loads the FRONTEND'S OWN modules (not a re-implementation) and drives them
// with payloads fetched from the live server, so what is under test is the
// shipped code and the real wire shape.
//
//   * overlay geometry  -> esbuild bundles src/ai/{hand,skeleton,draw}.ts and
//     runs the drawing functions against a RECORDING 2D context, asserting the
//     exact coordinates passed to the canvas.
//   * component markup  -> Vite's ssrLoadModule renders HandHud.vue /
//     HandPanel.vue through @vue/server-renderer, asserting no `undefined` /
//     `NaN` reaches the screen and that every live value is actually printed.
//
// Usage: node hand_overlay_probe.mjs <input.json> <output.json>
import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'
import { createRequire } from 'node:module'
import { fileURLToPath, pathToFileURL } from 'node:url'

const [inputPath, outputPath] = process.argv.slice(2)
const input = JSON.parse(fs.readFileSync(inputPath, 'utf8'))
const WEB = input.webRoot
const require = createRequire(path.join(WEB, 'package.json'))

const checks = []
const notes = []
/** Record one assertion. `detail` is what the operator needs to debug a FAIL. */
function check(name, ok, detail = '') {
  checks.push({ name, ok: !!ok, detail: String(detail) })
}
/** Record something that could not be asserted for lack of a sample. Kept
 *  separate from `check` so a missing sample can never masquerade as a pass. */
function note(name, detail = '') {
  notes.push({ name, detail: String(detail) })
}

const RED_FLAGS = ['undefined', 'NaN', '[object Object]', 'Infinity']

// ---------------------------------------------------------------------------
// A 2D context that records instead of drawing.
// ---------------------------------------------------------------------------
function makeCtx() {
  const calls = []
  return {
    calls,
    ctx: {
      strokeStyle: '',
      fillStyle: '',
      lineWidth: 0,
      font: '',
      textBaseline: '',
      beginPath: () => calls.push(['beginPath']),
      moveTo: (x, y) => calls.push(['moveTo', x, y]),
      lineTo: (x, y) => calls.push(['lineTo', x, y]),
      arc: (x, y, r) => calls.push(['arc', x, y, r]),
      stroke: () => calls.push(['stroke']),
      fill: () => calls.push(['fill']),
      strokeRect: (...a) => calls.push(['strokeRect', ...a]),
      fillRect: (...a) => calls.push(['fillRect', ...a]),
      fillText: (...a) => calls.push(['fillText', ...a]),
      clearRect: () => calls.push(['clearRect']),
      setTransform: () => calls.push(['setTransform']),
      measureText: (t) => ({ width: String(t).length * 6 })
    }
  }
}

// ---------------------------------------------------------------------------
// 1. Bundle the app's own modules
// ---------------------------------------------------------------------------
async function loadAppModules() {
  const esbuild = require('esbuild')
  const src = path.join(WEB, 'src')
  const entry = [
    `export { AI_MODES } from ${JSON.stringify(path.join(src, 'api', 'client.ts'))}`,
    `export * from ${JSON.stringify(path.join(src, 'ai', 'hand.ts'))}`,
    `export * from ${JSON.stringify(path.join(src, 'ai', 'skeleton.ts'))}`,
    `export * from ${JSON.stringify(path.join(src, 'ai', 'draw.ts'))}`
  ].join('\n')

  const outfile = path.join(os.tmpdir(), `vf-hand-probe-${process.pid}.cjs`)
  await esbuild.build({
    stdin: {
      contents: entry,
      resolveDir: WEB,
      sourcefile: 'hand-probe-entry.ts',
      loader: 'ts'
    },
    bundle: true,
    format: 'cjs',
    platform: 'node',
    write: true,
    outfile,
    logLevel: 'silent'
  })
  // Written to a file and require()d: a data: URL blows up with the whole
  // base64 payload in the exception when anything goes wrong.
  return require(outfile)
}

// ---------------------------------------------------------------------------
// 2. Pure/geometry checks
// ---------------------------------------------------------------------------
function checkTopology(m) {
  check(
    'skeletonFor(17) resolves the COCO body topology',
    m.skeletonFor(17)?.length === 17,
    `pairs=${m.skeletonFor(17)?.length}`
  )
  check(
    'skeletonFor(21) resolves the hand topology (21 bones)',
    m.skeletonFor(21)?.length === 21,
    `pairs=${m.skeletonFor(21)?.length}`
  )
  // The whole reason this module exists: an arity with no topology must draw
  // NOTHING rather than guess pairs into a shorter array (which paints stray
  // lines across the frame and gets blamed on the tracker).
  check(
    'skeletonFor(unknown arity) returns null instead of guessing',
    m.skeletonFor(5) === null && m.skeletonFor(0) === null,
    `5->${m.skeletonFor(5)} 0->${m.skeletonFor(0)}`
  )
  const bad = m.HAND_CONNECTIONS.filter(
    ([a, b]) => !(a >= 0 && a < 21 && b >= 0 && b < 21) || a === b
  )
  check(
    'every hand bone indexes a distinct landmark in [0,20]',
    bad.length === 0,
    bad.length ? JSON.stringify(bad) : 'all 21 bones valid'
  )
  const used = new Set(m.HAND_CONNECTIONS.flat())
  check(
    'the hand topology covers landmarks 0..20 (wrist and fingertips included)',
    used.size === 21,
    `distinct landmarks referenced=${used.size}`
  )
  check(
    'HAND_KEYPOINT_COUNT is the 21 the agent emits',
    m.HAND_KEYPOINT_COUNT === 21,
    String(m.HAND_KEYPOINT_COUNT)
  )
  check(
    "the UI's mode whitelist accepts ai-hand",
    m.AI_MODES.includes('ai-hand'),
    m.AI_MODES.join(',')
  )
}

function checkDrawing(m, hands) {
  const sx = 1280 / 1280
  const sy = 720 / 720

  // --- hand landmarks, on real recorded data ---
  const withLm = hands.filter((h) => h.landmarks && h.landmarks.length === 21)
  check(
    'recorded hand frames carry all 21 landmarks',
    withLm.length > 0,
    `${withLm.length} frame(s) with 21 landmarks`
  )

  if (withLm.length > 0) {
    const lm = withLm[0].landmarks
    const { ctx, calls } = makeCtx()
    m.drawHandLandmarks(ctx, lm, sx, sy)

    const strokes = calls.filter((c) => c[0] === 'stroke').length
    check('drawing a hand emits exactly 21 bones', strokes === 21, `strokes=${strokes}`)

    const arcs = calls.filter((c) => c[0] === 'arc')
    check(
      'drawing a hand emits 21 joints + 5 fingertips + 1 wrist',
      arcs.length === 27,
      `arcs=${arcs.length}`
    )

    // Every drawn arc must sit at landmark * scale, EXACTLY. This is the
    // assertion that pins "no clamping": if the overlay ever pulled an
    // out-of-frame point back inside, the coordinates would stop matching.
    const drawn = arcs.map((a) => [a[1], a[2]])
    const missing = lm
      .map((k, i) => i)
      .filter((i) => !drawn.some(([x, y]) => x === lm[i][0] * sx && y === lm[i][1] * sy))
    check(
      'every landmark is drawn at its exact (unclamped) scaled position',
      missing.length === 0,
      missing.length ? `landmarks not drawn 1:1: ${missing.join(',')}` : 'all 21 present'
    )
  }

  // --- no clamping, explicitly, with a deliberately out-of-frame point ---
  {
    const lm = Array.from({ length: 21 }, (_, i) => [100 + i, 200 + i, 0])
    lm[8] = [640, 4000, 0] // way below the frame
    const { ctx, calls } = makeCtx()
    m.drawHandLandmarks(ctx, lm, 1, 1)
    const hit = calls.some((c) => c[0] === 'arc' && c[2] === 4000)
    check(
      'an out-of-frame landmark is passed through, not clamped to the canvas',
      hit,
      hit ? 'y=4000 reached the canvas (clipped there, as intended)' : 'coordinate was altered'
    )
  }

  // --- negative controls ---
  {
    const { ctx, calls } = makeCtx()
    m.drawHandLandmarks(ctx, Array.from({ length: 20 }, () => [10, 10, 0]), 1, 1)
    m.drawHandLandmarks(ctx, undefined, 1, 1)
    check(
      'a 20-point or missing landmark array draws nothing',
      calls.length === 0,
      `calls=${calls.length}`
    )
  }

  // --- body pose still works through the same code path ---
  {
    const kps = Array.from({ length: 17 }, (_, i) => [10 * i, 20 * i, 0.9])
    const { ctx, calls } = makeCtx()
    m.drawSkeleton(ctx, kps, 1, 1)
    check(
      'the COCO body skeleton still draws its 17 bones and 17 joints',
      calls.filter((c) => c[0] === 'stroke').length === 17 &&
        calls.filter((c) => c[0] === 'arc').length === 17,
      `strokes=${calls.filter((c) => c[0] === 'stroke').length} arcs=${calls.filter((c) => c[0] === 'arc').length}`
    )
  }
  {
    const { ctx, calls } = makeCtx()
    m.drawSkeleton(ctx, Array.from({ length: 17 }, (_, i) => [i, i, 0.1]), 1, 1)
    check(
      'low-confidence pose joints are skipped rather than drawn as noise',
      calls.length === 0,
      `calls=${calls.length}`
    )
  }
}

function checkHud(m, hands, emptyHud) {
  // HandHud mixes display strings with the booleans that drive styling, so
  // "everything is a string" would be the wrong assertion. Check each group
  // for its own type, and check the key SET matches so a field that is added
  // to the interface but never populated cannot slip through.
  const allKeys = Object.keys(emptyHud).sort()
  const STRING_FIELDS = allKeys.filter((k) => typeof emptyHud[k] === 'string')
  const BOOL_FIELDS = allKeys.filter((k) => typeof emptyHud[k] === 'boolean')
  check(
    'the HUD shape splits into display strings and style booleans',
    STRING_FIELDS.length === 11 && BOOL_FIELDS.length === 5,
    `strings=${STRING_FIELDS.length} booleans=${BOOL_FIELDS.length}`
  )

  const badType = []
  const badText = []
  const badKeys = []
  for (const h of hands) {
    const hud = m.buildHandHud(h)
    const keys = Object.keys(hud).sort()
    if (keys.join(',') !== allKeys.join(',')) badKeys.push(keys.join(','))
    for (const k of STRING_FIELDS) {
      if (typeof hud[k] !== 'string') badType.push(`${k}=${typeof hud[k]}`)
      else if (RED_FLAGS.some((f) => hud[k].includes(f))) badText.push(`${k}="${hud[k]}"`)
    }
    for (const k of BOOL_FIELDS) {
      if (typeof hud[k] !== 'boolean') badType.push(`${k}=${typeof hud[k]}`)
    }
  }
  check(
    'every HUD display field is a string for every recorded hand frame',
    badType.length === 0,
    badType.length ? badType.slice(0, 5).join(', ') : `${hands.length} frame(s), strings=${STRING_FIELDS.length}`
  )
  check(
    'no HUD field renders "undefined" / "NaN" / "[object Object]"',
    badText.length === 0,
    badText.length ? badText.slice(0, 5).join(', ') : 'clean'
  )
  check(
    'buildHandHud returns exactly the fields the interface declares',
    badKeys.length === 0,
    badKeys.length ? `unexpected key set on ${badKeys.length} frame(s)` : `${allKeys.length} fields`
  )

  // The protocol forbids consuming delta before the reference is latched, so
  // the HUD must not show it either - a phantom jump would otherwise read as a
  // tracking failure.
  const preLatch = m.buildHandHud({
    detected: true,
    confidence: 0.9,
    handedness: 'right',
    reference_initialized: false,
    position: [0.5, 0.4, 0.6],
    reference: [0, 0, 0],
    delta: [0.5, 0.4, 0.6],
    motion: { x: 'STATIONARY', y: 'STATIONARY', z: 'STATIONARY' },
    gesture: 'OPEN_PALM',
    gesture_candidate: 'FIST',
    gesture_confidence: 0.8,
    landmarks: []
  })
  check(
    'delta is withheld and flagged while the reference is not latched',
    preLatch.deltaUsable === false &&
      preLatch.delta === '—' &&
      preLatch.refLabel === 'REF LATCHING',
    `deltaUsable=${preLatch.deltaUsable} delta="${preLatch.delta}" ref="${preLatch.refLabel}"`
  )
  check(
    'a differing pre-debounce gesture is surfaced as the candidate',
    preLatch.candidate === 'FIST' && preLatch.gesture === 'OPEN_PALM',
    `gesture=${preLatch.gesture} candidate=${preLatch.candidate}`
  )

  const latched = m.buildHandHud({
    detected: true,
    confidence: 1,
    handedness: 'left',
    reference_initialized: true,
    position: [0.5, 0.4, 0.6],
    reference: [0.5, 0.4, 0.6],
    delta: [0.012, -0.5, 0.75],
    motion: { x: 'LEFT', y: 'FORWARD', z: 'UP' },
    gesture: 'FIST',
    gesture_candidate: 'FIST',
    gesture_confidence: 0.99,
    landmarks: []
  })
  check(
    'a latched reference exposes signed delta and per-axis motion',
    latched.deltaUsable === true &&
      /^X [+-]\d\.\d{3}\s+Y [+-]\d\.\d{3}\s+Z [+-]\d\.\d{3}$/.test(latched.delta) &&
      latched.motion === 'X L  Y F  Z U' &&
      latched.candidate === '',
    `delta="${latched.delta}" motion="${latched.motion}"`
  )

  const noHand = m.buildHandHud({
    detected: false,
    confidence: 0,
    handedness: 'unknown',
    reference_initialized: false,
    position: [0, 0, 0],
    reference: [0, 0, 0],
    delta: [0, 0, 0],
    motion: { x: 'STATIONARY', y: 'STATIONARY', z: 'STATIONARY' },
    gesture: 'UNKNOWN',
    gesture_candidate: 'UNKNOWN',
    gesture_confidence: 0,
    landmarks: []
  })
  check(
    'an empty frame is labelled NO HAND rather than left blank',
    noHand.title === 'NO HAND' && noHand.titleActive === false,
    `title="${noHand.title}"`
  )

  // A motion token the agent adds later must not paste `undefined` into the HUD.
  const future = m.buildHandHud({
    detected: true,
    confidence: 1,
    handedness: 'right',
    reference_initialized: true,
    position: [0, 0, 0],
    reference: [0, 0, 0],
    delta: [0, 0, 0],
    motion: { x: 'SIDEWAYS', y: 'STATIONARY', z: 'STATIONARY' },
    gesture: 'OPEN_PALM',
    gesture_candidate: 'OPEN_PALM',
    gesture_confidence: 1,
    landmarks: []
  })
  check(
    'an unrecognised motion token degrades to "?" instead of undefined',
    future.motion.startsWith('X ?'),
    `motion="${future.motion}"`
  )
}

// ---------------------------------------------------------------------------
// 3. SSR: render the two hand views through Vite
// ---------------------------------------------------------------------------
// Vite 5 ships both a CJS and an ESM node entry. resolve() hands back whichever
// matches the *caller's* format, and `await import()` of the CJS entry does not
// give usable named exports (createServer comes back undefined). Unwrap both
// shapes rather than assuming one.
async function loadVite() {
  try {
    const cjs = require('vite')
    if (cjs && typeof cjs.createServer === 'function') return cjs
  } catch {
    // fall through to the ESM entry
  }
  const mod = await import(pathToFileURL(require.resolve('vite')).href)
  if (mod && typeof mod.createServer === 'function') return mod
  if (mod && mod.default && typeof mod.default.createServer === 'function') return mod.default
  throw new Error(`cannot obtain createServer from vite (keys: ${Object.keys(mod).join(',')})`)
}

async function checkSsr(liveHand, sampleHands) {
  const vite = await loadVite()
  const server = await vite.createServer({
    root: WEB,
    configFile: path.join(WEB, 'vite.config.ts'),
    appType: 'custom',
    logLevel: 'error',
    server: { middlewareMode: true }
  })
  try {
    const entry = await server.ssrLoadModule('/tools/ssr-probe-entry.ts')

    const withHand = await entry.renderHandViews(liveHand)
    const empty = await entry.renderHandViews(null)

    for (const [name, html] of [
      ['HUD (live hand)', withHand.hud],
      ['panel (live hand)', withHand.panel],
      ['HUD (empty state)', empty.hud],
      ['panel (empty state)', empty.panel]
    ]) {
      const flags = RED_FLAGS.filter((f) => html.includes(f))
      check(`SSR ${name} renders without undefined/NaN`, flags.length === 0 && html.length > 0,
        flags.length ? `flags: ${flags.join(',')}` : `${html.length} bytes`)
    }

    check(
      'SSR of the empty state still prints the explicit no-data label',
      empty.hud.includes('NO DATA') && empty.panel.includes('NO DATA'),
      empty.hud.includes('NO DATA') ? 'present' : 'MISSING'
    )

    // Positive samples. "No red flags" alone would also pass on a component
    // that renders nothing at all, so every assertion below anchors on a
    // string taken from the payload being rendered.
    if (liveHand) {
      check(
        'SSR panel prints the live gesture verbatim',
        withHand.panel.includes(liveHand.gesture),
        liveHand.gesture
      )
      check(
        'SSR prints the live confidence value',
        withHand.hud.includes(Number(liveHand.confidence).toFixed(2)) &&
          withHand.panel.includes(Number(liveHand.confidence).toFixed(2)),
        Number(liveHand.confidence).toFixed(2)
      )
      // The HUD headline IS the gesture when a hand is in frame, and an
      // explicit NO HAND when it is not. Those are different strings, so the
      // assertion follows the state instead of assuming one of them.
      const expectedTitle = liveHand.detected ? liveHand.gesture : 'NO HAND'
      check(
        `SSR HUD headline reflects the live detection state (${expectedTitle})`,
        withHand.hud.includes(expectedTitle),
        expectedTitle
      )
    }

    // A frame with a REAL gesture proves the headline is bound to the payload
    // rather than to a constant. Prefer a non-UNKNOWN gesture: 'UNKNOWN' would
    // pass by accident, since it is also what an unbound field renders.
    const named = sampleHands.find((h) => h.detected && h.gesture && h.gesture !== 'UNKNOWN')
    if (named) {
      const r = await entry.renderHandViews(named)
      check(
        `SSR prints a named gesture (${named.gesture}) as the HUD headline`,
        r.hud.includes(named.gesture) && r.panel.includes(named.gesture),
        named.gesture
      )
      check(
        'SSR of a detected frame prints the landmark count',
        r.panel.includes(String((named.landmarks || []).length)),
        String((named.landmarks || []).length)
      )
      check(
        'SSR shows the reference as latched for a frame that had it latched',
        named.reference_initialized
          ? r.hud.includes('REF LOCKED')
          : r.hud.includes('REF LATCHING'),
        `reference_initialized=${named.reference_initialized}`
      )
    } else {
      note('SSR named-gesture sample', 'no recorded frame had a non-UNKNOWN gesture')
    }

    // And the opposite state, from a frame that genuinely had no hand.
    const missed = sampleHands.find((h) => !h.detected) ||
      (liveHand && !liveHand.detected ? liveHand : null)
    if (missed) {
      const r = await entry.renderHandViews(missed)
      check(
        'SSR of a hand-less frame shows NO HAND rather than a stale gesture',
        r.hud.includes('NO HAND'),
        'NO HAND'
      )
    } else {
      note('SSR hand-less sample', 'no recorded frame without a hand was available')
    }
  } finally {
    await server.close()
  }
}

// ---------------------------------------------------------------------------
async function main() {
  const m = await loadAppModules()
  checkTopology(m)

  const hands = [input.liveHand, ...input.archiveHands].filter(Boolean)
  checkDrawing(m, hands)
  checkHud(m, hands, m.EMPTY_HAND_HUD)
  await checkSsr(input.liveHand, input.archiveHands)

  fs.writeFileSync(outputPath, JSON.stringify({ checks, notes }, null, 2))
  console.log(`hand_overlay_probe: ${checks.length} check(s) emitted`)
}

main().catch((e) => {
  fs.writeFileSync(
    outputPath,
    JSON.stringify({ checks, notes, fatal: String(e && e.stack ? e.stack : e) }, null, 2)
  )
  process.exit(1)
})
