# Handoff note — 2026-05-08, D2 roles, permissions, role-scoped memberships

Implemented D2 against the existing identity slice:

- `src/core/permissions.h` is the single source of truth for the permission
  catalog (`Permission` enum + key strings + `builtin_role_permissions()`).
- `src/core/role.h` adds `Role`, `RolePermission`, the `RolePermissionId`
  composite key, `builtin_role_id(RoleKind)` (deterministic UUIDs reserved
  in the `00000000-0000-0000-0000-00000000000X` namespace), and the
  `validate_scope_filter()` helper that pins
  `LabMembership.scope_filters_json` to a closed set of additive whitelist
  keys (`freezer_in`, `project_in`, `item_type_in`).
- `src/core/identity.h` extends `LabMembership` with `std::optional<RoleId>
  role_id` (nullable to keep migration safe; future RPC writes will fill it).
- SQLite migration `0003_roles` creates `permissions`, `roles`,
  `role_permissions`, and `lab_memberships.role_id`, and seeds the
  permission catalog plus the five built-in roles with their grants. The
  built-in role UUIDs and grants in the seed mirror `core::builtin_role_id()`
  and `core::builtin_role_permissions()` so SQLite and the future Postgres
  backend (C5) produce identical role rows.
- `src/storage/RoleTraits.h` and `src/storage/sqlite/RoleRepositories.{h,cc}`
  add typed repositories for `Role` (CRUD + soft-delete via
  `archived_at_micros`, with a guard against archiving built-in roles) and
  `RolePermission` (insert + hard-delete via `soft_delete`, since grant rows
  carry no tombstone state). Audit rows are still appended through the
  shared transaction commit hook.
- `src/storage/sqlite/IdentityRepositories.cc` threads `role_id` through
  `lab_memberships` reads/writes; FK violations against a missing role
  surface as `ForeignKeyViolation`.

Verification completed locally:

- `cmake --build --preset dev`
- `ctest --preset dev` — 63/63 tests passed (up from 38).
- `FMGR_STORAGE_STRESS=1 ctest --preset dev -R SqliteBackendConformance` —
  10/10 SQLite conformance tests passed.
- `clang-format --dry-run --Werror` on all new/changed files (clean after
  one auto-format pass).
- `tools/check-spdx-headers.sh`
- `git diff --check`

Handoff notes:

- D2.* checkboxes below are ticked. The seed migration is intentionally
  non-idempotent on a SQLite "downgrade" replay (it would re-INSERT
  permissions and re-`ALTER TABLE`); the conformance suite uses test-only
  migrations so this is not exercised. C5 will need its own
  Postgres-flavoured 0003 with the same UUIDs and grants.
- D1.3 first-run wizard is still deferred; it now has both the role rows
  and the permission catalog it needs — pick it up alongside K5
  (CLI bootstrap) + E2 (LocalAuthProvider).
- Custom roles defined by lab admins (PRD §3 "Lab admins may define
  custom roles") are schema-supported (`roles.lab_id NOT NULL`,
  `is_builtin = 0`) but the RPCs to create/edit them belong to E1.
- The next implementation slice should start at D3 (`Freezer` and
  `StorageContainer` recursive layout) per the §D entity ordering, or
  jump to E1 RBAC middleware now that the permission catalog exists.
