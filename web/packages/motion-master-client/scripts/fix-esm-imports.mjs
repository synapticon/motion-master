// Appends explicit `.js` extensions to relative import/export specifiers in the emitted ESM
// output (dist/). `tsc` with `moduleResolution: bundler` leaves them extensionless, which is fine
// for bundlers (Vite/Vitest consume the package's TS source directly) but Node's ESM loader
// rejects extensionless relative specifiers in the *published* artifact. This keeps the build
// dependency-free — Node built-ins only — so the package needs no bundler.
import { readdir, readFile, writeFile } from 'node:fs/promises'
import { join, extname } from 'node:path'
import { fileURLToPath } from 'node:url'

const DIST = fileURLToPath(new URL('../dist', import.meta.url))

// `from './x'`, side-effect `import './x'`, and dynamic `import('./x')`. Captures the keyword
// (+ optional open paren), the opening quote, the relative specifier, and the closing quote.
const RELATIVE_SPECIFIER = /(\bfrom\s*|\bimport\s*\(?\s*)(['"])(\.\.?\/[^'"]+)(['"])/g

async function* walk(dir) {
  for (const entry of await readdir(dir, { withFileTypes: true })) {
    const path = join(dir, entry.name)
    if (entry.isDirectory()) {
      yield* walk(path)
    } else if (entry.name.endsWith('.js') || entry.name.endsWith('.d.ts')) {
      yield path
    }
  }
}

let patched = 0
for await (const file of walk(DIST)) {
  const src = await readFile(file, 'utf8')
  const out = src.replace(RELATIVE_SPECIFIER, (match, keyword, open, spec, close) =>
    extname(spec) ? match : `${keyword}${open}${spec}.js${close}`,
  )
  if (out !== src) {
    await writeFile(file, out)
    patched += 1
  }
}
console.log(`fix-esm-imports: patched ${patched} file(s) in dist/`)
