// Type-check the <script setup> block of every SFC.
//
// WHY THIS EXISTS
// `tsc` does not read .vue files and vite/esbuild only *transpile* them, so the
// whole of <script setup> is a blind spot: an undefined identifier there - e.g.
// a `computed()` still referencing an import that was just deleted - builds
// green and throws a ReferenceError at runtime, which in a browser shows up as
// a blank panel rather than an error message. This probe extracts each
// <script setup> body, declares the compiler macros, and runs tsc over the
// result, turning that blind spot into a normal compile error with a line
// number pointing back into the .vue file.
//
// Usage: node tools/check-sfc.mjs        (or: npm run check)
import fs from 'node:fs'
import path from 'node:path'
import { createRequire } from 'node:module'
import { execFileSync } from 'node:child_process'
import { fileURLToPath } from 'node:url'

const WEB = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..')
// Must be inside the frontend tree: module resolution walks up from the
// importing file, so a tsconfig parked outside never finds node_modules and
// every import collapses to `any` (which hides real errors behind cascades).
const TMP = path.join(WEB, '.sfc-check')
const OUT = path.join(TMP, 'out')

const require = createRequire(path.join(WEB, 'package.json'))
const sfc = require('@vue/compiler-sfc')

fs.rmSync(TMP, { recursive: true, force: true })
fs.mkdirSync(OUT, { recursive: true })

// Compiler macros are not globals; declare them so a bare script-setup body
// type-checks on its own.
const MACROS = [
  'declare function defineProps<T>(...a: any[]): T',
  'declare function defineEmits<T>(...a: any[]): T',
  'declare function defineExpose<T>(x?: T): void',
  'declare function withDefaults<T, D>(p: T, d?: D): T',
  'declare function defineOptions<T>(o?: T): void',
  'declare function defineSlots<T>(): T',
  'declare function defineModel<T>(...a: any[]): T'
]
const MACRO_SRC = MACROS.join('\n') + '\n'
const PREFIX_LINES = MACROS.length

function walk(dir, acc = []) {
  for (const e of fs.readdirSync(dir, { withFileTypes: true })) {
    const p = path.join(dir, e.name)
    if (e.isDirectory()) walk(p, acc)
    else if (e.name.endsWith('.vue')) acc.push(p)
  }
  return acc
}

const files = walk(path.join(WEB, 'src'))
const map = []
for (const f of files) {
  const src = fs.readFileSync(f, 'utf8')
  const { descriptor, errors } = sfc.parse(src, { filename: f })
  if (errors.length) {
    console.log(`SFC PARSE ERROR ${f}: ${errors.map((e) => e.message).join('; ')}`)
    continue
  }
  const content = descriptor.scriptSetup?.content ?? descriptor.script?.content
  if (!content) continue
  const name = path.relative(path.join(WEB, 'src'), f).replace(/\.vue$/, '').replace(/[\\/]/g, '__')
  const out = path.join(OUT, name + '.vue.ts')
  fs.writeFileSync(out, MACRO_SRC + content)
  map.push({ out: path.basename(out), src: path.relative(WEB, f) })
}

fs.writeFileSync(
  path.join(TMP, 'tsconfig.json'),
  JSON.stringify(
    {
      compilerOptions: {
        target: 'ESNext',
        module: 'ESNext',
        moduleResolution: 'Bundler',
        strict: true,
        jsx: 'preserve',
        lib: ['ESNext', 'DOM', 'DOM.Iterable'],
        skipLibCheck: true,
        noEmit: true,
        isolatedModules: true,
        esModuleInterop: true,
        baseUrl: WEB,
        paths: { '@/*': ['src/*'] }
      },
      // env.d.ts carries the `declare module '*.vue'` shim, so importing one
      // component from another resolves instead of erroring as unresolved.
      include: ['out/*.ts', '../src/env.d.ts']
    },
    null,
    2
  )
)

const tsc = path.join(WEB, 'node_modules', 'typescript', 'bin', 'tsc')
let out = ''
let code = 0
try {
  out = execFileSync(process.execPath, [tsc, '-p', path.join(TMP, 'tsconfig.json')], {
    encoding: 'utf8',
    stdio: 'pipe'
  })
} catch (e) {
  out = (e.stdout || '') + (e.stderr || '')
  code = e.status || 1
}

console.log(`checked ${map.length} SFC script block(s)`)
let findings = 0
for (const line of out.split('\n')) {
  if (!line.trim()) continue
  const m = line.match(/^(.*?)\((\d+),(\d+)\): (error|warning) (.*)$/)
  if (!m) {
    console.log(line)
    continue
  }
  // tsc counts the generated macro declarations as lines, so the reported
  // number is offset by exactly that prefix - map it back to the .vue file.
  const hit = map.find((x) => x.out === path.basename(m[1]))
  const where = hit ? `${hit.src}:${Number(m[2]) - PREFIX_LINES}` : `${m[1]}:${m[2]}`
  console.log(`${where}:${m[3]}: ${m[5]}`)
  findings++
}
console.log(findings === 0 ? 'SFC-CHECK: clean' : `SFC-CHECK: ${findings} finding(s)`)
process.exit(code === 0 ? 0 : 1)
