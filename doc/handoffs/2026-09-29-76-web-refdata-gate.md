# Handoff note — 2026-09-29, [web] the SPA still gated reference data on *.define (#76, worker-1)

The client half of #69. `useSampleReferenceData` and `SampleBrowserScreen` closed
their `custom-field-def/list` queries on `useCan('custom_field.define')`, but the
server's `ListCustomFieldDefinitions` had moved to `sample.read` — the permission
a lab `Member` holds, and `custom_field.define` they do not. The SPA was therefore
refusing a request the server would have answered, and every generated sample form
rendered with no custom fields and no explanation, because the screens degrade
gracefully. This is the same class of bug as #54, one layer up.

**Changed:** `src/web/**` only, two commits — red (`ad41bb3`), green (`a8c800e`).

- `src/web/src/features/sample-detail/useSampleReferenceData.ts` — the gate
  (`:52`) moves to `useCan('sample.read', labId)`, and the module comment is
  rewritten (see Decisions 1).
- `src/web/src/features/samples/SampleBrowserScreen.tsx` — the gate (`:95`) moves
  to `sample.read`; the `canDefineFields` local is renamed `canReadSamples`, since
  the old name asserted the thing that was wrong.
- `src/web/src/features/sample-detail/useSampleReferenceData.test.tsx` — **new**,
  the two gate tests.
- `src/web/src/features/samples/SampleBrowserScreen.test.tsx` — the
  `READ_ONLY` fixture's premise inverted, plus a `WITHOUT_SAMPLE_READ` guard.
- `src/web/src/api/hooks/labs.ts`, `src/web/src/test/fakeApi.ts`,
  `src/web/src/api/hooks/labs.test.tsx`, `src/web/src/features/samples/sampleColumns.test.tsx`,
  `src/web/src/test/session.ts` — the same false claim corrected in comments.
- `src/web/src/features/sample-detail/SampleForm.test.tsx` — one test adapted.
- `src/web/locales/en/sample-detail.json` — `customFieldsUnavailable` named the
  wrong permission **to the user**.

**Decisions:**

1. **The comment was the deliverable, not a tidy-up.** `useSampleReferenceData.ts`
   stated as a fact about the server that `ListCustomFieldDefinitions` authorizes
   on `custom_field.define`. After #69 that is false, and a future reader who
   trusted it would "fix" the gate *back* to matching it. It now names
   `sample.read`, the enforcement point
   (`ItemTypeServiceImpl::ListCustomFieldDefinitions` in
   `src/server/ItemTypeServiceImpl.cc` — the `middleware_.authorize(...,
   core::Permission::SampleRead, lab_id)` call), notes the `AuthMiddleware`
   registry entry agrees with it, records that only the *read* moved
   (Create/Update/Archive still require `custom_field.define`), and says outright
   that the old statement was true before #69 and must not be restored. Anchors
   are given by file and function rather than line number, because #70's diff
   moves them.
2. **There was no `item_type.define` gate to fix.** The issue names two gates,
   `useCan('custom_field.define')` / `useCan('item_type.define')`. `useItemTypes`
   was never gated in either screen, and the single `item_type.define` in
   `route-map.tsx:253` guards the item-types **admin** screen (G3.9), where
   defining still needs it. Both real gates were `custom_field.define`.
3. **The negative direction is asserted, not assumed.** Both new tests use a
   caller holding `custom_field.define` but *not* `sample.read`, so they fail
   under the old gate too — that is what makes them gate tests rather than
   tautologies — and they pin that the fix is not "everyone sees everything".
   `fakeApi` models no permissions, so an empty `cfds` list can only mean the
   request was never sent: had it been sent, the fake would have answered.
4. **`SampleForm.test.tsx`'s "server rejection on a field the form could not
   render" is adapted, not weakened.** It used `['sample.read', 'sample.write']`
   and relied on the old gate to leave the form without a `tissue_grade` control;
   under the new gate that user is served the definitions. Injecting a
   `custom-field-def/list` failure is **not** an alternative:
   `SampleCreateScreen.tsx:40` turns `reference.isError` into an `ErrorState` and
   the form never renders, so `definitionsReadable` is the only path to "the form
   rendered no custom fields". The test now uses a caller holding `sample.write`
   without `sample.read`; its assertions are unchanged.
