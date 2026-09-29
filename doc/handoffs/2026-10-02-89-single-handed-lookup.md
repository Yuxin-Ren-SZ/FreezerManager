# Handoff note — 2026-10-02, the single-handed lookup (G3.5, worker-3)

The bench's most common flow (PRD §9): scan or type, press Enter, read the
location, be ready for the next scan before the user is. Branch
`feat/89-lookup`, PR **#90**, issue **#89**.

The lookup itself is the easy half. What this task is really about is the
*loop*, because every part of it fails silently — a screen that resolves every
individual scan correctly can still be unusable for the person doing the second
one. Three properties were designed for rather than bolted on, and each has a
test that was watched failing against a planted violation:

1. **The field is the only thing that ever holds the focus**, and it comes back
   focused with its text selected after every lookup — so the next scan
   overwrites it instead of appending to it.
2. **Nothing is debounced and nothing searches while typing.** The term is read
   from the DOM at submit time. A scanner types a whole barcode faster than a
   human types one character, and a search-as-you-type debounce is exactly what
   swallows that.
3. **The exact barcode is tried first, alone.** An exact scan must not be
   diluted by a free-text name search that happens to contain the same
   characters.

**Changed:**

- `src/web/src/features/lookup/lookupSearch.ts` (new) — the search contract as a
  value: barcode probe, then the G0.4 `query` fallback **only on an empty
  answer**, the two-byte minimum handled client-side, one page of
  `LOOKUP_PAGE_SIZE = 25` with `hasMore`, and `lookupKeys` nested under
  `sampleKeys.all(labId)`.
- `src/web/src/features/lookup/LookupScreen.tsx` (rewritten from the G1.3
  placeholder) and `LookupScreen.module.css` (new) — the screen.
- `src/web/src/features/lookup/lookupSearch.test.ts` (10 tests) and
  `LookupScreen.test.tsx` (17 tests) (new).
- `src/web/locales/en/lookup.json` — the screen's copy (the G1.3 `placeholder`
  key is gone with the placeholder).
- `src/web/src/app/App.test.tsx` — the `lookup` row becomes `task: null`, so the
  route test now asserts the placeholder sentence is gone. **One line, no
  reformat**, since G3.4 was editing the same table in the same wave.

`src/app/route-map.tsx`, `src/test/fakeApi.ts`, `package.json` and
`package-lock.json` are untouched: the route, nav entry and permission gate were
already registered by G1.3, and G0.4's `query` / `barcode` filters were already
in the fake.

**Decisions:**

- **The barcode probe is never skipped, and a failure is never a fallback
  trigger.** Falling back on an error would turn "the server is unreachable"
  into "this barcode does not exist" — the worst possible answer at a freezer.
  `searchSamples` therefore rejects, and the screen renders the error state with
  a retry, which still hands the field back focused.
- **A term shorter than two bytes is a miss the screen explains**, not a request
  `SampleServiceImpl::ListSamples` answers `INVALID_ARGUMENT` for. The rule and
  the constant are G3.2's (`sampleFilters.ts`), not a second copy.
- **Five answers, not one empty list.** One hit → the large card; several → the
  pick list; no match; too short; **unplaced** (`{placed:false}`) and **location
  unavailable** (a named box that is not in the layout). The last two are
  G3.2's `placement.ts` line, reused rather than re-derived — "never placed" and
  "the box is missing" are different things.
- **While the layout is still loading, the card says "Resolving the location…"**
  rather than "Location unavailable". G3.1's `locationPath` sees an empty layout
  and returns `partial: true`, which is indistinguishable from a genuinely
  missing box; without this the card would briefly accuse a placed sample of
  having no location. The test for the missing-box case can only pass once the
  layout has settled, which is what makes it non-vacuous.
- **The pick list is driven from the field**: `role="combobox"` on the input,
  `role="listbox"`/`option` below it, `aria-activedescendant` for the highlight,
  ArrowUp/ArrowDown/Home/End to move and Enter to take. The mouse path hangs off
  `onMouseDown` with `preventDefault`, so a click cannot move the caret out of
  the field (`jsx-a11y/click-events-have-key-events` cannot see the input's
  keyboard handling, and the comment on the handler says why it is shaped that
  way).
- **`autoFocus` alone was not enough, and was removed.** The lab id arrives with
  the session, so the field renders **disabled** first — and a disabled input
  silently drops the focus. Focus now follows the field becoming usable (an
  effect on `labId`). `jsx-a11y/no-autofocus` rejects the attribute anyway, so
  the effect is both the correct and the lint-clean answer. The screen test is
  what caught this: it asserts the field ends up focused, not that a prop is
  present.
- **The check-out button is gated on `sample.checkout`** (UX only, G-arch 8) and
  enabled only for an active sample, which is the server's rule. On success the
  card shows the new status without a second lookup, because the lookup query key
  nests under the prefix `useCheckoutSample` invalidates.
