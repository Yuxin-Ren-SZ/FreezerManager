# Handoff note — 2026-10-20, [storage] a malformed `outer_dimensions_json` degrades instead of 500ing (#138, worker-2)

The demo-readiness audit of 2026-09-29 found `POST /api/v1/container-type/list`
returning 500 on a seeded database: `[json.exception.out_of_range.403] key
'width' not found`. The seed wrote `{}` into `outer_dimensions_json`, but the
write path (`dimensions_dump`) stores the text `null`; `parse_dimensions()`
called `json.get<OuterDimensionsMm>()` on any non-null value, and
`core::OuterDimensionsMm::from_json` does `json.at("width")`. Rows created
outside the API were the only ones that could reach this state, which is why CI
never saw it and a seeded demo hit it immediately. The visible symptom on the
sample detail screen was the raw container-type UUID, because the emptying
reference list left the screen with nothing to resolve the name from.

Branch `feat/138-container-dimensions`, PR **#156** (draft at the time of
writing).

**Changed:**

- `src/storage/detail/BoxGeometryColumns.h` — `parse_dimensions()` is a
  tolerant reader. Empty text (how a SQL NULL surfaces on the SQLite read
  path), JSON `null`, a JSON string, `{}`, partial objects and non-numeric
  fields all read back as "no dimensions" — the same state
  `dimensions_dump(nullopt)` writes. Only an object with numeric `width`,
  `height` and `depth` produces `OuterDimensionsMm`. The strict
  `core::OuterDimensionsMm::from_json` stays the validation converter for the
  create path, so `CreateContainerType` still rejects a partial object with
  `INVALID_ARGUMENT`.
- `scripts/seed_demo.py` — `NO_DIMENSIONS_JSON = "null"` replaces the `{}` the
  seed used for container types, so a seeded database and an API-created one
  are indistinguishable.
- `src/web/src/features/sample-detail/SampleDetailScreen.test.tsx` — pins that
  a seeded row's detail screen shows `15 mL tube`, not `ct-15ml`.
- Tests: `tests/unit/box_geometry_columns_test.cpp` (new, 8 cases),
  `tests/integration/box_service_integration_test.cpp`
  (`ListContainerTypesToleratesMalformedOuterDimensionsJson`), and the SPA case
  above.

**Decisions:**

- **"Absent" on the wire is the empty string.** The proto3
  `outer_dimensions_json` field has no presence bit; an empty string is exactly
  what an API-created row with no dimensions already reads back with. No
  migration: this is tolerance, not a schema change.
- **The seed adopts the write path's representation, `"null"`,** rather than
  changing the write path to store `{}`. The API's representation is the one
  that must stay authoritative.
- **Silent degradation is the intended read behavior.** A malformed value is
  data written outside the API; failing the whole list RPC would make one bad
  row hide every container type. Nothing repairs the row — a repair would need
  a migration or a write-back path that #138 does not ask for.
- **The SPA already resolved the name** (`containerType?.name ??
  sample.containerTypeId`); the UUID was a symptom of the 500, not a second
  bug. The test pins the screen-level behavior the issue asked for.

**Tests:**

```
$ clang-format --dry-run --Werror $(git ls-files '*.c' '*.cc' '*.cpp' '*.cxx' \
    '*.h' '*.hh' '*.hpp' '*.hxx' '*.ipp' '*.proto')
exit 0

$ CTEST_PARALLEL_LEVEL=1 ctest --preset dev \
    -R 'ParseDimensions|ListContainerTypesToleratesMalformedOuterDimensionsJson|ContainerTypeJsonRoundTrip'
100% tests passed out of 12

$ CTEST_PARALLEL_LEVEL=1 ctest --preset dev
100% tests passed out of 1667   # 0 failed, 231 skipped (Postgres, no FMGR_TEST_POSTGRES_URL)

$ cd src/web && env -u NODE_ENV npm run check
Test Files  44 passed (44)
     Tests  720 passed (720)
check-bundle-size: initial JS 162.4 KiB gzipped, budget 250 KiB
exit 0                          # lint 0 errors, 1 pre-existing warning in src/ui/Table.tsx

$ scripts/agent/conan-install.sh asan && cmake --build --preset asan
$ CTEST_PARALLEL_LEVEL=1 ctest --preset asan -LE 'grpc_integration|e2e'
100% tests passed out of 1238   # 0 failed, 231 skipped (Postgres)
```

The red run for the new tests (before the fix) is recorded on PR #156: 7 of 10
focus tests failed, including the integration test with the exact
`key 'width' not found` error. The integration test plants `{}` with a direct
`UPDATE`, because the API write path cannot produce that state.

**Known limitations / follow-ups:**

- The Postgres backend's copy of the same `parse_dimensions()` is covered by
  the unit test (the header is backend-neutral), but the integration test
  plants the row through SQLite only; Postgres integration tests were skipped
  because no `FMGR_TEST_POSTGRES_URL` is configured on this machine.
- Under asan the `grpc_integration|e2e` labels are excluded, as CI does.
- #143 (seed: audit rows, PHI demo, box-size variety) is the next slice on this
  worker; it touches `scripts/seed_demo.py` again.
