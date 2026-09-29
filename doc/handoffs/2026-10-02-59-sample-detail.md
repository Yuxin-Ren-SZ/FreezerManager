# Handoff note — 2026-10-02, G3.3 sample detail, create and edit (#59, worker-1)

Everything a person does to one sample: read every field, create one, edit one,
and run the five lifecycle actions. This is where the custom-field system
becomes visible — the form is **generated** from the item type's *inherited*
definitions, and the server, not the client, has the last word on validity.
Spec: PRD §9 (flows), F6.4/F6.5, TODO.md §Section G → G3.3. Branch
`feat/59-sample-detail`, PR **#66**.

**Changed:**

- `src/web/src/features/sample-detail/customFields.ts` — the two things ported
  from C++: `resolveInheritedDefinitions` (mirrors
  `storage::resolve_custom_field_defs` — lab-global + ancestor chain, most-derived
  definition wins on a duplicate key, cycle-guarded) and
  `validateCustomFieldValues` / `parseCustomFieldValues` /
  `serializeCustomFieldValues` (mirror `core::validate_custom_fields` rule for
  rule, all eight `FieldDataType`s, including the C++'s **UTF-8 byte** length
  check and its "an empty string is a present value" reading of `required`).
  Validation messages come back as i18n keys, so the same key is used by the
  mirror and by a server rejection.
- `src/web/src/features/sample-detail/serverErrors.ts` — `mapServerFailure`:
  where a rejection lands. Parses the server's
  `custom field validation failed: [key: message] …` groups, the core-field
  messages both backends share (`container_type size_class is not accepted at
  this box position`, `position_label does not exist in this box's BoxType`,
  `box_id …`), and `ALREADY_EXISTS` as the `samples_position_unique` index. The
  server's English text is never shown (G-arch 7); each recognised rule maps to
  the same key the mirror uses.
