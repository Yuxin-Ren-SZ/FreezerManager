# Handoff note — 2026-05-27, D4.2 seed templates + D5 Box entity

Implemented D4.2 and D5 against the existing geometry + layout slices:

- `data/seed/container_types.json` defines four standard ContainerType stubs
  (cryovial_2ml, tube_50ml, tube_15ml, microplate_well) importable by lab admins.
- `data/seed/box_types/` holds four BoxType seed templates: `9x9_cryobox.json`
  (81 positions), `10x10_cryobox.json` (100 positions), `96_well_rack.json`
  (96 positions), and `mixed_eppendorf.json` (13 positions: 3×3 for 50 mL
  tubes + 2×2 for 15 mL tubes).
- `src/core/box.h` adds the `Box` struct with 9 fields and JSON serialization;
  the file already contained ContainerType / BoxType / Position from D4.1.
- `src/storage/BoxGeometryTraits.h` adds `EntityTraits<Box>`.
- SQLite migration `0006_boxes` creates the `boxes` table with FKs to
  `labs`, `box_types`, and `storage_containers` (all deferrable, no ON DELETE
  CASCADE — tombstone propagation is application-level). Unique index on
  `(lab_id, label) WHERE archived_at_micros IS NULL`.
- `src/storage/sqlite/BoxGeometryRepositories.{h,cc}` adds `BoxRepository` and
  `register_box_repositories()`. Validation enforces: non-empty label;
  `box_type_id` must reference a live BoxType in the same lab; `storage_container_id`
  must reference a live StorageContainer in the same lab — both via direct SQL
  queries (same pattern as D4.1 size-class cross-reference checks).

Verification completed locally:

- `cmake --build --preset dev`
- `ctest --preset dev` — 103/103 tests passed (up from 88).
- `FMGR_STORAGE_STRESS=1 ctest --preset dev -R SqliteBackendConformance`
  — 10/10 SQLite conformance tests passed.
- `clang-format --dry-run --Werror` on all new/changed files — clean.
- `clang-tidy -p out/build/dev` on new .cc and test files — clean.
- `tools/check-spdx-headers.sh` — all new C++/SQL files carry the AGPL header.
- `git diff --check` — no trailing whitespace.

Handoff notes:

- D4.2 and D5 checkboxes are ticked.
- The seed JSON files have no `id`, `lab_id`, `created_at`, or `archived_at`
  fields — an importer (future CLI command or RPC) must supply these on ingestion.
  The seed template test (`tests/unit/seed_templates_test.cpp`) validates
  position counts and structure via the `FMGR_SEED_DATA_DIR` compile definition.
- D6 (`ItemType` + `CustomFieldDefinition`) is the natural next slice — it
  unblocks D7 (Sample) and triggers C4.3 (JSON-path indexes on indexed custom fields).
- C5 (Postgres backend) when started must mirror migration `0006_boxes`
  with the same version number; the application-level tombstone propagation
  constraint (no ON DELETE CASCADE on `storage_container_id`) must be preserved.
- D5 Watch: if a StorageContainer is soft-deleted, the application must cascade
  the tombstone to all Boxes in that container. No automated cascade exists in the
  schema by design (preserves audit history of sample locations).
