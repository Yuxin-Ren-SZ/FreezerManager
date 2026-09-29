// SPDX-License-Identifier: AGPL-3.0-or-later
//
// `npm run check:routes` — the route checker (TODO.md G1.2).
//
// The REST gateway is written in C++ (`src/rest/RestGateway.cc`, one
// `FMGR_ROUTE(...)` line per unary RPC) and the SPA calls it through
// `src/api/routes.ts`. Nothing keeps those two lists in step at compile time —
// the C++ side has no idea the SPA exists, and the SPA only has generated
// message types, not routes. So a C++ PR can add an endpoint and the browser
// simply cannot reach it, or the SPA can call a path that was renamed, and both
// sides still build and test green.
//
// This script fails, with a non-zero exit code, when:
//
//   * the gateway serves a path that `routes.ts` does not have,
//   * `routes.ts` has a path the gateway does not serve,
//   * a route's RPC method or service does not match the C++ line,
//   * a route key is not `<noun>/<verb>` derived from its own path,
//   * a streaming `…/watch` handler is missing from `src/api/sse.ts`.
//
// It also fails when it cannot parse one of the two sides: a checker that
// silently finds no routes would pass forever, which is the same class of bug
// as the always-exit-0 `npm run gen` stub G1.1 shipped.

import { readFileSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const webDir = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const repoDir = resolve(webDir, '..', '..');
const gatewayPath = join(repoDir, 'src', 'rest', 'RestGateway.cc');
const routesPath = join(webDir, 'src', 'api', 'routes.ts');
const ssePath = join(webDir, 'src', 'api', 'sse.ts');

/** `stub` token in the C++ macro -> the proto service it belongs to. */
const STUB_SERVICE = {
  audit: 'AuditService',
  auth: 'AuthService',
  box: 'BoxService',
  item_type: 'ItemTypeService',
  lab: 'LabService',
  role: 'RoleService',
  sample: 'SampleService',
  session: 'SessionService',
  share: 'ShareService',
};

const problems = [];

const read = (path) => {
  try {
    return readFileSync(path, 'utf8');
  } catch (error) {
    problems.push(`cannot read ${path}: ${error.message}`);
    return null;
  }
};

const gatewaySource = read(gatewayPath);
const routesSource = read(routesPath);
const sseSource = read(ssePath);

if (gatewaySource === null || routesSource === null || sseSource === null) {
  report();
}

// ---------------------------------------------------------------- C++ side --

const ROUTE_RE = /FMGR_ROUTE\(\s*"([^"]+)"\s*,\s*(\w+)\s*,\s*(\w+)\s*,\s*(\w+)\s*,\s*(\w+)\s*\)/g;

/** @type {Map<string, {stub: string, method: string, request: string, response: string}>} */
const gateway = new Map();
for (const match of gatewaySource.matchAll(ROUTE_RE)) {
  const [, path, stub, method] = match;
  if (gateway.has(path)) {
    problems.push(`${gatewayPath}: duplicate FMGR_ROUTE for ${path}`);
  }
  gateway.set(path, { stub, method });
}

// The macro is defined once and each route invokes it once, so every other
// occurrence must have been parsed. This is the "did the parser break?" guard.
const macroOccurrences = (gatewaySource.match(/FMGR_ROUTE\(/g) ?? []).length;
const macroDefinition = /\n#define\s+FMGR_ROUTE\(/.test(gatewaySource) ? 1 : 0;
if (macroOccurrences - macroDefinition !== gateway.size) {
  problems.push(
    `${gatewayPath}: parsed ${gateway.size} of ${macroOccurrences - macroDefinition} FMGR_ROUTE ` +
      `occurrences — the macro shape changed and this checker no longer understands it`,
  );
}
if (gateway.size === 0) {
  problems.push(`${gatewayPath}: no FMGR_ROUTE lines found at all`);
}

// `client.ts` hardcodes POST for every unary route, which is only correct while
// the macro registers POST. Fail loudly if that ever changes.
if (!/\{drogon::Post\}\)/.test(gatewaySource)) {
  problems.push(
    `${gatewayPath}: the FMGR_ROUTE macro no longer registers {drogon::Post} — ` +
      `src/api/client.ts assumes every unary route is POST`,
  );
}

// ------------------------------------------------------------ routes.ts side --

