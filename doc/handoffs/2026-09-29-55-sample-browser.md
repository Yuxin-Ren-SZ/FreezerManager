# Handoff note — 2026-09-29, G3.2 sample browser (#55, worker-2)

The sample browser at `/labs/:labId/samples` (TODO.md G3.2, PRD §9 flows,
F6.2/F6.6/F7): a virtualized TanStack table over `useInfiniteQuery`'s
`page_token` paging, five filters kept in the URL, custom-field columns from the
lab's field definitions, CSV export through `sample/export`, and live updates
from `sample/watch`. It replaces the G1.3 placeholder; the route, its TODO id and
its `sample.read` gate in `src/app/route-map.tsx` are unchanged.

**Changed**

- `src/features/samples/SampleBrowserScreen.tsx` (+ `.module.css`) — the screen:
  filter bar, one of four states (loading / error / empty / table), the row
  count, the export button and the live indicator. `Table` comes from the UI kit
  with `onEndReached`.
- `src/features/samples/sampleFilters.ts` — `SampleFilters`, `parseSampleFilters`
  (URL → value, unknown statuses dropped), `sampleFiltersToSearch` (value → URL,
  defaults omitted), `toListFilters` (value → request/key), `queryTooShort`,
  `sampleStatusFromParam` / `sampleStatusParamValue`.
- `src/api/hooks/sampleLive.ts` — `useSampleLive` plus the exported
  `mergeSampleFrame` / `applySampleFrame`: `sample/watch` frames merged into list
  caches, `sample/get` entries invalidated.
- `src/features/samples/sampleColumns.tsx`, `customFields.ts`, `placement.ts` —
  the column set (one per CFD), `custom_fields_json` parsing/formatting, and the
  three-state placement helper.
- `src/features/samples/exportSamples.ts` — `exportFileName`
  (`samples-<lab>-<date>.csv`, UTC) and `downloadTextFile` (blob URL, revoked).
- `src/api/hooks/samples.ts` — `SampleListFilters` gained `status` and `query`;
  `useExportSamples`. `src/api/hooks/labs.ts` — `useCustomFieldDefinitions`.
- `src/ui/Table.tsx` — optional `onEndReached` / `endReachedThreshold`: fires on
  the transition into "the window covers the end", read through a ref so the
  caller's identity churn cannot re-trigger it.
- `src/test/fakeApi.ts` — faithful paging, `status`/`query` filters,
  `custom-field-def/list` and `sample/export` resolvers, custom-field values on
  the demo samples, `seedSamples(lab, count)`.
- `src/test/render.tsx` — optional `user`; `src/test/setup.ts` — the shared
  `FakeEventSource` installed as the global `EventSource` (jsdom has none).

**Decisions**

- **The `fakeApi` 100-row cap was fixed by making the fake faithful, not by
  raising the cap.** `sample/list` now mirrors `SampleServiceImpl::ListSamples`
  exactly: `page_size = 0` means no limit, a `next_page_token` only after a full
  page (so a client makes one extra request to discover the end, as production
  does), and no `total_count` — no `*ServiceImpl` sets one, so the fake was
  inventing a number a screen could render. The other list routes return
  everything because `LabServiceImpl`/`BoxServiceImpl`/`ItemTypeServiceImpl`
  ignore `page`. Two G1.2 assertions that relied on `total_count` now assert the
  proto default, with the reason in a comment. `sample/list` also ignored
  `status` and `query` before this slice, which would have made the filter tests
  pass for the wrong reason.
- **A watch frame is never a detail view.** Frames merge into list caches; the
  matching `sample/get` key is invalidated, never written. The stream carries no
  PHI, so overwriting a detail would replace a complete record (PHI included,
  when the server disclosed it) with a partial one.
- **Frames are matched against each cached list's own filters**, read back from
  the query key rather than passed in: a row that stops matching (moved box,
  changed status, edited name against a search) leaves that list. A row the
  client has never loaded is appended only when the loaded window reaches the
  end of the sequence — while a token is outstanding, appending would put a row
  inside an offset window it does not belong to and duplicate it when that page
  arrives.
- **The URL is the only filter state**, and a query under two bytes is held back
  (the server answers `INVALID_ARGUMENT`) with a hint instead of an error state.
- **Placement has three states.** `{placed: false, partial: true}` from G3.1's
  resolver reads as "Location unavailable", never as "Not in a box".
