# Handoff note — 2026-09-29, G3.1 lab layout tree (#51, worker-2)

The first Section G feature: `useLabLayout(labId)` loads a lab's freezers,
storage containers, box types and boxes once, and derives both the collapsible
tree the layout screen renders and a `locationPath(boxId, position)` helper
(freezer → … → box → position). TODO.md says G3.2–G3.5 and G3.8 all reuse this
hook, so the shape was chosen for them, not for this screen. Spec: PRD §9 (the
flow it serves), §10 (Web UI), TODO.md §Section G → G3.1. Branch
`feat/51-lab-layout-tree`, PR **#52**.

**Changed:**

- `src/web/src/features/layout/layoutModel.ts` — the pure model: `buildLayoutTree`
  (forest, per-subtree box counts, position counts) and `resolveLocationPath`
  (`segments`, `placed`, `partial`), plus the `LabLayoutData` / `LayoutNode` /
  `LocationPath` types.
- `src/web/src/features/layout/useLabLayout.ts` — the hook: four lab-scoped
  queries, the derived `tree`, the `locationPath` callback, the raw lists for
  G3.4/G3.8, `isPending` / `isError` / `error` / `refetch`.
- `src/web/src/api/hooks/layout.ts` — `layoutKeys` and `useFreezers` /
  `useStorageContainers` / `useBoxTypes` / `useBoxes`; re-exported from
  `src/api/hooks/index.ts`.
- `src/web/src/features/layout/LayoutTreeScreen.tsx` (+ `.module.css`) — the real
  screen, replacing the G1.3 placeholder. Route, TODO id and `sample.read`
  permission in `src/app/route-map.tsx` are **unchanged**.
- `src/web/src/features/layout/LayoutTree.tsx` (+ `.module.css`) — the tree.
- `src/web/src/features/layout/paths.ts` — `boxPath(labId, boxId)`.
- `src/web/locales/en/layout.json` — the screen's copy; `locales/en/common.json`
  — `enums.ContainerKind.*`, the key shape `api/helpers.enumLabel` looks up.
- `src/web/src/test/fakeApi.ts` — a seeded `lab-demo` layout (two freezers, a
  rack/drawer/shelf tree, 96- and 9-position box types, four boxes) **including
  archived rows**, and resolvers for `freezer/list`, `storage-container/list`,
  `box-type/list`, `box/list`. Until now those four answered with protobuf
  defaults, so "hidden when archived" would have passed vacuously.
- `src/web/src/app/App.test.tsx` — the per-route expectation table gained
  `task: null` for an implemented screen, which flips that route's assertion to
  "the placeholder sentence is gone"; plus a file-level `fakeApi()` handler set,
  because a real screen in the route map fetches.
- Tests: `layoutModel.test.ts` (20), `useLabLayout.test.tsx` (9),
  `LayoutTreeScreen.test.tsx` (10), `paths.test.ts` (2).

**Decisions:**

- **The guards are the Qt resolver's, not a variant.** `resolveLocationPath`
  walks the container chain box-side first, ends on a revisited id (cycle) or a
  parent that is not in the loaded set (orphan), and marks the result `partial` —
  including Qt's split between `placed` (a box was named) and `partial` (the
  chain broke), and Qt's "an empty `box_id` is an unplaced sample, not a
  failure". `src/qt/LocationPathResolver.cc` is the reference; its cycle guard
  arrived only in `2afb49b`, which is the failure class this exists for.
- **The model is pure and React-free**, in its own file. G3.5's lookup card and
  G3.3's detail page need a path without mounting a tree, and the guards have to
  be testable headlessly.
- **Archived rows are hidden by the client.** None of the four list RPCs has an
  `include_archived` field, so they are always in the response. The tree drops an
  archived node *and its subtree* (a live box under a dead parent must not be
  shown as reachable); the path deliberately still resolves **through** an
  archived container, because it answers "where is this sample" — the Qt resolver
  has no archived filter at all. Both behaviours have their own test so the
  difference is a decision, not an accident.
- **No `page` field is sent, and that is deliberate.** All four RPCs ignore
  paging today (`src/server/BoxServiceImpl.cc`), and the Qt client calls them the
  same way. A page boundary in the middle of a parent chain would manufacture
  orphans. If those RPCs start paging, all four hooks in
  `src/api/hooks/layout.ts` change together — the comment there says so.
- **A disclosure list, not `role="tree"`.** `role="tree"` promises roving focus
  and arrow-key navigation; claiming it without implementing it is worse for a
  keyboard user than a `<button aria-expanded>` that owns its children and a
  `<Link>` for a box row (selecting a box *is* navigation to G3.4). Collapsed
  branches stay in the DOM under `[hidden]`, so `aria-controls` always points at
  a real element; the stylesheet carries `.children[hidden] { display: none }`
  because an author `display` rule would otherwise beat the UA's.
- **One error state, never a partial tree.** `isError` is the OR of the four
  queries and the screen renders `ErrorState` (with the translated failure and
  the request id) instead of the tree: freezers with nothing under them look
  exactly like an empty lab. That is the same failure mode `doc/dev/web.md`
  records for `fakeApi`'s default answers.
