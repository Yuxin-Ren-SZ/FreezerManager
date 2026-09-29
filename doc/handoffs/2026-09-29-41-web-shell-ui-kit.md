# Handoff note — 2026-09-29, G1.3 app shell and UI kit (#41, worker-2)

`src/web/` no longer has a single placeholder page. It has the shell every screen
lives in, the route map as one source of truth for the router and the side nav,
and the shared `src/ui/` kit that feature tasks build on. A feature task now
edits one `src/features/<name>/` directory and its own `locales/en/<name>.json`,
which is the promise G-arch 11 makes. Spec: PRD §10, §1.3; TODO.md §Section G →
G1.3. Branch `feat/41-web-shell-ui-kit`, PR **#45**.

**Changed:**

- `src/web/src/ui/` — the kit: `Button`, `IconButton`, `TextField`, `Select`,
  `Checkbox`, `Dialog`, `ConfirmDialog`, `Toast` (+ provider/hook), `Table`,
  `Tabs`, `EmptyState`, `ErrorState`, `Spinner`, `Skeleton`, `Badge`, `Kbd`,
  plus `VisuallyHidden`, a four-line `classNames`, an `index.ts` barrel and
  `a11y.ts` (the one axe entry point for tests).
- `src/web/src/ui/tokens.css` — the full light/dark token set. It replaces
  G1.1's seven-colour seed, and `--fmgr-color-border` darkened from `#ccd3db`
  to `#7b8794` because the old value was 1.5:1 against white.
- `src/web/src/ui/tokens.contrast.test.ts` — reads `tokens.css` back through the
  bundler and computes every WCAG ratio in both themes.
- `src/web/src/app/` — `providers.tsx` (QueryClient, session, lab, connection,
  toasts), `session.tsx` (+ `can`/`useCan`/`RequireSession`), `labs.tsx`,
  `connection.tsx`, `queryClient.ts`, `ErrorBoundary.tsx` (class boundary +
  `AppErrorBoundary` + router `errorElement`), `guards.tsx` (`RequireSession`,
  `RouteGuard`), `route-map.tsx`, `router.tsx`, `permissions.ts`,
  `stubSession.ts`, `testing.tsx`, and `shell/{AppShell,TopBar,SideNav,
  GlobalLookup,LabPicker,UserMenu,ConnectionIndicator}.tsx`,
  `pages/{NotFound,NoAccess,PlaceholderScreen}.tsx`.
- `src/web/src/features/<name>/` — twelve directories, one per feature task,
  each holding placeholder screens. `locales/en/<name>.json` — one i18next
  namespace each. `locales/en/shell.json` — the shell's own copy. `ui.json` —
  the kit's.
- `src/web/vite.config.ts` — `test.css = true` (see Decisions).
- `src/web/eslint.config.js` — `i18next/no-literal-string` is off for
  `**/*.test.{ts,tsx}`, where literal text is the fixture.

**Decisions:**

- **The route map is one array.** Path, TODO id, element, future permission, lab
  scope and nav entry live together in `route-map.tsx`, and both the router and
  the side nav are derived from it. A hand-written `<Routes>` tree plus a
  hand-written nav is how a screen ends up reachable but invisible; this way a
  test per route catches it. There is one, iterating `ROUTES`.
- **`test.css = true` in `vite.config.ts`.** Vitest stubs CSS by default, so
  `import css from './x.css?raw'` resolves to `''`. The contrast test would have
  passed *vacuously* on an empty stylesheet — the most dangerous kind of green.
  The sticky-header test carries an explicit `tableCss.length > 0` guard for the
  same reason. Both the contrast test and the sticky-header test read rules out
  of the real stylesheet, because jsdom applies none.
- **`Table` keeps a real `<table>`.** TanStack Table v9 + TanStack Virtual, but
  the windowed gaps are spacer rows rather than `position: absolute`, so column
  alignment, `<th scope>`, the row/column-header roles and `position: sticky`
  keep working. Virtual-core measures its scroll container with `offsetHeight`,
  which jsdom reports as 0; `Table.test.tsx` spies on that getter and returns the
  container's inline `max-height`, which is what a browser measures.
- **Radix only for the modal and the menu** (G-arch 1). `Select` and `Tabs` are
  native/hand-rolled: a `<select>` already has type-ahead, a mobile picker and
  screen-reader support, and the tabs pattern is three ARIA attributes and one
  key handler. `@radix-ui/react-popover` and `react-tooltip` are still unused.
- **Nav entries the role cannot reach are absent, not disabled.** A greyed-out
  list tells a member nothing useful. G-arch 8: this is UX; the server refuses
  the calls regardless, and every screen still has to handle `PERMISSION_DENIED`.
- **The "no access" page distinguishes three cases** — no lab at all, not a
  member of *this* lab, member without the permission. They have three different
  fixes, so they get three different sentences.
