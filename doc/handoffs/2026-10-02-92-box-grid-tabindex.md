# Handoff note — 2026-10-02, [web] box grid roving tabindex (#92, worker-3)

Issue **#92**: G3.4's grid already satisfied its acceptance criterion — a sample
moves by keyboard — so nothing was broken and this is an ergonomic fix, not a
gap. Every cell was a tab stop, so reaching `H12` of a 96-well box meant tabbing
through the whole box. The grid is now **one tab stop with a roving tabindex**:
arrow keys step between positions, `Home`/`End` go to the ends of the current row
and `Control+Home`/`Control+End` to the first and last declared position. A
corner-to-corner move is **20 key presses** instead of a walk through 96 stops.
Branch `feat/92-box-grid-tabindex`, PR **#113**, draft at the time of writing.

No C++, proto, CMake or workflow file is touched, so CI's C++ matrix and the
`clang-tidy` job are path-gated to *skipped*; the `web` job runs the same
`npm ci && npm run check`.

**Changed:**

- `src/web/src/features/box/boxGridModel.ts` — the movement rule, pure and
  React-free: `cellInDirection(grid, from, direction)` → the nearest declared
  position in that direction or `null`; `cellAtRowEdge(grid, from, edge)` for
  `Home`/`End`; `gridEdgeCell(grid, edge)` for `Control+Home`/`Control+End`;
  types `GridDirection` and `GridEdge`. `cellAt` and `gapCount` are unchanged.
- `src/web/src/features/box/BoxGrid.tsx` — the roving tabindex. `focusedLabel`
  state holds the tab stop; a label → `HTMLButtonElement` ref map is what a key
  press focuses. `role="grid"` on the container with `aria-rowcount`/
  `aria-colcount`, real `role="row"` elements with `aria-rowindex`, and
  `role="gridcell"` wrappers with `aria-colindex`. Arrow/Home/End handling moved
  into `onNavigate`; `Escape` and Space are unchanged.
- `src/web/src/features/box/BoxScreen.module.css` — `.grid` is a flex column of
  `.row` elements, each `.row` its own grid of the same equal columns (the
  inline `gridTemplateColumns` moved from the grid to the rows).
- `src/web/locales/en/box.json` — `grid.hint` and `selection.picked` now name
  the arrow keys, which is what the keyboard actually does.
- Tests: `boxGridModel.test.ts` (10 → 19), `BoxScreen.test.tsx` (25 → 33).

**Decisions:**

- **A step goes to the nearest declared position, and `null` is a normal
  result.** Not the next entry of `cells` (a hole for `C3`, `B4`, `B5`), not
  `col ± 1` (not a position for those three), not the next entry of `positions`
  (`A5` → `B1` — a row down as well as along). Where no step exists, focus stays
  put. **No wrapping**: the APG's layout-grid example wraps for a uniform grid,
  but wrapping past `C3` of the mixed template has to invent a rule for "the
  first cell of the next row" on a row that ends three columns early.
- **The interior-hole fixture, and why it exists.** `boxGridModel.test.ts`'s
  `holedBoxType()` is a fixture, not a shipped template: it declares **columns 0
  and 2 only** — `A1`(0,0), `A3`(0,2), `B1`(1,0), `B3`(1,2). Its rectangle is
  2×3, so the whole of column 1 — `(0,1)` and `(1,1)` — is a hole that sits
  **inside** a row rather than at the end of one. `A1`'s neighbour to the right
  is therefore `A3`.
  The reason it had to be added: **on all four shipped templates a
  null-checked rectangle step agrees with the correct rule on every case this
  repo tests.** Both holes of the mixed Eppendorf template sit at the *end* of
  row C (columns 3 and 4 of a three-position row), where "one column further
  along, and give up if it is not a position" returns exactly the `null` the
  right rule returns. The lead's warning — the mixed template is where a
  rectangle assumption breaks — is correct for **grid rendering** (G3.4's
  uniform-rectangle plant invented `C4`/`C5` and reddened the mixed tests), but
  the shipped mixed holes do not discriminate **movement**. The interior-hole
  fixture is what carries that guard; the demonstration is plant 1 under
  *Guards proven able to fail* below.
- **`Home`/`End` are the row's ends; `Control+Home`/`Control+End` are the grid's
  ends.** The last declared position of the mixed template is `C3` — not `C5`,
  which is a hole, and not `B5`, which is a row up. `positions` is in reading
  order, so "last" is `C3` and the screen reader's "bottom right" is the bottom
  right of the map as declared.
- **Free positions are `aria-disabled`, not `disabled`.** A disabled button
  cannot hold focus at all, and the point of the roving tabindex is that an
  arrow key can stand on a free position so `Enter` can put a sample down there.
  `aria-disabled` says the same thing to a screen reader and still lets focus
  in. The visual consequences: a free cell now receives click events (with
  nothing in hand, `activate()` does nothing, so the behaviour is unchanged),
  and the `cursor: default` that used to hang off `.free:disabled` — a selector
  that can never match again — now keys off `.free[aria-disabled="true"]`, so
  the inert cell still does not offer a pointer.
- **Rows are real elements.** `role="row"` needs one (a gridcell must be owned
  by a row, and axe checks it), so each row is its own CSS grid. The columns
  still line up because every row declares the same number of equal columns and
  the same gap. `display: contents` was rejected: it keeps the flat CSS grid but
  relies on the a11y tree keeping a box-less element, which is the kind of thing
  that regresses per browser.
