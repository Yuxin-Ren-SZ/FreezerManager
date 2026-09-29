// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Two guards on the shape of the build, both run by `npm run build`.
//
// 1. **The budget.** Fails when the initial JS payload is over 250 KiB gzipped
//    (TODO.md G1.1, overridable with FMGR_WEB_JS_BUDGET_KIB). "Initial JS" is
//    everything `dist/index.html` pulls in before the first paint: the entry
//    `<script>` plus the `<link rel="modulepreload">` files Vite emits for its
//    static imports. Chunks behind `import()` are deliberately not counted.
//
// 2. **The chunking.** Fails when a feature screen is reachable from the entry
//    chunk's static imports, i.e. when it is not behind `import()` in
//    `src/app/route-map.tsx` (issue #64). See the section at the bottom.
//
// They belong together because the budget alone is too blunt to be a regression
// test: it only complains once the entry has already grown past the ceiling, so
// a screen imported statically today is reported weeks later as "this screen is
// too big" rather than as "the entry is carrying every screen".
//
// Guard 2 reads Vite's build manifest, which needs `build.manifest` in
// `vite.config.ts`. It exits 1 when the manifest is missing rather than passing
// by finding nothing.

import { readFile, readdir } from 'node:fs/promises';
import { join, resolve, sep } from 'node:path';
import { gzipSync } from 'node:zlib';

const BUDGET_KIB = Number.parseInt(process.env.FMGR_WEB_JS_BUDGET_KIB ?? '250', 10);
const distDir = resolve(process.cwd(), 'dist');

const html = await readFile(join(distDir, 'index.html'), 'utf8');

const assets = new Set();
for (const match of html.matchAll(/<(?:script|link)\b[^>]*?\b(?:src|href)="([^"]+\.js)"/g)) {
  const url = match[1];
  if (url === undefined) {
    continue;
  }
  assets.add(url.startsWith('/') ? url.slice(1) : url);
}

if (assets.size === 0) {
  console.error(
    'check-bundle-size: no initial JS found in dist/index.html — did `vite build` run?',
  );
  process.exit(1);
}

let totalBytes = 0;
for (const asset of [...assets].sort()) {
  const raw = await readFile(join(distDir, asset));
  const gzipped = gzipSync(raw).length;
  totalBytes += gzipped;
  console.log(
    `  ${asset}  ${(raw.length / 1024).toFixed(1)} KiB raw  ${(gzipped / 1024).toFixed(1)} KiB gzip`,
  );
}

const totalKiB = totalBytes / 1024;
console.log(
  `check-bundle-size: initial JS ${totalKiB.toFixed(1)} KiB gzipped, budget ${BUDGET_KIB} KiB`,
);

if (totalKiB > BUDGET_KIB) {
  console.error(
    `check-bundle-size: over budget by ${(totalKiB - BUDGET_KIB).toFixed(1)} KiB — ` +
      'split the new code behind import(), or raise the budget in the issue first.',
  );
  process.exit(1);
}

// --- 2. the chunking guard -------------------------------------------------
//
// The property: every screen under `src/features/<name>/<Name>Screen.tsx` is
// reached through `import()`, so it gets its own chunk and the entry chunk
// carries only the shell (issue #64). G-arch 11 puts one screen per feature
// directory, which is why the glob is exactly one level deep.
//
// `import()`ed modules appear in the build manifest as dynamic entries; a
// screen imported statically does not appear in the manifest at all, and a
// screen that is imported both ways appears in the entry's *static* closure.
// Both are failures below.
//
// The limit worth knowing: the manifest maps entries to chunks, not every
// module to the chunk it landed in, so this proves each screen has a dynamic
// entry of its own and is not statically reachable. It cannot prove no copy of
// a screen also sits in the entry chunk; the budget above is what pays for that
// case, and together the two cover it.

const FEATURES_DIR = join(process.cwd(), 'src', 'features');

const screenFiles = (await readdir(FEATURES_DIR, { recursive: true }))
  .map((path) => path.split(sep).join('/'))
  .filter((path) => /^[^/]+\/[^/]+Screen\.tsx$/.test(path))
  .map((path) => `src/features/${path}`)
  .sort();

if (screenFiles.length === 0) {
  console.error(
    'check-bundle-size: no src/features/*/*Screen.tsx found — the chunking guard would ' +
      'pass by finding nothing, which is the failure mode it exists to avoid.',
  );
  process.exit(1);
}