- **Two stubs, each in one place.** `App.tsx` wires `stubSessionLoader` (G1.2's
  `auth/whoami`) and `connectionStatus="offline"` (G1.2's SSE wrapper).
  `'offline'` rather than `'live'` because there is no live connection yet, and
  an indicator that always says "Live" is worse than none. `stubSession.ts` is
  one file with one export, so deleting it is a one-line change.
- **`app.scaffold.*` in `locales/en/common.json` is now dead** and was left in
  place: #42 edits that same file in this wave, and a two-line dead key is
  cheaper than a merge conflict. Delete it in a follow-up.
- **`src/ui/a11y.ts` uses `vitest-axe`'s exported `axe` runner, not its
  `toHaveNoViolations` matcher.** It was written while `src/test/setup.ts`
  belonged to #42, so registering a matcher there was not available; the runner
  is the same axe-core and only the failure formatting differs. Folding the
  matcher into `setup.ts` and deleting the file is **#46** — a tidy-up, not a
  fix, and the marker in the file points there rather than at a task that has
  already merged.
- **`src/app/testing.tsx` (`renderApp`) and `src/test/render.tsx`
  (`renderWithProviders`, G1.2) are deliberately both here.** The second mounts
  one component in the provider stack; the first boots the whole application
  with the real route table and a real history, which is what "a user at this
  URL sees this screen" needs. Merging them would make every component test pay
  for the router.
- **The per-route test asserts *which* screen rendered, not just that one did.**
  Each route's heading has to match the title declared by the namespace the test
  says it owns, and the body has to name that route's TODO id — plus a test that
  every route id has an entry, so adding a route forces the decision. Asserting
  only "a level-1 heading exists" would have passed a copy-paste that pointed
  `/labs/:labId/audit` at the samples screen; proven by making exactly that
  edit, which fails with `Unable to find role="heading" and name "Audit log"`.
- **`RouteGuard` checks the lab scope before the "no permission required"
  shortcut.** No route is both `permissions: null` and `scoped: true`, so the
  order is currently inert — which is why it has a test with a synthetic route
  rather than relying on the map. Restoring the old order fails it.

**Resolved during the task:**

- **`vitest-axe` was not on `origin/main`.** It was on #42's branch only, so the
  first `web` run of #45 failed at *lint* in 24 s. I reproduced the cause exactly
  — by moving `node_modules/vitest-axe` aside and re-running the check — rather
  than inferring it from the job name, and posted the evidence as a `QUESTION`
  instead of editing `package.json` under #42's `lock:deps`. **#42 merged as
  `0592dd6`.** After rebasing and `npm ci`, `npm run check` is green with no
  dependency change on this branch: the lock was the only thing missing.

**Open / for the lead:**

- **`TODO.md` G1.3 is done.** The lead owns the tick.
- **Two caveats on how far the verification goes.** The 360 px criterion is
  asserted as CSS rules read out of the stylesheets, because jsdom has no layout
  engine and applies no stylesheet — that is honest technique, not verified
  layout, and the real check is the Playwright pass in **G5.1**. And `npx eslint
  .` exits 0 with **one warning**, not zero: React Compiler flags
  `useVirtualizer` at `src/ui/Table.tsx:99` as "an incompatible library", which
  is informational — the hook's return value cannot be memoized, and the
  component is written so it does not need to be.
- **`doc/dev/web.md` is not updated by this PR.** It describes the G1.1 layout;
  the shell adds `src/app/shell/`, `src/app/pages/` and `src/features/<name>/`,
  and the `test.css` change is worth a line. Deliberately left to avoid touching
  a file the wave's other tasks may also want.
- **`Table` column visibility is component state, not persisted.** G-arch 7
  allows the column layout in `localStorage`; whether a given grid remembers it
  is the feature task's call (G3.2), not the shell's.

**Verification:**

Measured after rebasing onto #42's merge (`0592dd6`), so the counts include
G1.2's tests as well as G1.3's:

```sh
cd src/web
export PATH="/Users/yuxin/Code/personal/FreezerManager/out/tools/node22/bin:$PATH"   # v22.23.3
NODE_ENV=development npm ci
npm run check                       # exit 0 — 255 passed (15 files), 152.7 KiB gz / 250 KiB
NODE_ENV=production npm run check   # exit 0 — 255 passed (15 files), identical
```

`npm run check` now also runs G1.2's `check:routes`: *70 unary routes and 2 SSE
routes agree between RestGateway.cc, routes.ts and sse.ts*.

Guard-failure proofs (planted, run, reverted; full output on #41):
`i18next/no-literal-string` → eslint exit 1; axe → `button-name [critical]`;
contrast → `dark: #6b747d on #10151a is 3.86:1, needs 4.5:1`.