- `src/web/src/features/sample-detail/SampleForm.tsx` — the generated
  create/edit form: one control per data type, the core fields, the box/position
  picker (free positions only, computed from the destination box's own samples),
  the PHI warning described below, and the `server-field-errors` list for a
  rejection that names a field the form could not render.
- `src/web/src/features/sample-detail/SampleDetailScreen.tsx` (+ `.module.css`)
  — every field, custom fields per definition type, the parent link with the
  parent's status, the location path from G3.1's `resolveLocationPath`, the
  `audit.read`-gated history, and edit mode in place.
- `src/web/src/features/sample-detail/SampleActions.tsx` — check out / check in
  (volume used + reason) / discard (reason; the server consumes the remainder) /
  move (destination box + free positions) / soft delete behind a confirmation.
- `src/web/src/features/sample-detail/SampleCreateScreen.tsx`,
  `useSampleReferenceData.ts` — the create route, and the shared reference-data
  composition (item types, definitions, container types, layout).
- `src/web/src/api/hooks/{containers,audit}.ts` — the two reads G1.2 did not
  provide. Item types and custom-field definitions are **G3.2's** hooks in
  `labs.ts` (see Decisions), so this branch adds no third copy.
- `src/app/route-map.tsx` — **only** the two `sample-new` / `sample-detail`
  entries' imports. The route objects, their `task: 'G3.3'` and their
  permissions are unchanged, and nothing else in the array was touched, so
  G3.2's edit to the `samples` entry rebases as a one-liner.
- `src/app/i18n.ts`, `locales/en/sample-detail.json` — the namespace, keyed
  `'sample-detail'` to match the feature directory.
- `src/app/App.test.tsx` — the two `EXPECTED_SCREEN` rows for my routes
  (`task: null`, the real headings).
- `src/test/fakeApi.ts`, `src/test/session.ts` — see Decisions. G3.2 had
  already extended the same file, so the rebase merged its `customFieldDefs`
  list and unpaged list resolvers with this slice's `auditEvents`, its real
  enforcement in `sample/create|update|move`, and its two new routes.

**Decisions:**

- **Inheritance lives in the client, and the form asks for the whole lab's
  definitions.** `ListCustomFieldDefinitions` with an `item_type_id` returns that
  node's definitions *only* — no ancestor merge. A leaf-only form looks perfectly
  correct against an item type with no parent, which is why the fake seeds a
  Blood → {Serum, Plasma, Tissue} chain and a key (`notes`) defined on both Blood
  and Serum with different `max_length`s.
- **The fake validates too.** `sample/create`, `sample/update` and `sample/move`
  now enforce the inherited custom-field rules with the server's real message
  wording, the `(box_id, position_label)` unique index as `ALREADY_EXISTS`, and
  the size-class rule as `INVALID_ARGUMENT`. A screen test therefore meets a
  rejection the server would really return. The fake resolves inheritance with
  **its own** copy of the rule, not the screen's, so an inheritance bug cannot
  make both sides agree.
- **`sample/checkout`'s illegal-transition code stays `FAILED_PRECONDITION`**,
  which G1.2's `fakeApi.test.ts` and `hooks/samples.test.tsx` pin, even though
  the C++ `ConstraintViolation` maps to `INVALID_ARGUMENT`. Changing another
  task's pinned contract was not this issue's call; reported here instead. The
  state machine it now implements (check-in subtracts volume and auto-depletes
  at zero, discard consumes the rest, both append the audit row) is faithful.
- **The seed did not grow.** `sample/list` counts are asserted by G1.2 and by
  G3.2's in-flight work, so the demo lab kept its four samples; a test that needs
  a depleted parent mutates its own copy.
- **Discard takes a reason, not a volume.** `storage::apply_checkout` ignores
  `volume_used` for `Destroyed` and consumes whatever is left, so the dialog says
  so rather than offering an input the server discards.
- **G3.2 (#55) merged while #59 was paused, and it had already built three of
  the things this branch added.** The rebase preferred G3.2's version
  everywhere it existed: `useItemTypes` / `useCustomFieldDefinitions` (theirs,
  in `labs.ts`, with the `custom_field.define` gate as an option), the
  `DemoLab` definitions field name (`customFieldDefs`), and the unpaged list
  resolvers. My duplicate `concentration` definition was dropped — G3.2's is
  the same key, label and data type on `it-serum`. `audit/list` is the one
  route of mine that pages, because `AuditServiceImpl` really does set
  `next_page_token` while `ItemTypeServiceImpl` and `BoxServiceImpl` ignore
  `page`. Two of G3.2's assertions were relaxed with the shared seed
  (`labs.test.tsx` now uses `arrayContaining` plus a lab-scoping check;
  `SampleBrowserScreen.test.tsx` scopes a cell lookup to Serum A's row, since
  two samples now hold a `3`), and both were exact expectations over fixture
  data another task grows. The route map took no conflict.
- **Two findings, both on #59 and both needing a lead decision** — the
  `custom_field.define` gap (a Member cannot read the definitions the form needs;
  interim behaviour is a visible notice, not a silent omission) and the
  `UpdateSample` PHI-envelope replacement (an edit by a caller without `phi.read`
  clears PHI; the form warns first, and the server-side fix is a separate task).

**Tests:**

- `customFields.test.ts` (38), `serverErrors.test.ts` (18),
  `SampleForm.test.tsx` (18), `SampleDetailScreen.test.tsx` (28),
  `api/hooks/reference.test.tsx` (4 — the two routes this slice added; the
  item-type and definition assertions are G3.2's `labs.test.tsx`).
- `npm run check` → exit 0 with `NODE_ENV=production` exported, and exit 0 with
  it unset: `gen` + `check:routes` + `lint` + `typecheck` + `format:check` +
  `test` (**510/510 in 32 files**, on `origin/main` as of G3.2's merge) +
  `build`, identical artifact hash `013fe217ab1e58055b00740742c54c5b` both
  times.
- **Guards proven able to fail** by planting six violations and watching the
  relevant test go red: leaf-only inheritance (7 tests), a dropped `max_length`
  rule (2), an unattributed size-class rejection (2), a history section ignoring
  `audit.read` (1), an ungated audit query (1), and a PHI row rendered from the
  definition alone (1). Two of those plants exposed guards that **could not**
  fail — a PHI assertion that ran before the definitions loaded, and a
  "no request" assertion that relied on MSW's `onUnhandledRequest: 'error'`,
  which only logs. Both were rewritten and now fail when planted; that fix is
  its own commit.
- Web-only: `git diff --name-only origin/main...HEAD` is `src/web/**` only, so
  the C++ matrix cannot be affected. `ctest --preset dev` was **not** run — it
  cannot exercise any of this, and the machine was at load 17 from the other
  agents' builds.

**Known limitations / follow-ups:**

- The form's `parent_sample_id` is a text input for a UUID, not a picker: a
  sample search is G3.2's browser / G3.5's lookup, not this screen.
- Custom fields are read-only outside the generated form; the definitions
  themselves are G3.9's admin screen.
- The item type is fixed once a sample exists (the form disables the select):
  changing it would re-generate the field list under values that no longer belong
  to it, and the server keys PHI partitioning off the type.
- `features/samples/{SampleCreateScreen,SampleDetailScreen}.tsx` are now
  unreferenced G1.3 placeholders. They are in G3.2's directory, so they were left
  alone; whoever touches `features/samples/` next can delete them.
- **Initial JS is now 244.7 / 250 KiB gzipped — 5.3 KiB of headroom.** It was
  207.2 before G3.2 (#55) and this slice landed, and both added screens the
  route map imports statically. `npm run check` passes, but the next web task
  that adds a screen or a dependency will very likely fail the build. I did not
  measure the per-task split, and making it lazy is not this issue's change to
  make: `AppRoute.element` is a `ReactNode`, so it needs a `React.lazy` +
  `Suspense` boundary in `router.tsx`/`route-map.tsx` — the same file G3.2 had
  just edited. Worth a lead-filed task before G3.4/G3.5/G3.6 land; lazy
  `import()` for feature screens is the lever `doc/dev/web.md` already names.
