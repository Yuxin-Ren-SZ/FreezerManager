# Web UI (`src/web/`)

The SPA that talks to the REST gateway (PRD §10). Scaffolded by TODO.md **G1.1**,
which also fixed the "G-arch" decisions in `TODO.md` §Section G that every G task
follows; **G1.2** added the data layer (`src/api/`, `src/test/`) and **G1.3** the
shell and the UI kit (`src/app/shell/`, `src/app/pages/`, `src/ui/`,
`src/features/`). Everything below describes what is on `main` now, not the G1.1
placeholder page — feature screens fill in `src/features/<name>/` from G3 on.

The SPA is built with npm, never with CMake: `src/web/CMakeLists.txt` is
deliberately a no-op, so `cmake --build --preset dev` never needs Node.

## Toolchain

| Piece | Version / choice |
|---|---|
| Node | 22 LTS — repo-root `.nvmrc` + `engines` in `src/web/package.json` |
| Package manager | npm, with the committed `src/web/package-lock.json`; install only with `npm ci` |
| Build | Vite (`vite.config.ts`) |
| Language | TypeScript, `strict`, `tsconfig.json` (app) + `tsconfig.node.json` (build config) |
| Lint / format | ESLint flat config (`eslint.config.js`) + Prettier (`.prettierrc.json`) |
| Tests | Vitest + React Testing Library + MSW (`src/test/`) |

```sh
nvm use                # or any Node 22 on PATH
cd src/web
npm ci                 # the only supported install
```

## Scripts

```sh
npm run gen            # buf generate over ../../proto into src/gen/ (G1.2)
npm run check:routes   # cross-check src/api/routes.ts against RestGateway.cc
npm run dev            # Vite dev server with the /api proxy (see below)
npm run build          # typecheck + vite build + the JS budget check
npm run test           # vitest run (unit + component tests)
npm run lint           # eslint .
npm run typecheck      # tsc --noEmit for the app and for the build config
npm run format:check   # prettier --check .
npm run format         # prettier --write .  (the only auto-fixer)
npm run check          # gen + check:routes + lint + typecheck + format:check + test + build
```

`npm run check` is what CI runs; it is the definition of "green" for a web
change. `gen` runs before `dev`, `build`, `test` and `typecheck` through npm's
`pre*` hooks, so the generated directory always exists.

**`gen` is a real `buf generate`, not a stub** (G1.2). It runs
`buf generate ../../proto --clean` with `protoc-gen-es`, both from
`node_modules/.bin` — no system protoc. It deliberately does not trust buf's
exit code: after a successful run every `.proto` must have produced a non-empty
`src/gen/**/_pb.ts`, or `gen` exits 1. Without that check, `build`, `test` and
`typecheck` all pass with no generated types at all and the mistake only
surfaces in a feature task much later. The command is skipped when the protos,
the template and the buf version are unchanged.

**`check:routes` guards the REST surface in both directions.** `src/api/routes.ts`
is written by hand, one entry per `FMGR_ROUTE(...)` in `src/rest/RestGateway.cc`,
and `scripts/check-routes.mjs` fails if either side has a route the other does
not, if a route's RPC or `<noun>/<verb>` key is wrong, or if a `…/watch` stream
is missing from `src/api/sse.ts`. **A C++ PR that adds a route must add its
`routes.ts` line in the same PR.** The checker also fails when it can no longer
parse one of the two sides, so it cannot pass by finding nothing.

**`NODE_ENV` is set by the scripts, not by your shell.** `build` runs
`NODE_ENV=production vite build`, `test` runs `NODE_ENV=test vitest run` and
`dev` runs `NODE_ENV=development vite`. Vite, Vitest and React all change
behaviour on an inherited `NODE_ENV` (dev React in `dist/`, and React 19 tests
that fail on `React.act`), so pinning it in `package.json` is what makes a local
run and CI produce the same artifact and the same results.

**Bundle budget.** `npm run build` runs `scripts/check-bundle-size.mjs`, which
sums the gzipped size of every JS file `dist/index.html` loads before first
paint (the entry chunk plus its `modulepreload` dependencies) and fails the
build above **250 KiB gzipped**. Chunks behind `import()` are not counted —
table/tree screens are expected to be lazy. Override for an experiment with
`FMGR_WEB_JS_BUDGET_KIB=400 npm run build`.

## Tests

Run them through the script:

```sh
npm run test           # NODE_ENV=test vitest run — 257 tests in 15 files
```

