# Handoff note — 2026-09-29, entry-chunk split (#64, worker-2)

The initial JS bundle was 14.6 KiB under a hard 250 KiB budget with G3.2 merged
and two more G3 screens in flight, so the next one would have failed
`npm run build` with a message that reads as "this screen is too big" rather than
"the entry chunk is carrying every screen". This splits it: the shell stays
eager, every feature screen is fetched when its route is first rendered, and
`src/ui/index.ts` stops re-exporting the one heavy piece in the kit.

**Changed**

- `src/app/route-map.tsx` — the 17 static screen imports become
  `lazyScreen(() => import(…), 'Name')`. The route map stays the single source of
  truth for path, permissions and nav; only the screen reference changed.
- `src/app/lazyScreen.ts` (new) — `React.lazy` for a *named* export, with a
  runtime check that fails loudly when the module has no such component. A
  mistyped name is a type error at the call site; `module.default` would instead
  force every screen to grow a second export.
- `src/app/router.tsx` — one `Suspense` boundary per route, **inside** the shell
  for shell routes and around the whole page for the `bare` ones. The guard sits
  *outside* the boundary so a denial or a redirect still renders while a chunk is
  in flight.
- `src/app/pages/ScreenLoading.tsx` (+ `.module.css`, new) — the fallback: the
  kit's `Spinner`, centred in the content area. `locales/en/shell.json` gains
  `screen.loading`.
- `src/ui/index.ts` — `Table`, `TableColumn`, `TableFeatures` and `TableProps`
  are no longer re-exported, with the measurement in the comment so nobody adds
  the line back by accident.
- `src/features/samples/SampleBrowserScreen.tsx`, `sampleColumns.tsx` — import
  `Table` / `TableColumn` from `../../ui/Table`.
- `scripts/check-bundle-size.mjs` — two guards beyond the budget (below).
- `vite.config.ts` — `build.manifest: true`, which guard 2 reads.
- `doc/dev/web.md` — the bundle section documents all three guards and the barrel
  rule; the layout table notes the `Table` exception.

## The numbers

All measured with Node v22.23.3, `NODE_ENV=production`, `npm run build`.

| Tree | Entry chunk | Initial JS (gzip) | JS chunks |
|---|---|---|---|
| `origin/main` `9ca30ce` (G3.1 only) | 635.4 KiB raw, 1 chunk | 196.0 KiB | 1 |
| `feat/55-sample-browser` `a1c048f` (G3.2) | 781.2 KiB raw, 1 chunk | 235.4 KiB | 1 |
| + lazy routes only | 616.6 KiB raw | 189.2 KiB | 22 |
| + lazy routes + barrel fix | 475.2 KiB raw | **155.7 KiB** | 22 |

Headroom goes from **14.6 KiB to 94.3 KiB**. Nothing shrank — it moved: the
build's total JS raw is 786.8 KiB either side, and 22 chunks now exist where
there was one.

**What the next screen costs.** G3.2 added 39.4 KiB to the single entry chunk. A
screen of that size now adds **nothing to the initial JS**: it lands in its own
chunk, or in a shared on-demand chunk it has in common with a screen already
deferred. So the number to watch stays ~155.7 KiB while G3.3 and G3.4 land, and it
would only move if a screen introduced a heavy dependency the *shell* also uses —
which is exactly what guards 2 and 3 exist to catch.

The two deferred chunks a first visit to `/labs/:labId/samples` fetches are
`SampleBrowserScreen` (37.7 KiB gzip, carrying TanStack Table + Virtual) and a
shared `useLabLayout` chunk (46.3 KiB gzip, `@bufbuild/protobuf`, the generated
descriptors and the data-layer hooks) that the layout screen shares and the
browser caches. The login page fetches neither.

## Decisions

