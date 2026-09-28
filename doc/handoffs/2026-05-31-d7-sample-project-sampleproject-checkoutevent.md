# Handoff note — 2026-05-31, D7 Sample + Project + SampleProject + CheckoutEvent

Implemented D7 against the existing geometry, layout, identity, item-type, and box slices:

- `src/core/sample.h` defines `Sample`, `Project`, `SampleProjectId`, `SampleProject`,
  and `CheckoutEvent` with JSON serialization. Also adds `VolumeUnit`/`MassUnit` JSON
  converters (needed to persist them individually as separate DB columns).
- `src/storage/SampleTraits.h` adds `EntityTraits` specializations for all four entities.
  `Sample` uses `Field::Status` as its tombstone field (status = tombstoned, not
  archived_at_micros). `SampleProject` and `CheckoutEvent` use dummy tombstone fields
  (hard-delete and insert-only, respectively).
- SQLite migration `0008_samples` creates `projects`, `samples`, `sample_projects`, and
  `checkout_events` tables. Key constraints:
  - `CHECK ((box_id IS NULL) = (position_label IS NULL))` on samples.
  - Partial unique index `samples_position_unique` on `(box_id, position_label)` WHERE
    `status IN ('active', 'checked_out')` — the core no-double-booking invariant.
- `src/storage/sqlite/SampleRepositories.{h,cc}` adds four typed SQLite repositories:
  - `SampleRepository`: validates non-empty name, item_type_id liveness, box existence,
    position label existence in BoxType, and size_class compatibility via
    `box_type_position_accepts`. Soft-delete sets `status = tombstoned`.
  - `ProjectRepository`: standard CRUD + soft-delete via `archived_at_micros`.
  - `SampleProjectRepository`: composite-key link table; hard-deleted via `soft_delete()`;
    `update()` throws `UnsupportedOperation`.
  - `CheckoutEventRepository`: append-only audit records; `update()` and `soft_delete()`
    throw `UnsupportedOperation`.

Verification completed locally:

- `cmake --build --preset dev`
- `ctest --preset dev` — 175/175 tests passed (up from 103).
- `FMGR_STORAGE_STRESS=1 ctest --preset dev -R SqliteBackendConformance`
  — 10/10 SQLite conformance tests passed.
- `clang-format --dry-run --Werror` on all new/changed files — clean.
- `clang-tidy -p out/build/dev` on new .cc and test files — clean (exit 0).
- `tools/check-spdx-headers.sh` — all new C++/SQL files carry the AGPL header.
- `git diff --check` — no trailing whitespace.

Handoff notes:

- D7.1 through D7.4 are implemented. D7.5 (move atomicity property test: 50 threads
  moving the same sample concurrently) is not yet a test; it can be added to the
  property test suite (`tests/property/`) when RapidCheck integration lands.
- Sample state machine (active → checked_out → active → depleted → tombstoned) is NOT
  enforced in the repository layer — enforcing it at the RPC layer (F2) is intentional.
  The repository allows writing any valid status; the partial unique index enforces the
  no-double-booking invariant regardless of how status transitions are orchestrated.
- Cross-entity seeding in tests must use separate committed transactions when entities
  have validation cross-references (e.g. BoxType validates ContainerType.size_class in
  the DB, not in the pending staging map).
- C4.3 (JSON-path generated columns for indexed CustomFieldDefinition fields) now has
  its dependency (CustomFieldDefinition + samples.custom_fields_json) in place; it can
  be implemented at any time.
- D8 (ShareRequest) is the natural next domain entity slice.
- C5 (Postgres backend) must mirror migration 0008_samples with the same version number
  and preserve the no-ON-DELETE-CASCADE design on boxes and sample_projects.