5. **`customFieldsUnavailable` now names `sample.read`.** The copy previously told
   the user that reading field definitions needs `custom_field.define`. It keeps
   its shape and names the permission the gate actually checks, rather than
   dropping the permission name — the string is only reachable for a caller
   without `sample.read`, which no built-in role produces, so naming it is the
   only informative thing it can say.

**Dependency:** #70 (`feat/69-item-type-read-permission`, head `be41990`) is
**in review, not merged** — `main` still carries the old server permission.
Verified against that branch rather than assumed: all three read RPCs move to
`SampleRead` with registry entry and enforcement point agreeing
(`ListItemTypes`/`ListCustomFieldDefinitions` via `middleware_.authorize(...,
SampleRead, lab_id)`; `GetItemType` via `sctx.has_for_lab(item_type->lab_id,
SampleRead)` after the row load), and the six mutating paths do not appear in
`git diff main...origin/feat/69-item-type-read-permission` at all. **Until #70
merges, this gate and the deployed server disagree in the other direction** — a
caller with `sample.read` and no `custom_field.define` gets a 403 from `main`'s
server. Both halves should land together; the client tracks the enforcement
point, not the deployed state.

**Tests:** TDD, red committed first. New file
`src/web/src/features/sample-detail/useSampleReferenceData.test.tsx`.

Red — `NODE_ENV=test npx vitest run src/features/sample-detail/useSampleReferenceData.test.tsx src/features/samples/SampleBrowserScreen.test.tsx`
→ exit 1, **4 failed | 28 passed (32)**, all four the new assertions (both
directions of the gate, at both the hook and the screen level). Nothing else
moved.

Green — `npm run check` (gen + check:routes + lint + typecheck + format:check +
test + build), run three times because this shell has exported
`NODE_ENV=production` before and silently changed results:

| shell | exit | tests | initial JS |
|---|---|---|---|
| `NODE_ENV=development` | 0 | 516 passed (34 files) | 158.6 KiB gzip |
| `NODE_ENV=production` | 0 | 516 passed (34 files) | 158.6 KiB gzip |
| `NODE_ENV` unset | 0 | 516 passed (34 files) | 158.6 KiB gzip |

Identical build artifact `assets/index-B61EA7XL.js` in all three, so the result
does not depend on the shell's `NODE_ENV`. eslint reports `0 errors, 1 warning`;
the single warning is pre-existing and present before this diff.

**Known limitations / follow-ups:**

- **One loaded-run failure, kept on the record.** The first
  `NODE_ENV=production` run exited 1 with five
  `[vitest-pool]: Failed to start forks worker … Timeout waiting for worker to
  respond` errors and 29/34 files collected — **no assertion failure**. Load
  average was 13.95 with five other agent worktrees active; the immediate re-run
  at load 14.16 was clean at 516/516. This is the oversubscription pattern the
  board already records (#79's 3618 s run, worker-2's lost test name), not a
  regression here, and it is written down rather than deleted.
- **A pre-existing React duplicate-key warning** —
  `Encountered two children with the same key, 'field:notes'` — appears in the
  `SampleBrowserScreen` export tests. It reproduces on the red commit (92
  occurrences before this diff) and comes from the fake seeding both
  `cfd-notes-blood` and `cfd-notes-serum` with `key: 'notes'` for the same item
  type. Not caused by, and not fixed in, this slice.
- **The gate is now effectively always open on these screens.** Every built-in
  role that can reach the sample browser or the sample form holds
  `sample.read`, so `definitionsReadable === false` and therefore
  `customFieldsUnavailable` describe a custom role only. The flag and the option
  are kept — the server remains the enforcement point, and a `custom-field-def/list`
  refusal still has to degrade — but the branch is not reachable with the roles
  in `src/core/permissions.h` today.
- **No C++ touched, so no sanitizer run and no `ctest` run**: this is a
  `src/web/**`-only diff and the C++ build graph cannot see it (AGENTS.md §4).