- **Both halves of the fix were needed, and I verified the second one rather than
  assuming it.** Lazy routes alone recovered 46.2 KiB. Removing `Table` from the
  barrel recovered another 33.5 KiB — and putting only the *export line* back,
  with consumers still importing `../../ui/Table` directly, returned the entry to
  the exact same 189.2 KiB and the same entry chunk hash. So the mechanism is not
  "a screen used `Table` through the barrel": **an unused re-export of a
  component that imports CSS cannot be tree-shaken out of the entry chunk**,
  because the CSS import makes the module side-effectful. That is the trap the
  issue named, and it is why the fix is "import it from its own module" rather
  than "make the barrel lazy-safe" — there is no synchronous way to keep the
  export and lose the weight.
- **The shell stays eager, deliberately.** `AppShell`, `SideNav`, `TopBar`,
  `guards.tsx` and the kit they import are unchanged, so the frame paints on the
  first frame and only the screen shows a fallback. That was the reasoning behind
  G3.1's static import and it still holds; what changed is the arithmetic.
- **The budget was not touched.** 250 KiB is a G-arch decision; 155.7 KiB leaves
  no reason to revisit it.
- **The proof is a build guard, not a paragraph.** `scripts/check-bundle-size.mjs`
  now also asserts, from real build output, that (2) every
  `src/features/*/*Screen.tsx` is a dynamic entry of the Vite manifest and not in
  the entry chunk's static imports, and (3) TanStack Table and TanStack Virtual
  appear only in chunks behind `import()`. Both were verified red on the unfixed
  tree: guard 2 lists all 17 screens, guard 3 reports
  `TanStack Table is in the initial JS: assets/index-0TNKIKdU.js`. Both fail
  rather than pass when their input is missing (no manifest, no
  `src/features/*/*Screen.tsx`). They went into
  `check-bundle-size.mjs` rather than a new script because `src/web/package.json`
  is under `lock:deps` and no new npm script may be added.

## Verification

```sh
export PATH="/Users/yuxin/Code/personal/FreezerManager/out/tools/node22/bin:$PATH"
cd src/web
env -u NODE_ENV npm run check     # exit 0
NODE_ENV=production npm run check # exit 0 — 408 tests in 28 files, 1 pre-existing
                                  # lint warning in src/ui/Table.tsx
```

- `npm run check` green **both** with `NODE_ENV=production` exported and with it
  unset. The two runs also cover the new `lazyScreen` tests (3) and the unchanged
  `App.test.tsx` route table (44), which is what proves a lazy screen still
  renders through the real router and guard order.
- Guard 2 red before the fix (exit 1, 17 screens listed), green after.
- Guard 3 red with the barrel export restored (exit 1), green without it.

## Known limits and follow-ups

- **Ordering: this branch needs #58 (G3.2) merged first.** The barrel change is
  what makes `SampleBrowserScreen.tsx` and `sampleColumns.tsx` import
  `../../ui/Table`; that edit and PR #58 cannot land in either order without the
  other. Measured on `a1c048f` + this change, then rebased on `origin/main` once
  G3.2 landed.
- **Guard 3 only knows the two TanStack packages.** A third heavy kit piece would
  need its own marker, and the markers are string literals inside those packages
  (the `fnName` of the core row-model feature, the virtualizer's option names),
  not public API. A
  marker that is in no chunk at all is reported, not treated as a pass, so the
  guard cannot go quiet without saying so — but on a tree where no screen uses
  `Table` yet it is inactive by definition.
- **All 15 i18n namespaces are still in the entry chunk** (`src/app/i18n.ts`
  imports every `locales/en/*.json` eagerly, 2.5 KiB gzipped in total). Not worth
  splitting at this size, and doing it would need `addResourceBundle` before
  render; recorded here so a future task measures before deciding.
- **`doc/dev/web.md:99` still says "257 tests in 15 files"** for `npm run test`
  (it is 408 in 28). Stale before this task; left alone to keep the PR to one
  issue.
- No locks were held, and no file under `.github/workflows/`,
  `CMakePresets.json` or the top-level `CMakeLists.txt` was touched.

Implemented by worker-2 (Claude Code).
