# Handoff note — 2026-09-28, G1.1 src/web scaffold, toolchain and CI job (#32, worker-3)

`src/web/` is now a real Vite + React + TypeScript SPA instead of a placeholder,
with the whole G-arch baseline installed and enforced, one placeholder page, four
passing tests and its own CI job. Every later G task starts from here: G1.2 adds
the API layer under `src/api/` + `src/test/`, G1.3 the app shell, router and
`src/ui/` kit, and feature tasks then touch only `src/features/<name>/` and their
own `locales/en/<feature>.json` (TODO.md §Section G). PRD §10; this delivers P3.

**Changed:**

- `src/web/` (new): `vite.config.ts` (dev server + `/api` proxy + Vitest), a
  single `tsconfig.json` for the app plus `tsconfig.node.json` for the build
  config, `eslint.config.js` (flat; typescript-eslint **strict and type-aware**
  over `src/**`, react-hooks, jsx-a11y, `i18next/no-literal-string`,
  Prettier last), `.prettierrc.json`, `package.json` + committed
  `package-lock.json`, `index.html`.
- `src/web/src/`: `main.tsx`, `app/App.tsx` (the one page), `app/App.module.css`,
  `app/global.css`, `app/i18n.ts` (bundled resources, `initAsync: false`),
  `app/i18next.d.ts` (type-checked `t()` keys against the JSON), `ui/tokens.css`
  (seed design tokens; G1.3 owns growing it), `test/{setup,server}.ts` (MSW) —
  they sit in the layout G-arch 11 prescribes.
- `src/web/scripts/check-bundle-size.mjs`: fails `build` above 250 KiB gzipped
  initial JS; `scripts/gen.mjs`: the `gen` stub G1.2 replaces.
- `src/web/.gitignore`: `node_modules/`, `dist/`, `src/gen/` **and
  `!package-lock.json`**, because the repo-root `.gitignore` ignores
  `package-lock.json` globally for agent tooling.
- `.nvmrc` (repo root, `22`), `.github/workflows/build.yml` (new independent
  `web` job), `tools/check-spdx-headers.sh` (now `*.ts *.tsx *.js *.mjs *.cjs`
  and `*.css`), `scripts/agent/env.sh` (`FMGR_WEB_DEV_PORT` = 5173 + 10·N),
  `src/web/CMakeLists.txt` (comment only, still a no-op), `doc/dev/web.md`,
  `AGENTS.md` §2/§3/§4.

**Decisions:**

- **`.nvmrc` at the repo root**, not in `src/web/`: it is the path
  `actions/setup-node` reads by default and what `nvm use` finds from the repo
  root. `engines.node: ^22` in `src/web/package.json` pins the app the same way.
- **Radix primitives are installed but unused in G1.1** (`dialog`,
  `dropdown-menu`, `popover`, `tooltip`). G-arch 1 allows only those, and G1.3 —
  which builds Dialog/Menu/Popover/Tooltip — does not hold `lock:deps`, so the
  lock:deps holder installs them once. Nothing imports them yet, so they are
  tree-shaken out of the bundle.
- **`eslint` is pinned to `^9`**: `eslint-plugin-jsx-a11y@6.10.2` peers on
  `eslint ^3–^9`, so `eslint@10` breaks `npm install` with ERESOLVE. Move to 10
  once jsx-a11y supports it.
- **`i18next/no-literal-string` runs in `jsx-only` mode** with a technical
  attribute exclude list. Widening it to every string literal is a G1.3 call.
- **`NODE_ENV` is pinned by the npm scripts** (`build` → `production`, `test` →
  `test`, `dev` → `development`), because Vite/Vitest/React all change behaviour
  on an inherited value. `NODE_ENV=production npm run check` and
  `env -u NODE_ENV npm run check` now both pass and produce the **identical**
  artifact (md5 `12a77112…`); before the pin, dev React leaked into `dist/`
  (141.7 KiB instead of 81.9 KiB) and two tests failed on `React.act`. The
  `npm ci` part is still environment-sensitive (npm skips devDependencies under
  `NODE_ENV=production`), which the Troubleshooting section covers.
- **The react-hooks config lookup asserts non-empty** in `eslint.config.js`:
  spreading `undefined` would have dropped all 17 rules while lint still passed.
- **`npm run check` does not run `dev`** (a server, not a check); the `pre*`
  hooks run `gen` before `dev`/`build`/`test`/`typecheck` so G1.2 only has to
  rewrite `scripts/gen.mjs`.
- `ctest --preset dev` was **not** run locally: no C++ source changed and
  `src/web/CMakeLists.txt` is comment-only and unreferenced by the top-level
  `CMakeLists.txt`. CI runs the full matrix on the PR.

**Tests:**

- `src/web/src/app/App.test.tsx` — the page renders its title and body from the
  bundled `en` locale (proves i18next, the JSON resources and CSS Modules).
- `src/web/src/test/server.test.ts` — MSW intercepts a registered handler, and
  an unregistered request fails instead of touching the network.
- `cd src/web && npm ci` → 459 packages, exit 0; `npm run check` → exit 0 (lint
  clean, typecheck clean, Prettier clean, **4/4 tests passed**, initial JS
  **81.9 KiB gzip** vs the 250 KiB budget), run twice — once with
  `NODE_ENV=production` exported, once with it unset — with the same result and
  the same artifact hash; `tools/check-spdx-headers.sh` → exit 0;
  `git status --short` → empty afterwards.
- Guards proved to fail, not just pass: `FMGR_WEB_JS_BUDGET_KIB=100 node
  scripts/check-bundle-size.mjs` → exit 1 ("over budget by 41.7 KiB"); a planted
  `src/app/LintProbe.tsx` with JSX text → eslint exit 1 on
  `i18next/no-literal-string`; an `eslint.config.js` copy with the react-hooks
  lookup forced to `undefined` → eslint exits 2 with a message instead of
  silently running zero react-hooks rules.
- Dev loop on slot 3: Vite on `http://127.0.0.1:5203/`, `GET /` → 200,
  `/api/v1/healthz` proxied to the stub on `127.0.0.1:18110` with `Host` and
  `Origin` both still `127.0.0.1:5203` (`changeOrigin: false`), and a second
  `vite` on the same port exits 1 (`strictPort`).
- CI `web` job (ubuntu-24.04, Node from `.nvmrc`): run URL in PR #37.

**Known limitations / follow-ups:**

- `npm run gen` generates nothing yet — G1.2 replaces the stub and adds
  `buf generate` over `../../proto`.
- No router, providers or `src/ui/` components yet: `App.tsx` is a placeholder
  page and the only entry point. G1.3 owns the shell and the route-map
  placeholders; G1.2 owns `src/api/` and `src/test/fakeApi.ts`.
- The seed in `src/ui/tokens.css` covers only what the placeholder page needs;
  the full light/dark set with WCAG AA contrast is G1.3.
- `@testing-library/user-event` is installed but not yet used (G1.3+ keyboard
  tests).
- Local notes: the agent shell exports `NODE_ENV=production`, which makes `npm`
  skip devDependencies at install time (use `NODE_ENV=development npm ci`), and
  `~/.npm/_cacache` has root-owned files (use
  `npm_config_cache=out/npm-cache`). `npm run check` itself is immune to the
  first one now that the scripts pin `NODE_ENV`. Both are in `doc/dev/web.md` →
  Troubleshooting.
