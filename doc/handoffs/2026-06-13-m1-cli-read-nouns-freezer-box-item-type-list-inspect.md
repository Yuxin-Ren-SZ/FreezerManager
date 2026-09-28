# Handoff note — 2026-06-13, M1 CLI read nouns (freezer/box/item-type list + inspect)

Added the read-only half of the outstanding M1 CLI nouns: `freezerctl freezer|box|
item-type list` and `... inspect --id <uuid>` (PRD §19 M1). Every read is
lab-scoped and Postgres-RLS-gated, reusing the `sample list` query/format pattern.

**New files:**
- `src/cli/EntityRead.h` — header-only, templated `query_in_lab<T>()` and
  `find_in_lab<T>()`, factored from `SampleQuery.cc`. Both inject the
  `current_lab_ids` RLS session var (no-op on SQLite). `find_in_lab` returns
  nullopt when the row is absent **or** belongs to another lab — defense-in-depth
  against cross-lab disclosure via a guessed id, complementing RLS.
- `src/cli/NounCommands.{h,cc}` — `run_{freezer,box,item_type}_list` (tab table +
  `N x(s)` footer) and `run_{freezer,box,item_type}_inspect` (FIELD<TAB>VALUE
  detail; returns 1 + "not found" when absent/foreign). Output to an injected
  `std::ostream` (no stdout), so unit-testable.

**Changed:**
- `src/cli/CliApp.cc` — `freezer`/`box`/`item-type` subcommands, each with `list`
  (shared `--sqlite`/`--postgres`/`--lab`/`--limit`/`--include-tombstoned`) and
  `inspect` (`--lab` + required `--id`). Dispatch extracted into
  `dispatch_read_noun`/`dispatch_read_nouns` helpers to stay under the run_cli
  cognitive-complexity budget.
- `src/cli/CMakeLists.txt` — adds `NounCommands.cc`.

**Tests (`tests/unit/cli_test.cpp`):** the `CliFixture` now also seeds a
container-type, a root storage container + freezer per lab, a box-type, and a box
in lab A. New SQLite+Postgres-parameterized cases: per-noun list (incl. lab-scope
exclusion of lab B's freezer), box list tombstone include/exclude, freezer inspect
detail, inspect of an unknown id, and inspect of another lab's id (both
not-found). Plus argv-level `freezer list`, `box inspect`, and `item-type inspect`
through `run_cli`.

Verification: `cmake --build --preset dev` clean; `ctest --preset dev -j1` —
918/918 pass (Postgres cli/conformance skip without `FMGR_TEST_POSTGRES_URL`);
`clang-format --dry-run --Werror` + `run-clang-tidy-17` on new/changed sources
clean; `tools/check-spdx-headers.sh` + `git diff --check` clean.

Handoff notes:
- Read-only slice. The `create` nouns (write + audit `MutationContext`) are the
  next slice and need a lab to exist — pair with `lab create` / D1.3 first-run.
- `storage-container` and `container-type` read nouns drop in with the same
  `EntityRead.h` helpers when wanted.
- CSV import beyond Sample (boxes, item types, custom-field defs, users) remains a
  separate follow-up reusing `CsvReader` + the `build_import`/report pattern.