**Use `npm run test`, never a bare `npx vitest run`.** The `test` script pins
`NODE_ENV=test` (see above); running Vitest directly inherits whatever the shell
exports, and Vitest, Vite and React all change behaviour on an inherited
`NODE_ENV`. With `NODE_ENV=production` exported, all 14
`src/ui/primitives.test.tsx` tests fail, while `npm run test` is 257/257 with
`NODE_ENV` set to `production` or unset — the script is the pin, so bypassing the
script bypasses it. (A reviewer lost a round to exactly this during the G1.3
review.)

**`test.css: true` in `vite.config.ts` is load-bearing, not cosmetic.** Vitest
stubs CSS by default, and a stubbed `import css from './x.css?raw'` resolves to
an **empty string** instead of failing. `src/ui/tokens.contrast.test.ts` reads
`tokens.css` back that way and asserts on the values it finds: with the setting
off it parses nothing, and every contrast assertion passes **vacuously** — a
guard whose failure mode is silence. Turning it on gives CSS modules their real
scoped class names *and* makes the `?raw` import return the file's text. Anyone
tempted to switch it off for speed should read that test first.

**Accessibility goes through one matcher**, registered with `expect.extend` in
`src/test/setup.ts` and configured there — `color-contrast` is off because jsdom
has no layout engine or canvas, so axe can only ever report it "incomplete";
contrast is checked from the real token values in `tokens.contrast.test.ts`
instead. A test renders, then asserts:

```ts
import { axe } from '../test/setup'; // path relative to the test file

expect(await axe(container)).toHaveNoViolations();
```

`toHaveNoViolations` asserts on axe's *results*, not on a container, which is why
the runner and the matcher live together. `src/test/setup.ts` is the only module
with the configured runner, so tests import it from there.

## Dev loop

`scripts/agent/env.sh` gives every agent slot its own ports, so several dev
servers can run on one machine:

```sh
AGENT_SLOT=3 source scripts/agent/env.sh   # sets FMGR_REST_LISTEN, FMGR_WEB_DEV_PORT
out/build/dev/src/server/freezerd &        # gRPC 50051+10·N, REST 18080+10·N
cd src/web && npm run dev                  # 127.0.0.1:5173+10·N
```

`vite.config.ts` reads both variables:

- it listens on `127.0.0.1:$FMGR_WEB_DEV_PORT` with `strictPort: true`, so it
  fails loudly instead of silently moving to another port;
- it proxies `/api` to `http://$FMGR_REST_LISTEN` with `changeOrigin: false`, so
  the `Origin`/`Host` pair the gateway validates in G0.1 still matches (Vite sends
  the request from `127.0.0.1:5173+10·N` and reports the same host upstream).

The SPA is same-origin only (G-arch 5): in production `freezerd` (G0.3) or a
reverse proxy in front of it serves the built `dist/`, and the gateway sends no
CORS headers. There is no separate API host to point at.

## Layout (G-arch 11)

| Path | What goes there | Landed in |
|---|---|---|
| `src/api/` | The REST client (`client.ts`), typed error mapping (`errors.ts`), the hand-written route table (`routes.ts`, `route-types.ts`), the SSE wrapper (`sse.ts`) and the TanStack Query hooks (`hooks/`) | G1.2 |
| `src/gen/` | Generated proto types — gitignored, regenerated by `npm run gen`, never edited | G1.2 |
| `src/app/` | Providers, router and route map, guards, session context, i18n bootstrap, `global.css`, `ErrorBoundary` | G1.3 |
| `src/app/shell/` | The chrome around every screen: `AppShell`, `SideNav`, `TopBar`, `UserMenu`, `LabPicker`, `GlobalLookup`, `ConnectionIndicator` | G1.3 |
| `src/app/pages/` | Router-level screens that belong to no feature: `NoAccess`, `NotFound`, `PlaceholderScreen` | G1.3 |
| `src/ui/` | Shared primitives (`Button`, `Table`, `Dialog`, …), `classNames.ts` and `tokens.css` (design tokens) | G1.3 |
| `src/features/<name>/` | One directory per screen, own i18next namespace | G1.3 route map, filled in from G3 |
| `src/test/` | `setup.ts` (matchers + the configured axe runner), MSW `server.ts`, `fakeApi()`, `fakeEventSource()`, `renderWithProviders()` | G1.2/G1.3 |
| `locales/en/<namespace>.json` | Translations; `common` is the default namespace, one file per feature | all |
| `scripts/` | `gen.mjs`, `check-routes.mjs`, `check-bundle-size.mjs` — the checks `npm run check` runs | G1.1/G1.2 |
| `e2e/` | Playwright against a real `freezerd` — not created yet | G5.1 |

**The two render helpers are not duplicates and both stay.**
`src/test/render.tsx`'s `renderWithProviders()` mounts **one component** with the
app's providers, which is what a screen test wants;
`src/app/testing.tsx`'s `renderApp()` boots the **whole app** through its real
router, which is what a route, guard or shell test wants. Collapsing one into the
other costs the coverage the other exists for.

