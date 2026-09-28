# Handoff note — 2026-09-27, G0.4 name/barcode search on ListSamples (#31, worker-2)

Server-side lookup by partial name or barcode (PRD §9 lookup flow, §6 REST
gateway). `ListSamples` could previously filter only by *exact* barcode, so the
Qt client scanned the whole lab client-side (the FIXME in
`SampleLookupWidget.cc`) — impossible for a browser at 100k rows. This slice
adds a paginated, case-insensitive substring search both clients can use.

**Changed:**

- `proto/fmgr/v1/sample.proto`: `ListSamplesRequest.query = 8` (additive,
  `lock:proto`; no new package version, no migration — next free version stays
  15).
- `src/storage/IStorageBackend.h`: `PredicateOperator::ContainsCi`,
  `Predicate::fields`, and the DSL factories `contains_ci(field, needle)` /
  `contains_ci_any({fields...}, needle)` (plus the fluent
  `FieldRef::contains_ci`).
- `src/storage/detail/QuerySqlBuilder.h`: `SqlDialect::contains_ci` implemented
  as SQLite `LIKE ? ESCAPE '\'` and PostgreSQL `ILIKE $N ESCAPE '\'`, sharing
  the new `detail::like_contains_pattern` escaper; `append_where` ORs the
  alternatives and parenthesizes them.
- `src/storage/{sqlite,postgres}/AuditRepositories.cc`: the new operator joins
  the explicit "unsupported audit query operator" case list.
- `src/server/SampleServiceImpl.cc`: `kMinQueryLength = 2`, validation and the
  `contains_ci_any({Name, Barcode})` predicate.

**Decisions:**

- The DSL has no OR combinator, and adding a general one is a much bigger change
  than this slice needs. `contains_ci_any` therefore carries the field list in
  the predicate (`Predicate::fields`) and the dialected renderer ORs it. If a
  future task needs arbitrary boolean queries, it should introduce a real clause
  tree and re-express this predicate on top of it, not extend `fields`.
- Escaping lives in one shared helper used by both dialects *and* by the SQLite
  and PostgreSQL conformance backends, which hand-roll their SQL translators.
  The two conformance translators still spell out `LIKE`/`ILIKE` themselves, the
  same way they already duplicate the other operators.
- Case folding is whatever the store does: SQLite's default `LIKE` folds ASCII
  only (no ICU in the pinned build), PostgreSQL `ILIKE` follows the column
  collation. The tests therefore only assert ASCII case-insensitivity and
  byte-exact non-ASCII matches; do not tighten the docs to promise
  Unicode-aware folding without adding ICU.
- The length check runs *after* `authorize()`, matching the comment already in
  `ListSamples`: an unauthenticated caller must not learn whether its payload
  would have been acceptable.
- `custom_fields_json` and `phi_fields_enc_json` are simply not in the searched
  field list — there is no filtering step to get wrong. L10 can replace the
  implementation behind the same proto field later.

**Tests:** `tests/backend_conformance/{storage,sqlite,postgres}_backend_conformance_test.cpp`
(three new cases each: case-insensitive substring, OR over listed fields,
wildcard/escape/non-ASCII literals), `tests/integration/sample_service_integration_test.cpp`
(six cases: name, barcode, AND with another filter + pagination,
`INVALID_ARGUMENT` under two characters, wildcard escaping, custom fields and
PHI never searched), `tests/integration/rest_gateway_integration_test.cpp`
(`ListSamplesQueryFiltersByNameAndBarcodeThroughRest`, end to end over HTTP).

- `cmake --build --preset dev` on the red commit `39497e5` → 18 expected
  compile errors; after the implementation commits, build is clean.
- `ctest --preset dev -R 'ContainsCi|QueryDsl'` → 8 passed, 4 skipped (Postgres:
  no `FMGR_TEST_POSTGRES_URL` on this machine).
- `ctest --preset dev -R 'ListSamplesQuery|QueryFiltersByName'` → 7/7 passed.
- `ctest --preset dev -R 'SampleServiceTest.ListSamples|RestGatewayTest'` →
  37/37 passed.
- Full suite, asan and ubsan: see PR #38.

**Known limitations / follow-ups:**

- The Qt client still does its own client-side filtering; switching
  `SampleLookupWidget` to `query` is deliberately out of scope here and keeps
  its FIXME.
- No ranking or fuzzy matching (out of scope per the issue); substring order is
  whatever the backend returns.
- PostgreSQL conformance runs only with `FMGR_TEST_POSTGRES_URL` set, so on this
  machine the `ILIKE` branch is compile-checked but not executed.
- Local sanitizer runs need a manual `CMakeUserPresets.json` fix (the generated
  file includes both `out/conan/dev` and the new folder, and both define
  `conan-debug`); see PR #38.