let manifest;
try {
  manifest = JSON.parse(await readFile(join(distDir, '.vite', 'manifest.json'), 'utf8'));
} catch (error) {
  console.error(
    'check-bundle-size: cannot read dist/.vite/manifest.json, which the chunking guard ' +
      `needs (${error.message}). \`vite.config.ts\` sets build.manifest, so a run of ` +
      '`npm run build` produces it.',
  );
  process.exit(1);
}

const entryKey = Object.keys(manifest).find((key) => manifest[key].isEntry === true);
if (entryKey === undefined) {
  console.error('check-bundle-size: dist/.vite/manifest.json has no entry chunk.');
  process.exit(1);
}

/** The chunks the entry reaches over one manifest edge type, transitively. */
function reachable(edge) {
  const seen = new Set([entryKey]);
  const visit = (key) => {
    for (const next of manifest[key]?.[edge] ?? []) {
      if (seen.has(next)) {
        continue;
      }
      seen.add(next);
      visit(next);
    }
  };
  visit(entryKey);
  return seen;
}

const staticChunks = reachable('imports');

const dynamicChunks = new Set();
for (const key of staticChunks) {
  for (const next of manifest[key]?.dynamicImports ?? []) {
    dynamicChunks.add(next);
  }
}

const problems = [];
for (const screen of screenFiles) {
  if (staticChunks.has(screen)) {
    problems.push(`${screen} is in the entry chunk's static imports`);
  } else if (!dynamicChunks.has(screen)) {
    problems.push(`${screen} is not reached by import() from the entry chunk`);
  }
}

const jsChunks = (await readdir(join(distDir, 'assets'))).filter((name) => name.endsWith('.js'));
console.log(
  `check-bundle-size: ${jsChunks.length} JS chunks for ${screenFiles.length} feature screens, ` +
    `${assets.size} chunk(s) before first paint`,
);

if (problems.length > 0) {
  console.error(
    'check-bundle-size: the entry chunk is carrying feature screens (issue #64):\n' +
      problems.map((problem) => `  - ${problem}`).join('\n') +
      '\n  Route a screen with `lazyScreen(() => import(…), …)` in src/app/route-map.tsx ' +
      'rather than importing it at the top of the file: the shell stays eager, the screen ' +
      'is a screen the user chose to open.',
  );
  process.exit(1);
}

// --- 3. the heavy-kit guard ------------------------------------------------
//
// `src/ui/index.ts` is imported by the shell, so it is in the entry chunk, and
// a re-export from it is *not* tree-shakeable when the component imports CSS.
// `Table.tsx` does, so re-exporting `Table` from the barrel put TanStack Table
// and TanStack Virtual in the entry chunk even while every screen using them
// was lazy: 33.5 KiB gzipped on the G3.2 tree (issue #64). The barrel no longer
// exports it, and screens import `../../ui/Table` directly.
//
// The markers below are string literals inside the two packages (`fnName`
// values and virtualizer option names), not minified identifiers, so they
// survive minification. A marker that is in no chunk at all means nothing
// imports the table on this tree yet — that is reported rather than silently
// counting as a pass, and it is only ever true before the first table screen
// lands.

const HEAVY_MARKERS = new Map([
  ['TanStack Table', 'getCoreRowModel'],
  ['TanStack Virtual', 'getVirtualItems'],
]);

const initialFiles = new Set(assets);
const chunkText = new Map();
for (const name of jsChunks) {
  chunkText.set(`assets/${name}`, await readFile(join(distDir, 'assets', name), 'utf8'));
}

const heavyProblems = [];
for (const [name, marker] of HEAVY_MARKERS) {
  const carriers = [...chunkText].filter(([, text]) => text.includes(marker)).map(([file]) => file);
  if (carriers.length === 0) {
    console.log(`check-bundle-size: ${name} is not in this build — no screen imports it yet`);
    continue;
  }
  const eager = carriers.filter((file) => initialFiles.has(file));
  if (eager.length > 0) {
    heavyProblems.push(`${name} is in the initial JS: ${eager.join(', ')}`);
  } else {
    console.log(`check-bundle-size: ${name} is behind import() only: ${carriers.join(', ')}`);
  }
}

if (heavyProblems.length > 0) {
  console.error(
    'check-bundle-size: the entry chunk is carrying a heavy kit dependency (issue #64):\n' +
      heavyProblems.map((problem) => `  - ${problem}`).join('\n') +
      '\n  Import it from its own module (`../../ui/Table`), not from the `../../ui` barrel: ' +
      'the barrel is in the entry chunk, and a re-export of a component that imports CSS ' +
      'cannot be tree-shaken away.',
  );
  process.exit(1);
}