## Rules that apply to every web change

1. **No literal user-visible strings.** Text comes from `locales/en/*.json`
   through `react-i18next`; the ESLint rule `i18next/no-literal-string` (mode
   `jsx-only`, with a technical-attribute exclude list) fails the build
   otherwise. Keys are type-checked against the JSON by `src/app/i18next.d.ts`.
2. **Types are generated, never hand-written** (G-arch 4). `src/gen/` is
   gitignored; the gateway speaks proto3 JSON with **snake_case** names, int64 as
   strings, enums as names, defaults omitted.
3. **Transport** (G-arch 5): one wrapper, `src/api/client.ts`, every unary call a
   `POST /api/v1/<noun>/<verb>`; live data over `EventSource` on the `…/watch`
   routes. Mutations send the CSRF header. Every failure arrives as an
   `ApiError` — a translated gRPC status, `UNAVAILABLE` for a network failure,
   `INTERNAL` for a body that is not the expected message — and
   `onSessionExpired()` fires on `UNAUTHENTICATED` so the query cache can be
   cleared (rule 5).
4. **Auth** (G-arch 6): the session is an `HttpOnly` cookie. The token never
   reaches JavaScript — not in memory, not in `localStorage`, not in URLs.
5. **PHI stays out of the browser** (G-arch 7): no API data in
   `localStorage`/`sessionStorage`/IndexedDB/Cache API, no service worker, no API
   payloads in `console.log`, and the TanStack Query cache is cleared on logout,
   expiry and any 401. `localStorage` holds UI preferences only.
6. **Permission gating is UX, not security** (G-arch 8): the server enforces;
   every screen still handles `PERMISSION_DENIED`.
7. **Testing** (G-arch 10, TDD): write the test first. MSW fakes come from one
   factory with per-RPC error injection (`fakeApi({ fail: { 'sample/list':
   'PERMISSION_DENIED' } })`); every screen tests its `UNAUTHENTICATED`,
   `PERMISSION_DENIED`, conflict and network-failure branches, not just the happy
   path.
   `fakeApi()` answers **every** route in `routes.ts`, but only the routes with a
   resolver in `fakeApi.ts` return real data — the rest reply with the response
   message's *defaults*. So `box/list` is an empty list, not an error: an
   assertion like "the grid is empty" can pass for the wrong reason against a
   route nobody has implemented yet. Assert on the specific data you seeded, or
   add a resolver, rather than treating an empty result as proof.
8. **Adding a dependency** needs the `lock:deps` label (it covers
   `src/web/package.json` and `package-lock.json` since G1.1) and a one-line
   justification in the PR. No CDN assets, hosted web fonts, analytics or
   error-reporting services (G-arch 3).
9. **SPDX headers** on every new `.ts`, `.tsx`, `.js`, `.mjs`, `.cjs` (`//`) and
   `.css` (`/* … */`) file: `tools/check-spdx-headers.sh` enforces it in CI, as
   it does for C++ and Python.

## CI

`.github/workflows/build.yml` has a `web` job, independent of the C++ matrix:
`actions/setup-node` reads the repo-root `.nvmrc`, caches npm on
`src/web/package-lock.json`, then runs `npm ci && npm run check` on
`ubuntu-24.04`, plus the SPDX check.

## Troubleshooting

- **A shell that exports `NODE_ENV=production` breaks `npm ci`** — npm then
  skips `devDependencies`. Use `NODE_ENV=development npm ci`. The npm scripts
  themselves pin `NODE_ENV` for `dev`/`test`/`build`, so `npm run check` is safe
  either way, but the install is not: without devDependencies nothing runs.
  Verify with `env | grep NODE_ENV` before blaming the code.
- **`npm` fails with `EPERM` in `~/.npm/_cacache`** — npm blames root-owned
  files in the shared cache; on this machine that cache is owned by the same
  user and the write is still refused (an agent sandbox that only allows writes
  under the checkout), so treat the message as a symptom, not a diagnosis. Point
  npm at a cache inside the worktree:
  `npm ci --cache "$PWD/../../out/npm-cache"` (or
  `npm_config_cache=out/npm-cache npm ci`), with `NODE_ENV=development` so
  devDependencies are installed.
- **Vite exits with `Port 5173 is already in use`** — `strictPort` doing its job:
  another slot is on that port. Set `AGENT_SLOT=N` and source
  `scripts/agent/env.sh` again.
- **ESLint reports `i18next/no-literal-string` in a test** — assertion strings
  are fine; JSX text is not. Assert on the translated string, and add the
  attribute to the exclude list in `eslint.config.js` only if it really is
  technical (`className`, `role`, …).