/** Brace-aware scan of `export const apiRoutes = { … }` into top-level entries. */
function parseRouteEntries(source) {
  const start = source.indexOf('export const apiRoutes = {');
  if (start < 0) {
    problems.push(`${routesPath}: could not find \`export const apiRoutes = {\``);
    return [];
  }

  const open = source.indexOf('{', start);
  let depth = 0;
  let end = -1;
  for (let index = open; index < source.length; index += 1) {
    const char = source[index];
    if (char === '{') depth += 1;
    else if (char === '}') {
      depth -= 1;
      if (depth === 0) {
        end = index;
        break;
      }
    }
  }
  if (end < 0) {
    problems.push(`${routesPath}: unbalanced braces in the apiRoutes table`);
    return [];
  }

  const body = source.slice(open + 1, end);
  const entries = [];
  let entryDepth = 0;
  let current = '';
  const flush = () => {
    if (current.trim().length > 0) entries.push(current);
    current = '';
  };
  for (const char of body) {
    if (char === '{') entryDepth += 1;
    else if (char === '}') entryDepth -= 1;
    if (char === ',' && entryDepth === 0) {
      flush();
      continue;
    }
    current += char;
  }
  flush();
  return entries;
}

/** @type {Map<string, {key: string, path: string, rpc: string}>} */
const web = new Map();
for (const entry of parseRouteEntries(routesSource)) {
  // `m` matters: an entry carries its `// ---- Service ----` comment line above it.
  const key = /^\s*'([^']+)'\s*:/m.exec(entry)?.[1];
  const path = /path:\s*'([^']+)'/.exec(entry)?.[1];
  const rpc = /rpc:\s*'([^']+)'/.exec(entry)?.[1];
  if (path === undefined || rpc === undefined) {
    problems.push(`${routesPath}: entry without a path/rpc pair:\n    ${entry.trim()}`);
    continue;
  }
  if (key === undefined) {
    problems.push(`${routesPath}: entry for ${path} has no route key`);
    continue;
  }
  if (web.has(path)) {
    problems.push(`${routesPath}: duplicate entry for ${path}`);
  }
  web.set(path, { key, path, rpc });
}

const pathLiterals = (routesSource.match(/path:\s*'/g) ?? []).length;
if (pathLiterals !== web.size) {
  problems.push(
    `${routesPath}: parsed ${web.size} of ${pathLiterals} \`path:\` literals — the table shape ` +
      `changed and this checker no longer understands it`,
  );
}
if (web.size === 0) {
  problems.push(`${routesPath}: the apiRoutes table is empty`);
}

// ------------------------------------------------------------- cross-check --

for (const [path, route] of gateway) {
  if (!web.has(path)) {
    problems.push(
      `missing from routes.ts: the gateway serves ${path} ` +
        `(${STUB_SERVICE[route.stub] ?? route.stub}/${route.method})`,
    );
  }
}

for (const [path, route] of web) {
  const cpp = gateway.get(path);
  if (cpp === undefined) {
    problems.push(`missing from RestGateway.cc: routes.ts has ${path} (${route.rpc})`);
    continue;
  }

  const service = STUB_SERVICE[cpp.stub];
  if (service === undefined) {
    problems.push(
      `${gatewayPath}: unknown stub token '${cpp.stub}' for ${path} — ` +
        `add it to STUB_SERVICE in scripts/check-routes.mjs`,
    );
  } else if (route.rpc !== `fmgr.v1.${service}/${cpp.method}`) {
    problems.push(
      `rpc mismatch for ${path}: routes.ts says ${route.rpc}, ` +
        `RestGateway.cc registers ${service}/${cpp.method}`,
    );
  }

  const expectedKey = path.replace(/^\/api\/v1\//, '');
  if (route.key !== expectedKey) {
    problems.push(
      `route key mismatch for ${path}: key is '${route.key}', expected '${expectedKey}' ` +
        `(the /api/v1/ prefix stripped)`,
    );
  }
}

// --------------------------------------------------------------- SSE routes --

// The gateway bridges the server-streaming feeds with `registerHandler` rather
// than FMGR_ROUTE, so they need their own (much smaller) check: every path the
// gateway streams must be one the SPA subscribes to.
const streamed = new Set();
for (const match of gatewaySource.matchAll(/registerHandler\(\s*"([^"]+\/watch)"/g)) {
  streamed.add(match[1]);
}
const subscribed = new Set(
  [...sseSource.matchAll(/path:\s*'([^']+\/watch)'/g)].map((match) => match[1]),
);

for (const path of streamed) {
  if (!subscribed.has(path)) {
    problems.push(`missing from sse.ts: the gateway streams ${path}`);
  }
}
for (const path of subscribed) {
  if (!streamed.has(path)) {
    problems.push(`stale in sse.ts: ${path} is not streamed by RestGateway.cc`);
  }
}

report();

function report() {
  if (problems.length > 0) {
    console.error(`check-routes: ${problems.length} problem(s)\n`);
    for (const problem of problems) console.error(`  - ${problem}`);
    console.error(
      '\nThe REST gateway (src/rest/RestGateway.cc) and src/api/routes.ts must list the ' +
        'same routes, in both directions.',
    );
    process.exit(1);
  }
  console.log(
    `check-routes: ok — ${gateway.size} unary routes and ${streamed.size} SSE routes agree ` +
      `between RestGateway.cc, routes.ts and sse.ts`,
  );
  process.exit(0);
}
