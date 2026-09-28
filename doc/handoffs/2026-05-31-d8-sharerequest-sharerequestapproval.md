# Handoff note — 2026-05-31, D8 ShareRequest + ShareRequestApproval

Implemented D8 cross-lab share-request workflow:

- `src/core/enums.h` adds `ShareRequestStatus` (pending/approved/rejected/revoked) and
  `ShareApprovalRole` (source_admin/target_admin/system_admin) with string converters and
  JSON adapters, following the existing enum pattern.
- `src/core/share_request.h` defines `ShareRequestApprovalId` (composite key), `ShareRequestApproval`
  (append-only audit record), and `ShareRequest` with JSON serialization. Uses `sr_opt_to_json`
  helpers for optional fields. State machine and FK validation deferred to RPC layer.
- `src/storage/ShareRequestTraits.h` adds EntityTraits for both entities. ShareRequest uses
  `Field::Status` as tombstone marker (soft_delete sets status = revoked). ShareRequestApproval
  uses a dummy tombstone field (append-only, never soft-deleted).
- SQLite migration `0009_share_requests` creates `share_requests` (with CHECK source != target) and
  `share_request_approvals` (PRIMARY KEY (share_request_id, approver_role), append-only). No ON
  DELETE CASCADE; application-level integrity only.
- `src/storage/sqlite/ShareRequestRepositories.{h,cc}` adds two typed SQLite repositories:
  - `ShareRequestRepository`: validates non-empty scope_json and source != target lab at
    application layer (DB CHECK enforces it too). Default query filter: status = 'pending';
    include_tombstoned() shows all. soft_delete() sets status = revoked + decided_at = now.
  - `ShareRequestApprovalRepository`: append-only (update() and soft_delete() throw
    UnsupportedOperation). insert() validates share_request exists in committed DB before
    inserting approval. Composite-key pending map (no base template, same pattern as
    CheckoutEventRepository).

Verification completed locally:

- `cmake --build --preset dev`
- `ctest --preset dev -j1` — 229/229 tests passed (up from 175, +54 total new tests including D6/D7/D8 work).
- `FMGR_STORAGE_STRESS=1 ctest --preset dev -j1 -R SqliteBackendConformance` — 10/10 passed.
- `clang-format --dry-run --Werror` on all new/changed C++ files — clean.
- `clang-tidy -p out/build/dev` on new .cc and test files — clean.
- `tools/check-spdx-headers.sh` — all new C++/SQL files carry the AGPL header.
- `git diff --check` — no trailing whitespace.

Handoff notes:

- D8.* (ShareRequest + ShareRequestApproval) are implemented. The three-signature approval
  workflow (source_admin + target_admin + system_admin) is enforced by the DB PRIMARY KEY on
  share_request_approvals; the state machine transitions (pending → approved/rejected/revoked) are
  intentionally deferred to the RPC layer (F2).
- Cross-entity validation in ShareRequestApprovalRepository::insert() validates committed DB only
  (not staging map) — same pattern as SampleRepository validates item_type_id and box_id.
- The visible-labs computation ({home_lab} ∪ {labs sharing TO me}) is NOT implemented in the
  repository layer — it belongs in E3 (RBAC middleware) and the Postgres RLS policy (C5.3).
- D9 (Session entity) is the natural next domain entity slice.
- C5 (Postgres backend) must mirror migration 0009_share_requests with the same version number
  and preserve the no-ON-DELETE-CASCADE design.
- Note: D7 tests (sqlite_sample_repository_test.cpp) have a known parallelism flakiness when
  run with `ctest` default multi-job mode; run `ctest -j1` for deterministic results. Root
  cause: SQLite file path generation uses stack pointer address which can collide across
  concurrent fixture constructors in multi-process test execution.
