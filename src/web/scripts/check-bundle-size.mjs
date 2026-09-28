// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Fails `npm run build` when the initial JS payload is over budget (TODO.md
// G1.1: 250 KiB gzipped, overridable with FMGR_WEB_JS_BUDGET_KIB).
//
// "Initial JS" is everything `dist/index.html` pulls in before the first paint:
// the entry `<script>` plus the `<link rel="modulepreload">` files Vite emits
// for its static imports. Chunks behind `import()` are deliberately not
// counted — the TanStack Table/Virtual screens are expected to be lazy (G1.3).

import { readFile } from 'node:fs/promises';
import { join, resolve } from 'node:path';
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