- **No new dependency and no shared-file edit beyond the one App table row.**

**Tests:** `lookupSearch.test.ts` (transport-level: request order, request count,
bodies) and `LookupScreen.test.tsx` (the loop and the five answers).

Red first: both files were written and run before the implementation existed
(`Test Files 2 failed (2)`, `Tests 17 failed (17)` — the unit file failed to
import). Green after, focused:

```sh
$ npm run test -- src/features/lookup
 ✓ src/features/lookup/lookupSearch.test.ts (10 tests) 64ms
 ✓ src/features/lookup/LookupScreen.test.tsx (17 tests) 472ms
 Test Files  2 passed (2)
      Tests  27 passed (27)
```

**Planted violations — each applied to the working tree, observed red, reverted:**

1. Removed `input.select()` and kept `input.focus()`:

   ```
   FAIL  LookupScreen.test.tsx > hands the field back focused with its text selected after every lookup
   AssertionError: expected 9 to be +0 // Object.is equality
   Tests  1 failed | 26 passed (27)
   ```

   `selectionStart` stays at the end of the text, so the next scan would append
   to the previous barcode instead of overwriting it. This is the criterion the
   issue calls invisible when it breaks.

2. A human-tuned 300 ms debounce whose value `submit()` reads instead of the DOM
   value (the mutation a "search as you type" refactor would introduce):

   ```
   × is not swallowed by a scanner burst, and searches the whole barcode exactly once
   TestingLibraryElementError: Unable to find role="region" and name "Serum A"
   Tests  14 failed | 3 passed (17)      # screen file
   ```

   The burst's Enter searched the stale copy — nothing was found at all.

3. Free text searched before the exact barcode:

   ```
   × answers an exact barcode with one request and never a free-text search
   AssertionError: expected 'query' to be 'barcode'
   × is not swallowed by a scanner burst…
   AssertionError: expected { lab_id: 'lab-demo', …(2) } to match object { barcode: 'DEMO-0001' }
   Tests  4 failed | 23 passed (27)
   ```

   The second failure is the request body: the scanner's request carried
   `query`, i.e. the search answered a name match for a scan.

Green on the final tree:

```sh
$ env -u NODE_ENV npm run check          # exit 0
check-routes: ok — 70 unary routes and 2 SSE routes agree between RestGateway.cc, routes.ts and sse.ts
✖ 1 problem (0 errors, 1 warning)        # pre-existing, src/ui/Table.tsx react-hooks/incompatible-library
 Test Files  36 passed (36)
      Tests  546 passed (546)
check-bundle-size: initial JS 159.5 KiB gzipped, budget 250 KiB
✓ built in 314ms

$ NODE_ENV=production npm run check      # exit 0
 Test Files  36 passed (36)
      Tests  546 passed (546)
check-bundle-size: initial JS 159.5 KiB gzipped, budget 250 KiB
```

(The bundle guard reports `initial JS 159.5 KiB gzipped, budget 250 KiB` on this
tree, and the screen is behind `import()` like every other one —
`LookupScreen-*.js` is built as its own 7.85 kB / 3.00 kB gzipped chunk. No claim
is made here about how much of the 159.5 KiB is this change: the last number
recorded on the board (#67) predates G3.3, and guard 2 above
(`scripts/check-bundle-size.mjs`) is the mechanical statement that matters — no
screen is back in the entry chunk.)

No C++ file is touched, so `ctest` is not this change's gate; CI's `web` job runs
the same `npm ci && npm run check`.

**Known limitations / follow-ups:**

- **A barcode search is exact and case-sensitive**, because the server's
  `barcode` filter is. A scan in the wrong case falls through to the
  case-insensitive free-text search and still finds the sample — as a `query`
  hit, which is why the card does not claim which filter matched.
- **A very broad name search shows the first 25 matches** and says so. Paging the
  pick list (or a "keep typing" nudge) is G3.2's browser territory; a lookup that
  matched 400 samples is a search problem, not a scan.
- **`lookup?q=` (the shell's global lookup box) is not yet consumed by this
  screen.** `GlobalLookup` navigates to `/lookup?q=…`, and G1.3 registered the
  parameter for the single-handed flow; wiring it would mean this screen reading
  `useSearchParams` and seeding the field and the search from it. It is one
  behaviour on top of a tested search and it belongs to whoever owns the shell
  flow (the parameter is already in the URL, so nothing is lost meanwhile).
- **No check-in from the card.** The card offers the one action the issue names
  (check out); a checked-out sample shows a disabled button with the reason, and
  the sample's own detail screen (linked from the card's title) has the full
  lifecycle. Scanning to check *in* is G3.6's focus mode.