- **The row count is what is loaded.** No `total_count` on the wire means no
  total on screen; the VisuallyHidden "Showing N of M rows" from the kit is what
  the 100k test asserts against.
- **Custom-field columns are permission-gated**: `custom-field-def/list` needs
  `custom_field.define`, so `useCan` switches the request off and the screen is
  complete with its base columns.
- **The export is lab-wide**, because `ExportSamplesCsvRequest` carries only
  `lab_id` and `include_archived`; the screen says so next to the button.
- `<lab>` in the file name is the lab **id** (stable, and sanitised because it
  comes from the URL); the date is UTC, matching the CSV's own timestamps.

**Tests**

New: `SampleBrowserScreen.test.tsx` (29), `sampleFilters.test.ts` (15),
`sampleColumns.test.tsx` (5), `customFields.test.ts` (9), `placement.test.ts`
(6), `exportSamples.test.ts` (6), `sampleLive.test.tsx` (15), `labs.test.tsx`
(4). Extended: `fakeApi.test.ts` (27), `samples.test.tsx` (16),
`Table.test.tsx` (14), `App.test.tsx` (44).

```
cd src/web
npm run check        # gen + check:routes + lint + typecheck + format:check + test + build
```
→ exit 0, **405 tests in 27 files**, `check-bundle-size: initial JS 235.4 KiB
gzipped, budget 250 KiB` (was 196.0 before this slice). Run twice: once with
`NODE_ENV=production` exported and once with it unset — same result.

**Prove the guards can fail** — 14 planted violations, each reverted after the
run, each turning its own tests red:

| Planted violation | Tests that went red |
|---|---|
| `fakeApi` caps un-paged lists at 100 again | fakeApi paging contract (1) |
| `sample/list` invents `total_count` | fakeApi + hook page tests (2) |
| a frame overwrites `sample/get` instead of invalidating | detail-invalidation test (1) |
| a tombstoned frame is merged like any other row | live hook + screen tombstone (2) |
| the free-text filter never reaches the URL | filter/URL/search tests (5) |
| a one-character query is sent | two hold-back tests (2) |
| a broken location reads as "never placed" | placement unit + screen (2) |
| the grid stops virtualizing | paging + 100k-row tests (2) |
| custom-field columns dropped from the chooser | columns + screen (5) |
| export file name changed | export unit + screen (4) |
| a failed list renders as an empty lab | both error-state tests (2) |
| the feed drops the box filter | feed-scope tests (2) |
| a frame ignores the list's filters | filter-scope tests (3) |
| the SSE wrapper stops reconnecting | sse + hook + screen reconnect (6) |

The tombstone plant found a **weak assertion** rather than a bug: the screen test
checked that the *edited name* was gone, which a row replaced by a tombstoned
copy also satisfies. It now asserts the row count drops (4 → 3) and that neither
name is present; both tests go red under the plant.

**Known limitations / follow-ups**

- **Export ignores the filters** (server-side RPC limit, above). Giving
  `ExportSamplesCsvRequest` filter fields is a proto change (`lock:proto`) and a
  new issue, not scope creep here.
- **No debounce on the search box**: once past two bytes, each keystroke is a
  request, and the server's `contains_ci_any` scans. Fine against the fake and
  small labs; a 100k-row lab would benefit from a debounce (and from the server
  side of F8/L10). Worth an issue if the lead wants it in G3.
- **A live insert that lands beyond the loaded window is dropped**, not shown —
  deliberate (see decisions). The row appears when the user pages to where it
  lives. Offset paging plus inserts can also shift a page boundary; that is
  inherent to the gateway's offset `page_token`, not to this screen.
- **Bundle headroom is now 14.6 KiB.** `doc/dev/web.md` says table screens are
  expected to be lazy, and I measured it: importing this screen through
  `import()` moved only 3.0 KiB gzipped out of the initial bundle (233.4 vs
  235.4), because `src/ui/index.ts` is reachable from the entry chunk and
  re-exports `Table`, so TanStack Table + Virtual stay in the entry either way.
  I kept the static import (matching G3.1's layout screen) rather than paying a
  spinner flash on every visit for 3 KiB. The remaining G3 screens will eat that
  headroom, so this needs a decision: drop `Table` from the `ui` barrel, split
  the kit by route, or raise the budget.
- `custom-field-def/list` requires `custom_field.define`; a member without it
  gets no custom columns (their sample *values* still arrive with the list, but
  there are no definitions to label or select them with).
