// SPDX-License-Identifier: AGPL-3.0-or-later
//
// `npm run gen` — generate the API types into `src/gen/` (TODO.md G1.2,
// G-arch 4).
//
// This used to be a stub that only created an empty `src/gen/`. That is the
// dangerous shape: `build`, `test` and `typecheck` all pass with no generated
// types at all, and the problem only shows up in a feature task much later. So
// this script does three things:
//
//   1. runs the real `buf generate` over `../../proto`,
//   2. *verifies* afterwards that every `.proto` produced its `_pb.ts` output,
//      and fails otherwise — a buf exit code of 0 is not on its own proof that
//      anything was generated,
//   3. short-circuits when the inputs and the previous output are unchanged, so
//      the `predev`/`prebuild`/`pretest`/`pretypecheck` hooks stay cheap.
//
// Everything comes from npm: `@bufbuild/buf` (the CLI) and
// `@bufbuild/protoc-gen-es` (the plugin) are devDependencies, resolved from
// `node_modules/.bin`. No system protoc, no system plugin.
//
// `FMGR_PROTO_DIR` overrides the proto tree (used by the failure-path check);
// it defaults to the repository's `proto/`.

import { spawnSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import {
  existsSync,
  mkdirSync,
  readFileSync,
  readdirSync,
  rmSync,
  statSync,
  writeFileSync,
} from 'node:fs';
import { dirname, join, relative, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const webDir = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const protoDir = resolve(process.env.FMGR_PROTO_DIR ?? join(webDir, '..', '..', 'proto'));
const generatedDir = join(webDir, 'src', 'gen');
const templatePath = join(webDir, 'buf.gen.yaml');
const stampPath = join(webDir, 'node_modules', '.cache', 'buf-gen.stamp');

/** Every `.proto` under `dir`, as paths relative to it. */
function listProtos(dir) {
  const found = [];
  const walk = (current) => {
    for (const entry of readdirSync(current, { withFileTypes: true }).sort((a, b) =>
      a.name.localeCompare(b.name),
    )) {
      const full = join(current, entry.name);
      if (entry.isDirectory()) {
        walk(full);
      } else if (entry.name.endsWith('.proto')) {
        found.push(relative(dir, full));
      }
    }
  };
  walk(dir);
  return found;
}

/** `fmgr/v1/sample.proto` -> `fmgr/v1/sample_pb.ts`, the protoc-gen-es layout. */
const generatedPathFor = (protoRelPath) =>
  join(generatedDir, protoRelPath.replace(/\.proto$/, '_pb.ts'));

const fail = (message) => {
  console.error(`gen: ${message}`);
  process.exit(1);
};

if (!existsSync(protoDir) || !statSync(protoDir).isDirectory()) {
  fail(`proto directory not found: ${protoDir} (set FMGR_PROTO_DIR to override)`);
}

const protos = listProtos(protoDir);
if (protos.length === 0) {
  fail(`no .proto files under ${protoDir}`);
}

const bufBin = join(
  webDir,
  'node_modules',
  '.bin',
  process.platform === 'win32' ? 'buf.cmd' : 'buf',
);
if (!existsSync(bufBin)) {
  fail(`buf CLI missing at ${bufBin} — run \`npm ci\` first`);
}

// `node_modules/.bin` must be on PATH for buf to find the `local: protoc-gen-es`
// plugin. npm puts it there for `npm run`, but not when this file is executed
// directly with `node`, so add it explicitly.
const pathKey = Object.keys(process.env).find((key) => key.toUpperCase() === 'PATH') ?? 'PATH';
const env = {
  ...process.env,
  [pathKey]: `${join(webDir, 'node_modules', '.bin')}:${process.env[pathKey] ?? ''}`,
  // buf defaults its cache to $HOME/.cache, which is not writable in every
  // sandbox or CI container. Keep the cache inside the project instead (it is
  // already gitignored via node_modules/) unless the caller chose a location.
  BUF_CACHE_DIR: process.env.BUF_CACHE_DIR ?? join(webDir, 'node_modules', '.cache', 'buf'),
};

const bufVersion = spawnSync(bufBin, ['--version'], { cwd: webDir, env, encoding: 'utf8' });
if (bufVersion.status !== 0) {
  fail(`\`buf --version\` failed:\n${bufVersion.stderr ?? ''}`);
}

const fingerprint = createHash('sha256');
fingerprint.update(bufVersion.stdout.trim());
fingerprint.update(readFileSync(templatePath));
for (const proto of protos) {
  fingerprint.update(proto);
  fingerprint.update(readFileSync(join(protoDir, proto)));
}
const stamp = fingerprint.digest('hex');

const outputsPresent = () =>
  protos.every((proto) => {
    const out = generatedPathFor(proto);
    return existsSync(out) && statSync(out).size > 0;
  });

if (outputsPresent() && existsSync(stampPath) && readFileSync(stampPath, 'utf8').trim() === stamp) {
  console.log(`gen: up to date (${protos.length} proto files, buf ${bufVersion.stdout.trim()})`);
  process.exit(0);
}

// `--clean` removes anything left over in src/gen first, so a deleted .proto
// cannot leave a stale module behind for a feature task to import by accident.
const result = spawnSync(bufBin, ['generate', protoDir, '--template', templatePath, '--clean'], {
  cwd: webDir,
  env,
  stdio: 'inherit',
});

if (result.error) {
  fail(`could not run buf: ${result.error.message}`);
}
if (result.status !== 0) {
  fail(`buf generate exited ${result.status ?? 'null'}`);
}

// buf exiting 0 is not proof that anything was written.
const missing = protos.filter((proto) => {
  const out = generatedPathFor(proto);
  return !existsSync(out) || statSync(out).size === 0;
});
if (missing.length > 0) {
  fail(
    `buf generate reported success but produced no output for:\n  ${missing.join('\n  ')}\n` +
      `expected under ${generatedDir}`,
  );
}

mkdirSync(dirname(stampPath), { recursive: true });
rmSync(stampPath, { force: true });
writeFileSync(stampPath, `${stamp}\n`);
console.log(`gen: generated ${protos.length} proto files into src/gen/`);