- **`layoutKeys` lives in `src/api/hooks/layout.ts`, not in the feature.**
  G-arch 11 puts the query hooks in `src/api/hooks/`; G3.4/G3.8 invalidate
  `layoutKeys.all(labId)` from there, and the feature keeps the derived model.
- **`boxPath` is hand-written with a drift test.** `route-map.tsx` imports this
  screen, so importing the map back would be an import cycle; `paths.test.ts`
  takes the pattern *from* `ROUTES` and fails if the two diverge.

**Tests:**

Written first (red: `Failed to resolve import "./layoutModel"`, then no
`layoutKeys` export, then the placeholder assertions).

```sh
cd src/web
npm run test -- src/features/layout/
#  Test Files  4 passed (4)
#       Tests  41 passed (41)

export NODE_ENV=production && npm run check     # EXIT=0
#  Test Files  19 passed (19)   Tests  298 passed (298)
#  check-bundle-size: initial JS 196.0 KiB gzipped, budget 250 KiB
env -u NODE_ENV npm run check                   # EXIT=0
#  Test Files  19 passed (19)   Tests  298 passed (298)
#  check-bundle-size: initial JS 196.0 KiB gzipped, budget 250 KiB
```

(The one remaining lint message is the pre-existing
`react-hooks/incompatible-library` warning in `src/ui/Table.tsx`, untouched by
this PR.)

**The guards can fail — planted violations, each reverted.** Every plant was
applied to `layoutModel.ts` alone and the file restored from a copy afterwards;
`git status` was clean before the next one.

| Plant | Command | Result |
|---|---|---|
| archived filter removed | `npm run test -- src/features/layout/layoutModel.test.ts -t 'hides an archived'` | `AssertionError: expected [ 'fz-gone', 'fz-live' ] to deeply equal [ 'fz-live' ]` — 1 failed |
| same plant, screen level | `npm run test -- src/features/layout/LayoutTreeScreen.test.tsx -t 'hides archived'` | `× hides archived nodes instead of greying them out` — 1 failed |
| visited guard removed from `buildLayoutTree` | `npm run test -- src/features/layout/layoutModel.test.ts -t 'terminates on a container cycle'` | `RangeError: Maximum call stack size exceeded` — 1 failed |
| orphan guard removed from `resolveLocationPath` | `npm run test -- src/features/layout/layoutModel.test.ts -t 'stops at an orphaned container'` | `TypeError: Cannot read properties of undefined (reading 'parentId')` — 1 failed |
| cycle guard removed from `resolveLocationPath` | `node_modules/.bin/vitest run src/features/layout/layoutModel.test.ts -t 'stops on a cycle'` with `NODE_ENV=test` and `--max-old-space-size=256` | worker dies: `Worker exited unexpectedly with signal SIGABRT` after 1.4 s (the walk allocates until the heap is gone) — exit 1 |

The last one is why its own run was heap-capped and process-group-killed: the
guarded failure mode is non-termination, so an unbounded reproduction would take
the machine with it. The other four are deterministic.

**Known limitations / follow-ups:**

- **`freezer/list`, `storage-container/list` and `box-type/list` require
  `freezer.configure` / `box.configure`** (`src/server/BoxServiceImpl.cc:215-236`),
  which the built-in `Member` and `ReadOnly` roles do not hold, while route
  `layout` is declared `sample.read`. A Member therefore gets `PERMISSION_DENIED`
  on three of the four calls and sees the error state. Posted as a `QUESTION` on
  #51 with two options (relax the read permissions, or re-gate the route); the
  screen handles the refusal either way, but the decision is the lead's.
- **`fakeApi`'s `paginate()` caps at 100 rows per list.** The real RPCs return
  everything, so a test that seeds more than 100 layout rows would see a
  truncated tree for a test-only reason. Worth knowing before G3.4/G3.8 seed
  large labs.
- **A root container no freezer names is not rendered** (the tree pairs a freezer
  with the container its `layout_root_id` names, as the Qt resolver does). A
  freezer whose root is missing still renders, as an empty node, so the data
  problem is visible; the unattached container itself is G3.8's to surface.
- **`App.test.tsx`'s route guard is weaker for an implemented screen**: it can no
  longer assert the TODO id in the body, so it asserts the h1 plus "the
  placeholder sentence is absent". A screen wired to the wrong *implemented*
  screen that shares the `layout` namespace would still pass; only
  `LayoutTreeScreen.test.tsx` covers the tree itself.
- **`useLabLayout` is the seam for the next four tasks.** G3.2 needs `boxes` and
  `layoutKeys` to invalidate after a move; G3.3 needs `locationPath`; G3.4 needs
  `boxTypes` for the grid and `boxPath`; G3.8 needs the raw lists plus the
  mutations, which are **not** written yet (only the four read hooks exist).
- `npm run check` does not cover the C++ side; no C++, proto or CMake file is
  touched by this PR, so the C++ matrix has nothing of ours to read.