- **The gridcell wrapper carries `tabIndex={-1}`.** `jsx-a11y`'s
  `interactive-supports-focus` treats `gridcell` as an interactive role and wants
  it focusable. Nothing calls `focus()` on the wrapper — the button inside is the
  focus target — but the attribute costs nothing and keeps the lint honest.
- **Arrow keys are swallowed even when the step does not exist.** Otherwise the
  key scrolls the page out from under the cell that has focus.
- **`Enter` is still not handled in `onKeyDown`.** A browser turns Enter on a
  focused button into a click, so handling both would move twice; Space's default
  is still suppressed, which is what stops it becoming a second click. The new
  tests drive the keyboard with `userEvent.keyboard`, which models that.
- **The tab stop follows focus, whichever way focus arrived.** `onFocus` sets the
  roving label, so a mouse click moves the tab stop too and `Tab` away/back
  returns to the cell the user was last on. Before anything has focus, the tab
  stop is the first declared position.
- **Nothing about the mouse path changed.** Same `payload`, same `onTarget`, the
  same single `sample/move` call site in `BoxScreen.tsx`; the drag and drop tests
  from G3.4 are untouched and still pass.

**Tests:** red first, then green. New model tests exercise the rule against the
**shipped** D4.2 templates (the file runs in the node environment for exactly
that reason), plus the interior-hole fixture; new screen tests drive the real
screen with `userEvent`.

```
# red, before the implementation
$ cd src/web && NODE_ENV=test npx vitest run src/features/box/boxGridModel.test.ts
 Tests  8 failed | 10 passed (18)
$ NODE_ENV=test npx vitest run src/features/box/BoxScreen.test.tsx
 Tests  7 failed | 26 passed (33)   # 7 of the 8 new tests; the stylesheet guard already passed

# green, final commit
$ NODE_ENV=test npx vitest run src/features/box/
 Test Files  3 passed (3)
      Tests  57 passed (57)        # 19 model + 5 moveFailure + 33 screen

$ env -u NODE_ENV npm run check
 Test Files  44 passed (44)
      Tests  708 passed (708)
check-bundle-size: initial JS 162.4 KiB gzipped, budget 250 KiB
exit 0

$ NODE_ENV=production npm run check
 Test Files  44 passed (44)
      Tests  708 passed (708)
check-bundle-size: initial JS 162.4 KiB gzipped, budget 250 KiB
exit 0
```

Both `npm run check` runs are on the final commit; lint reports **0 errors** and
the one pre-existing warning in `src/ui/Table.tsx`. No C++ file is touched, so
`ctest` is not this change's gate.

**One earlier full-suite run failed 1 test under load** — the same run had five
vitest fork workers time out at startup (`gen.test.ts`, `primitives.test.tsx`,
`exportSamples.test.ts`), the machine was carrying two other agents' builds, and
the harness truncated the log, so **the failing test could not be named**. The
isolated re-runs above (708/708, twice) are the evidence; it is recorded here
rather than hidden.

**Guards proven able to fail** (each plant applied to the green tree, run, then
reverted; `git status` clean afterwards):

1. **A rectangle step** — `cellInDirection` rewritten as
   `cellAt(row, col ± 1)` / `cellAt(row ± 1, col)`, the implementation a reader
   would write from "the cell to the right". Result: **1 test red — the
   interior-hole fixture test**, and it is the *only* one. The 9×9, 10×10,
   96-well and mixed-template tests all stayed green, including the screen's
   key-press count and the whole mixed-template walk. This is the demonstration
   the fixture exists for: without it, a rectangle implementation passes the
   suite.
2. **Reading order** — the step rewritten as `cells[flatIndex ± 1]`: **9 tests
   red** (6 model, 3 screen), including `A5` → `B1` on `ArrowRight`
   (`does not read "right" as "the next position in reading order"`), the mixed
   walk, and the 20-press move.
3. **No arrow-key movement** (the pre-#92 keyboard, everything else in place):
   **4 screen tests red**, including the key-press count, which is the criterion
   this issue is decided by.
4. **Every cell `tabIndex={0}`** (the pre-#92 tab order, arrow keys kept):
   **1 test red**, `is one tab stop, entered at the cell the keyboard was last
   on`.

**Known limitations / follow-ups:**

- **No wrap-around at a row's end.** Deliberate (see Decisions). If the APG's
  wrapping layout-grid behaviour is wanted, it is a new decision with a rule for
  ragged rows, not a bug fix.
- **`aria-rowindex`/`aria-colindex` are ordinals**, not the template's own
  `row`/`col`: `A4` of the shipped mixed template (row 0, column 3) is announced
  as row 1, column 4. That is the ARIA convention, and this repo deliberately has
  both a 0-based (seed files) and a 1-based (test fake) numbering — the model
  never derives a label from either, and the announcement does not either.
- **The first tab stop is the first declared position, not the first occupied
  one.** On a box whose only sample is at `H12`, `Tab` lands on `A1` and the
  sample is one `Control+End` away. Worth revisiting only if that turns out to
  be the common case.
- **jsdom applies no stylesheet**, so "focus is visible" is guarded by a
  stylesheet assertion (`\.position:focus-visible { outline: … }`) plus axe, not
  by a rendered ring. A browser pass over the arrow keys is cheap and worth doing
  before anyone relies on the grid for a large box.
- **`sample/move` still cannot express "unplace"** — unchanged from G3.4's
  handoff, and still a `lock:proto` task if it is ever wanted.
