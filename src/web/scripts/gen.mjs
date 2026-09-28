// SPDX-License-Identifier: AGPL-3.0-or-later
//
// `npm run gen` — generate the API types into `src/gen/` (G-arch 4).
//
// G1.1 ships this as a stub. G1.2 replaces the body with `buf generate` over
// ../../proto using @bufbuild/buf + protoc-gen-es from npm (no system
// install). It exists now so that `build`, `test` and `typecheck` can call it
// through their `pre*` hooks from day one, and so the generated directory
// (`src/gen/`, gitignored) always exists before a build.

import { mkdir } from 'node:fs/promises';

const generatedDir = new URL('../src/gen/', import.meta.url);

await mkdir(generatedDir, { recursive: true });

console.log(
  'gen: stub — proto codegen (@bufbuild/buf + protoc-gen-es) lands with G1.2; nothing generated.',
);
