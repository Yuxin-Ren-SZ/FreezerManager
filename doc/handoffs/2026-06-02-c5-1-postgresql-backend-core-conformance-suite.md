# Handoff note — 2026-06-02, C5.1 PostgreSQL backend core + conformance suite

Implemented C5.1: `PostgresBackend` + `PostgresTransaction` core and Postgres-dialect migrations
0001–0012 with RLS policies and 13 conformance tests (skipped when `FMGR_TEST_POSTGRES_URL` unset).

**New files:**
- `src/storage/postgres/PostgresBackend.{h,cc}` — `IStorageBackend` implementation:
  - Connection pool (`PostgresBackendState`): fixed-size vector of `unique_ptr<pqxx::connection>`,
    condition-variable acquire/release, configurable timeout → `Unavailable` on exhaustion.
  - `PostgresTransaction`: `std::optional<pqxx::work>` for safe lifecycle management; set/reset
    explicitly before releasing pool slot; `set_session_var` uses `set_config($1, $2, true)`.
  - Migration runner: same checksum scheme as SQLite; `std::ranges::sort` by version; idempotent
    (skip already-applied); checksum mismatch throws `MigrationFailure`.
  - Migrations 0001–0012: Postgres-dialect SQL (JSONB, BIGINT, BOOLEAN, DEFERRABLE FK). All
    lab-scoped tables get `ENABLE ROW LEVEL SECURITY; FORCE ROW LEVEL SECURITY; CREATE POLICY`
    using `current_setting('app.current_lab_ids', true)`. Migration 0011 is a no-op (Postgres
    already has the full audit_events schema from migration 0001; SQLite needed a DROP+recreate).
  - Audit chain: advisory lock `pg_advisory_xact_lock(8675309)` serialises concurrent appends;
    `pqxx::params` builder for the INSERT; `std::optional<std::string>{}` for NULL lab_id.
  - `caps()`: `row_level_security=true`, `json_path_equality=true`, `json_path_indexes=true`,
    `listen_notify=true`.
  - Test hooks: `fail_next_audit_append_for_tests()`, `audit_event_count_for_tests()`,
    `downgrade_to_zero_for_tests()` — mirrors SQLite test hook interface.
- `src/storage/postgres/CMakeLists.txt` — links libpqxx + libsodium + FreezerManager::audit.
- `tests/backend_conformance/postgres_backend_conformance_test.cpp` — 13 tests:
  - Full conformance suite (10 tests mirror SQLite: CRUD, query DSL, tombstone, position
    uniqueness, unsupported entity, serializable isolation, concurrent placement, audit atomicity,
    audit-failure prevention, migration downgrade+forward).
  - 3 Postgres-specific: `CapabilitiesReportRowLevelSecurity`, `MigrateToLatestIdempotent`,
    `SetSessionVarVisibleWithinTransaction`.
  - All skip with `GTEST_SKIP()` when `FMGR_TEST_POSTGRES_URL` is unset (local dev without Docker).

**CI addition needed (not yet done):**
Add to `build.yml`:
```yaml
services:
  postgres:
    image: postgres:16
    env: { POSTGRES_DB: fmgr_test, POSTGRES_USER: postgres, POSTGRES_PASSWORD: test }
    ports: ['5432:5432']
    options: --health-cmd pg_isready --health-interval 5s --health-timeout 5s --health-retries 5
env:
  FMGR_TEST_POSTGRES_URL: postgresql://postgres:test@localhost/fmgr_test
```

**API notes for domain repository authors:**
- Include `storage/postgres/PostgresBackend.h` (which includes `<pqxx/pqxx>`).
- Access transaction via `txn.work()` returning `pqxx::work&`.
- Use `txn.work().exec(sql)` for no-param queries.
- Use `txn.work().exec(sql, pqxx::params{arg1, arg2, ...})` for parameterized queries.
- `exec` returns `pqxx::result`; iterate with `for (pqxx::row_ref row : result)`.
- Field access: `row.at("col_name").as<std::string>()` etc.
- Null params: `std::optional<std::string>{}` appended to `pqxx::params`.
- Error mapping: catch `pqxx::sql_error` and check `error.sqlstate()` (returns `std::string_view`).
- Call `txn.note_mutation(entity_kind, entity_id, ctx)` for every mutation (audit chain).

**What remains for C5 completion:**
- C5 domain repositories (IdentityRepositories, RoleRepositories, LayoutRepositories,
  BoxGeometryRepositories, ItemTypeRepositories, SampleRepositories, SessionRepositories,
  ShareRequestRepositories, AuditRepositories) — not yet implemented. Needed for CLI/CSV (M1).
- C5.2: CI `build.yml` Postgres service container.
- C5.3: RLS policy unit test (set `app.current_lab_ids` to wrong lab, assert 0 rows on
  domain tables). Deferred until domain repositories land.
- C5.4: `sync_custom_field_indexes(lab_id)` for JSONB GIN indexes on indexed CustomFieldDefinitions.

Verification:
- `cmake --build --preset dev` — clean.
- `ctest --preset dev -j1` — 428/428 passed (13 Postgres tests skipped, 415 pass).
- `clang-format --dry-run --Werror` on all new/changed C++ files — clean.
- `clang-tidy -p out/build/dev src/storage/postgres/PostgresBackend.cc` — exit 0.
- `tools/check-spdx-headers.sh` — all new files carry AGPL header.
