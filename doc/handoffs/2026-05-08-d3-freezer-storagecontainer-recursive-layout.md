# Handoff note — 2026-05-08, D3 Freezer + StorageContainer recursive layout

Implemented D3 against the existing identity + role slices:

- `src/core/ids.h` adds `FreezerId` (StrongId tag).
- `src/core/freezer.h` defines `Freezer`, `StorageContainer`, and
  `CapacityHint` value types with `Field` enums and JSON conversions.
  `CapacityHint.{rows,cols,depth}` are advisory `std::optional<int>`
  per PRD §4.1 — no enforcement at write time.
- `src/storage/FreezerTraits.h` adds `EntityTraits<Freezer>` and
  `EntityTraits<StorageContainer>`, both using `ArchivedAt` as the
  tombstone field.
- SQLite migration `0004_layout` creates `storage_containers` and
  `freezers` with deferred-FK self/cross references so the layout root
  container and its parent freezer can be inserted in a single
  transaction in either order. `freezers (lab_id, name)` is uniquely
  indexed only among non-archived rows; `storage_containers (id,
  parent_id)` carries a `CHECK (id <> parent_id)` self-parent guard.
  The migration is also committed as
  `src/storage/sqlite/migrations/0004_layout.sql` for reference; the
  authoritative copy used by the runtime is the inline R-string in
  `SqliteBackend.cc::default_migrations()`.
- `src/storage/sqlite/LayoutRepositories.{h,cc}` adds typed SQLite
  repositories for both entities. `StorageContainer` writes (insert
  and update) invoke `check_no_cycle()`, which walks the proposed
  ancestor chain through both staged in-transaction state and
  persisted rows and rejects cycles with `ConstraintViolation`.
  Soft-delete bypasses the cycle check (parent_id unchanged).
- `register_layout_repositories()` registers `StorageContainer` first
  and `Freezer` second; either ordering works at commit time because
  of the deferred FKs, but registering the container repo first makes
  the parent-before-child reading order intuitive in tests.

Verification completed locally:

- `cmake --build --preset dev`
- `ctest --preset dev` — 76/76 tests passed (up from 63).
- `FMGR_STORAGE_STRESS=1 ctest --preset dev -R SqliteBackendConformance`
  — 10/10 SQLite conformance tests passed.
- `clang-format --dry-run --Werror` on all new/changed files (clean
  after one auto-format pass).
- `clang-tidy -p out/build/dev` on the new `.cc` and test files —
  clean (only third-party non-user-code warnings, all suppressed).
- `tools/check-spdx-headers.sh`
- `git diff --check`

Handoff notes:

- D3 checkbox is ticked.
- D4 (`ContainerType` + `BoxType` + `Position`) is the natural next
  slice — it unblocks D5 and the no-double-occupancy invariant tests
  in C3.3. The standard-library BoxType templates (D4.2) are
  importable seed JSON and can land in the same slice or a follow-up.
- C5 (Postgres backend) when started must mirror migration `0004_layout`
  with the same version number; the SQLite-only `CHECK (id <> parent_id)`
  is fine to keep, but the cycle check will need to be a Postgres
  trigger or RECURSIVE CTE since libpqxx callers should not pay an
  extra round-trip per write.
- Capacity-hint *enforcement* is intentionally deferred. The PRD
  treats hints as advisory; D5 (Box) will be the first place that
  could optionally consult them.
- `freezerctl` CLI commands (D-section says "create/list/inspect")
  remain deferred until K5/CLI scaffolding lands.
