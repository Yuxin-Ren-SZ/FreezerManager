# Handoff note — 2026-05-09, D4 core geometry

Implemented D4 core against the existing identity + SQLite storage slices:

- `src/core/box.h` defines `ContainerType`, `BoxType`, `Position`, and
  `OuterDimensionsMm` value types with `Field` enums and JSON conversions.
- `src/storage/BoxGeometryTraits.h` adds `EntityTraits<ContainerType>` and
  `EntityTraits<BoxType>`, both using `ArchivedAt` as the tombstone field.
- SQLite migration `0005_box_types` creates `container_types`, `box_types`,
  `box_type_positions`, and `box_type_position_accepts`. Positions and
  accepts are child rows rather than embedded JSON so D5 placement checks can
  query geometry directly.
- `src/storage/sqlite/BoxGeometryRepositories.{h,cc}` adds typed SQLite
  repositories. `ContainerType` validates non-empty `name`/`size_class` and
  positive dimensions when present. `BoxType` validates unique position
  labels, non-negative coordinates, non-empty unique accepts lists, and
  accepted `size_class` tokens that resolve to live `ContainerType` rows in
  the same lab.
- `BoxType` updates atomically replace the persisted position/accept rows
  for that box type inside the same transaction.

Verification completed locally:

- `cmake --build --preset dev`
- `ctest --preset dev` — 88/88 tests passed (up from 76).
- `FMGR_STORAGE_STRESS=1 ctest --preset dev -R SqliteBackendConformance`
  — 10/10 SQLite conformance tests passed.
- `clang-format --dry-run --Werror` on all new D4 source/test files.
- `clang-tidy -p out/build/dev` on the new `.cc` and test files — clean
  (only third-party non-user-code warnings, all suppressed).
- `tools/check-spdx-headers.sh`
- `git diff --check`

Handoff notes:

- D4.1 checkbox is ticked.
- D4 remains open only for D4.2 standard BoxType seed JSON templates.
- C5 (Postgres backend) when started must mirror migration `0005_box_types`
  with the same version number and preserve the D4.1 validation contract.
- D5 (`Box`) can now use `box_type_positions` and
  `box_type_position_accepts` to enforce placement compatibility.
