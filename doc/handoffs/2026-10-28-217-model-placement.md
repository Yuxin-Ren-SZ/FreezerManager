# #217 — freezer model catalogue, typed placement, migration 15 (3D-1)

Implemented by: lead (Claude Code), 3D track. No worker slot was free.
Integration branch: `feat/215-layout-3d`. Design: [ADR 0003](../adr/0003-freezer-3d-layout-api.md).

## Result

- **Core** (`src/core/`, header-only and pure):
  - `placement.h` adds `Placement` and `GridSpec`, plus:
    - `PlacementBounds`: columns per level, one shared depth;
    - `grid_error`: each dimension in 1..64;
    - `placement_error`: refuses a placement under a parent with no grid, or outside the bounds;
    - `stranded_placements`.
  - `freezer_models.h` adds the built-in catalogue:
    - the entries `eppendorf-cryocube-f740hi` and `thermo-tsx40086a`;
    - `find_freezer_model`;
    - `cabinet_bounds`: one level per compartment, `rack_slots` wide, one rack deep;
    - `freezer_model_errors`.
  - The catalogue test validates every entry. Eleven planted bad specs prove the validator catches each rule.
- **Core types:**
  - `Freezer::freezer_model_id` (`""` = none), `StorageContainer::{placement, grid}` and `Box::placement`. All are appended last, so existing designated initialisers are unchanged.
  - JSON snapshots carry the new keys.
  - A snapshot written before the keys existed reads back as unset (`json_helpers::opt_from_json_key`, `json.value`).
- **Migration 15** (`0015_layout_placement`), the same in both backends:
  - `freezers.freezer_model_id TEXT NOT NULL DEFAULT ''`.
  - Nullable placement and grid triples on `storage_containers`, and a placement triple on `boxes`.
  - CHECK constraints:
    - each triple is all-null or all-set;
    - placement values are ≥ 0;
    - grid values are in 1..64.
  - Partial unique indexes on (parent, level, column, depth) where the row is live and placed.
  - SQLite can't add a table CHECK with `ALTER TABLE`, so the cross-column rule sits on each triple's last column.
  - No table was added and no RLS policy changed.
- **Repositories** (SQLite and Postgres):
  - They read and write the new columns.
  - `detail/PlacementColumns.h` refuses negative placements and out-of-range grids with `ConstraintViolation` before any SQL runs.
  - Whether a placement fits its parent's grid needs the parent and the model, so that check is left to the service layer (#218).
- **Proto** (`box.proto`, additive):
  - The ADR 0003 messages and enums.
  - New fields: `Freezer.freezer_model_id = 10`, `StorageContainer.placement = 11` / `grid = 12`, `Box.placement = 10`, and on the Create requests.
  - `capacity_hint_json` is marked `deprecated`.
  - The request and response messages for the scene and the catalogue.
  - The two RPCs are **not declared yet**: declaring them without handlers would make `FreezerServer::build()` refuse to start, because no registry entry would exist. #218 declares them with their handlers.

## Validation

| Command | Result |
|---|---|
| `cmake --build --preset dev && ctest --preset dev` | 1831/1831 passed. The Postgres-parameterised cases skip locally: no `FMGR_TEST_POSTGRES_URL`, no container runtime. |
| `ctest --preset asan -LE 'grpc_integration\|e2e'` | 1357/1357 passed (macOS, Apple clang) |
| `ctest --preset ubsan -LE 'grpc_integration\|e2e'` | 1357/1357 passed (macOS, Apple clang) |
| `clang-format-17 --dry-run --Werror` on changed files | clean (`.venv` clang-format 17.0.6) |
| `tools/check-spdx-headers.sh` | clean |
| clang-tidy | not run locally. The CI `clang-tidy` job covers it. |

**Postgres:** the Postgres repository and migration paths were exercised by CI's `postgres:16` service only. They were not run locally.

## Tests (written first, commits 2083e35 and c04e880)

- `tests/unit/placement_test.cpp` and `tests/unit/freezer_models_test.cpp`.
- `tests/unit/freezer_types_test.cpp` and `box_types_test.cpp`: round-trips, plus legacy snapshots without the new keys.
- `tests/unit/layout_repository_test.cpp` and `box_repository_test.cpp`, parameterised over both backends:
  - round-trip and clear;
  - a duplicate live sibling is refused with `UniqueViolation`;
  - the same cell under a different parent is allowed;
  - an archived sibling's cell can be reused;
  - a negative placement is refused;
  - a grid outside 1..64 is refused.

The issue named `tests/backend_conformance/`. Those suites cover a synthetic sample entity only. The layout and box entities have always been checked across backends by the parameterised repository tests, so the new cases went there.

## Notes for #218

- **Update semantics** (ADR 0003 §2): on Update, an absent `placement`, `grid` or `freezer_model_id` means unchanged.
  - `clear_placement` / `clear_grid` remove them, and `freezer_model_id = ""` clears the model.
  - Old clients send whole messages without these fields, so they must not wipe them.

- **Placement errors:** the service validates parent bounds with `core::placement_error`. A layout root's bounds come from `core::cabinet_bounds(model)`; any other container's bounds come from `bounds_of(*grid)`.
- **Collisions:** a sibling collision surfaces as `UniqueViolation`. On SQLite this happens at commit, because writes are staged; on Postgres it happens at insert. Map it to `ALREADY_EXISTS`.
- **Swaps:** SQLite flushes staged rows in id order, so swapping two siblings' cells in one transaction can trip the unique index mid-flush. Do swaps in two steps, or clear one placement first.
- **Model changes:** a freezer whose model changes must not strand placed root children. Use `stranded_placements(cabinet_bounds(new_model), children)`.

## Merge to `main`

When the track merges, this migration needs `lock:migration`. It must be renumbered if `main` has taken 15 (for example #179). `box.proto` needs `lock:proto`.
