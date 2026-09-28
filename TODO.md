# TODO — Implementation Backlog

> **How this file is used** (see `AGENTS.md` → Coordination)
>
> - This is the **roadmap/spec**, keyed by stable IDs (`F7`, `C-10`, `H2.3`, …).
>   It says *what* to build; it does not track who is building it.
> - **Live status** (ready / in progress / blocked / in review, and who owns
>   it) lives in **GitHub Issues**, one issue per task titled `[<ID>] …`, plus
>   the pinned *Agent coordination board* issue.
> - **Only the lead agent edits this file**, in its own PRs, after the owner
>   merges work. Workers never touch it — put findings in your issue or PR.
> - Handoff notes do not go here. Each merged task adds one file under
>   `doc/handoffs/` (older notes were moved there on 2026-09-27).
> - Checkboxes: `[ ]` open · `[~]` partly done (the item says what remains) ·
>   `[x]` done.

## Quality review — 2026-07-01 (test coverage audit + Claude review)

Full audit: `doc/TEST_COVERAGE_AUDIT_2026-07-01.md`

### Root cause pattern found

All fake gRPC services returned `grpc::Status::OK` unconditionally. Every
`if (!result.ok)` branch in production code was dead code from a test
coverage perspective. Tests predominantly tested model layers directly
rather than through widget wire-up paths. This pattern allowed a
parameter-swap bug (`93d3b3c`) to pass 1488 tests undetected.

### Fixes applied (12/12 test files)

| File | Commit | What |
|------|--------|------|
| `qt_box_service_client_test.cpp` | `d3bc513` | 5 error-path: all RPCs |
| `qt_sample_lookup_widget_test.cpp` | `d4b7567` | 6: name fallback, disambiguation click, error display, status |
| `qt_location_path_resolver_test.cpp` | `580ce02` | 7: cycle guard, deep chain, edge cases |
| `qt_box_grid_model_test.cpp` | `55ad006` | 5: 3 setBox fails + accessors + empty box |
| `qt_sample_table_model_test.cpp` | `acd8be8` | +16: columns, errors, edge cases |
| `qt_lab_service_client_test.cpp` | `37e7537` | getLab error |
| `qt_auth_service_client_test.cpp` | `37e7537` | logout error |
| `qt_session_manager_test.cpp` | `7f12425` | QTimer auto-logout |
| `qt_lab_tree_model_test.cpp` | `7f12425` | mid-tree RPC failures |
| `qt_barcode_scan_controller_test.cpp` | `7f12425` | empty barcode, gRPC error |
| `qt_sample_service_client_test.cpp` | `7f12425` | getSample/export/checkout errors |
| `qt_box_map_pdf_test.cpp` + `qt_label_pdf_test.cpp` | `7f12425` | gRPC error paths |

### Remaining gaps (from Claude review)

- ✅ (fixed in `2afb49b`) **LabTreeModel recursion has no cycle guard** — `buildContainers` can
  infinite-recursively overflow stack. Need depth guard or visited set.
  Real bug, not just test gap.
- 🟡 `BoxGridWidget::savePdf()` blocks on `QFileDialog::getSaveFileName()`
  — not unit-testable without an `ISaveDialog` seam. Accepted limitation.
- 🟡 `BarcodeScanController::processScan` whitespace-only trim: behavior
  needs independent assertion.
- Wire-up path tests (Widget→PDF, Bridge→PDF) — PDF tests still call
  `buildModel()` directly, not through BoxGridWidget. Deferred to
  future integration test layer.
- `QSignalSpy` / `gridChanged` tests — require Qt6::Test linkage not in
  `qt_unit_tests` target. Deferred until test target restructuring.

### Key lesson for future work

Every fake gRPC service must support **per-RPC error injection flags**. Prefer
`grpc::StatusCode fail_<method> = grpc::StatusCode::OK` pattern. Never write a
fake that only returns `grpc::Status::OK` — it creates a false sense of test
coverage.

## Security review backlog — 2026-06-28 (senior-engineer code review)

Triaged from `doc/review-senior-engineer-security.md` — a line-level code
security review of `src/auth/`, `src/kms/`, `src/crypto/`, `src/rpc/`,
`src/server/`, `src/storage/detail/QuerySqlBuilder.h`, `src/audit/`,
`src/cli/CsvReader.h` (run against `feat/qt-csv-import-wizard`). Verdict:
unusually strong baseline; 12 findings, one Critical. The `file:line` anchors and
top findings were spot-validated against the live tree on triage. No code changed
in this slice — each row is a future fix slice. **No PRD edit needed**: the
revised §6 (TLS, rate-limiting) and §17.1 (request-id, operability) already cover
the intent; these are code-level follow-ups. Full rationale per finding lives in
the review doc.

| ID | Sev | Area | Anchor | Fix sketch | Target |
|----|-----|------|--------|-----------|--------|
| C-9 | **Critical** | Server | `FreezerServer.cc:68` | Implement TLS cert loading (path is an active `throw`, not a stub). **Pre-deployment blocker for any non-loopback bind.** | M5; gate remote deploy |
| C-1 | High | Auth | `LocalAuthProvider.cc:752` | Lockout map is in-memory, resets on restart → persist failed-attempt state (DB table + TTL) or external limiter. | first prod tag (M3.5/M4) |
| C-7 | High | Audit | `CanonicalJson.cc:13` | Canonical JSON not RFC 8785; nlohmann version drift can break the audit chain. Pin algorithm + CI golden-vector test, or implement JCS. | before 1.0 (M7) |
| C-3 | Medium | Auth | `LocalAuthProvider.cc:272` | `totp_secret_enc` stored/used plaintext despite `_enc`. Encrypt under master KEK via existing `FieldCipher`. | M5 |
| C-10 | Medium | Server | `FreezerServer.cc` build / no cap | No gRPC inbound message cap → set `ResourceQuota`/`MaxReceiveMessageSize` on `ServerBuilder`, configurable via `FreezerServerOptions` (~10 MiB default). | M3.5 (DoS) |
| C-11 | Medium | Server | `GrpcErrorTranslation.h` | `INTERNAL` may leak raw error text (schema probing). Mask in prod, log real error server-side. | M3.5 |
| C-12 | Low | Server | `SampleServiceImpl.cc:47` | `request_id = ""`. Extract `x-request-id` from gRPC metadata → `MutationContext::request_id`. | M3.5 (§17 obs) |
| C-2 | Low | Auth | `validate_token()` | Sessions not IP/UA-bound; no replay detection. Optional IP-binding, off by default (NAT-friendly). | backlog / v2 |
| C-4 | Low | Auth | `SampleServiceImpl.cc:541` | `SoftDeleteSample` two-phase authz bypasses the RPC-registry test. Register a wildcard perm or add `authorize_entity` middleware. | M3.5 |
| C-6 | Low | KMS | `KeyringKms.h:43` | Raw KEK bytes in `std::vector`, no mlock. Wrap in `SecureBuffer` (`sodium_mlock`/`memzero`, optional `mprotect`). | M5 |
| C-8 | Low | Storage | `QuerySqlBuilder.h:216` | Sort direction is the only non-parameterized SQL fragment (enum-gated, safe now). Add `static_assert`/stern comment so a future string-typed sort can't inject. | quick, any slice |
| C-5 | Info | Auth | `Totp.cc:161` | TOTP code compare `==` not constant-time. Switch to `sodium_memcmp` (robust if digit count grows). | quick, any slice |

---

## Section A — Licensing & contributor flow

### A.0 — One-time GitHub-side setup for the CLA workflow

The workflow file at `.github/workflows/cla.yml` is committed but inert
until these are done. Order matters: 1 → 2 → 3 → (open a test PR, see
"Verify" below) → 4.

- [ ] **A.0.1. Create the `cla-signatures` branch.**
  Open https://github.com/Yuxin-Ren-SZ/FreezerManager/branches → **New
  branch** → name `cla-signatures`, source `main` → Create. The Action
  will populate `signatures/v1/cla.json` on this branch on the first
  signed PR.

- [ ] **A.0.2. Create a fine-grained Personal Access Token.**
  Open https://github.com/settings/personal-access-tokens/new
  - Token name: `FreezerManager CLA bot`
  - Expiration: 1 year (set a calendar reminder; see rotation task below)
  - Repository access: *Only selected repositories* → `Yuxin-Ren-SZ/FreezerManager`
  - Repository permissions, all *Read and write*:
    - **Contents**, **Pull requests**, **Issues**, **Commit statuses**
  - Generate. Copy the `github_pat_…` token immediately (shown once).

- [ ] **A.0.3. Add the PAT as a repo secret.**
  Open https://github.com/Yuxin-Ren-SZ/FreezerManager/settings/secrets/actions/new
  - Name: `PERSONAL_ACCESS_TOKEN` (exact spelling — referenced from
    `.github/workflows/cla.yml`)
  - Secret: paste the token from A.0.2 → Add secret.

- [ ] **A.0.4. Make `CLA Assistant` a required status check on `main`.**
  Must be done *after* the workflow has run at least once (otherwise the
  check name doesn't appear in the dropdown). Once the test PR from the
  Verify task below has triggered the workflow:
  Open https://github.com/Yuxin-Ren-SZ/FreezerManager/settings/branch_protection_rules/new
  - Branch name pattern: `main`
  - ☑ Require status checks to pass before merging
  - Add `CLA Assistant` to the required checks list → Save.

### A.1 — Verification & maintenance

- [ ] **Verify CLA Assistant Lite end-to-end** (deferred — requires a second
      GitHub account or a friend's account).
  Steps once a second account is available:
  1. From the second account, fork `Yuxin-Ren-SZ/FreezerManager` and open a
     trivial PR (e.g., a README typo fix).
  2. Within ~30 s, confirm a bot comment appears on the PR asking for the
     CLA signature, and a `CLA Assistant` status check shows as **Failed**.
  3. From the second account, reply on the PR with exactly:
     `I have read the CLA Document and I hereby sign the CLA`
  4. Confirm the bot re-comments acknowledging the signature and the
     `CLA Assistant` check flips to **Passed**.
  5. Confirm a new entry exists on the `cla-signatures` branch at
     `signatures/v1/cla.json` with the contributor's GitHub username +
     timestamp.
  6. Open a second PR from the same account — the bot should NOT ask again.
  7. Confirm branch protection on `main` blocks merging the first PR until
     the check is green.
  Workflow file: `.github/workflows/cla.yml`.

- [ ] **Rotate the `PERSONAL_ACCESS_TOKEN` secret** before expiration
      (calendar reminder ~11 months after creation of A.0.2).
- [ ] **Update `CLA.md` "How to sign" section** to describe the bot-comment
      flow as the primary signing method (keep `Signed-off-by` note as a
      secondary record-keeping convention), once the Action is verified
      working.

---

## Section B — Repository & project hygiene (M0)

- [x] **B1. Confirm copyright holder string.** Decide the exact form for SPDX
      headers and `NOTICE` (e.g., `Copyright (C) 2026 Yuxin Ren`). Once
      confirmed, add `tools/check-spdx-headers.sh` and wire it into CI to
      reject PRs missing headers on new C++/Python files.

- [x] **B2. `CONTRIBUTING.md`.** Document branching model (PRs to `main`,
      squash-merge), commit-message style (Conventional Commits suggested),
      links to `CLA.md`, code-style rules (clang-format config, see B5),
      how to run tests locally.

- [x] **B3. `SECURITY.md`.** Vulnerability-disclosure process: dedicated
      contact email, GPG key for encrypted reports, 90-day coordinated-
      disclosure policy. **Required before any PHI feature lands.**

- [x] **B4. `CODE_OF_CONDUCT.md`.** Contributor Covenant 2.1 template.

- [x] **B5. Build & lint baseline.**
  - [x] **B5.1.** Top-level `CMakeLists.txt` requiring CMake ≥ 3.25, C++20,
        `cmake --preset` files for `dev`, `release`, `asan`, `ubsan`,
        `tsan`, `release-deterministic`.
  - [x] **B5.2.** `conanfile.txt` (or `vcpkg.json`) pinning: gtest, rapidcheck,
        spdlog, fmt, libsodium, sqlite3, libpqxx, gRPC, Protobuf, openssl,
        nlohmann_json (or simdjson), abseil. Lockfile committed.
  - [x] **B5.3.** `.clang-format` and `.clang-tidy` configs. CI fails on
        diffs from clang-format and on clang-tidy errors.
  - [x] **B5.4.** GitHub Actions workflow `.github/workflows/build.yml`:
        matrix over `{gcc-13, clang-17}` × `{Debug, Release}` × `{asan,
        ubsan, tsan, none}` for at least one combination. Caches Conan.
  - [ ] **B5.5.** Coverage workflow using `gcovr` or `llvm-cov`; comment
        coverage delta on PRs. **⚠ Watch:** keep coverage gating advisory
        until the codebase has real surface area; do not block PRs on
        coverage in M0–M1.

- [x] **B6. Repository skeleton.**
  ```
  src/
    core/                    # domain types, no I/O
    storage/                 # IStorageBackend + impls (sqlite, postgres)
    auth/                    # IAuthProvider + impls
    kms/                     # IKmsProvider + impls
    audit/                   # audit chain + verifier
    rpc/                     # gRPC service + REST gateway
    server/                  # main(), wiring, config
    cli/                     # freezerctl
    qt/                      # Qt 6 desktop client
    web/                     # React SPA (separate package.json)
    py/                      # freezerctl-py
  proto/                     # .proto files (source of truth)
  tests/
    unit/  property/  integration/  fuzz/  e2e/
    backend_conformance/     # parameterized over backends
  ```
  Add empty `CMakeLists.txt` per directory; create `proto/.gitkeep`, etc.

- [ ] **B7. SBOM generation in CI.** Emit a CycloneDX (or SPDX) SBOM for
      every tagged release and attach it to the GitHub release artifacts.
      Run advisory scanning of the SBOM against the OSV database in CI.
  - **⚠ Watch:** the SBOM must list both Conan-resolved dependencies and
    any vendored sources (e.g. submodules, copy-pasted headers).

- [ ] **B8. Dependency vulnerability scanning.** Add Dependabot or
      Renovate config under `.github/`. Advisory in M0–M2; from M3
      onwards block PR merges on `high` or `critical` advisories.
      Document the triage process in `SECURITY.md`.

---

## Section C — Domain core & storage abstraction (M0 → M1)

> **Cross-module priority:** Section C must land before Sections D, E, F.
> Anything in those sections that touches persistence must go through the
> interfaces defined here.

- [x] **C1. Domain value types** (`src/core/`). Pure C++, no I/O, no DB.
  - [x] **C1.1.** Strongly-typed IDs: `LabId`, `UserId`, `SampleId`, etc.,
        each a thin wrapper around a UUID. Compile-time errors if mixed.
  - [x] **C1.2.** `Money`-style typed quantities for volume/mass with unit
        enum (`mL`, `µL`, `mg`, `g`); arithmetic forbidden across units.
  - [x] **C1.3.** `Timestamp` (UTC, microsecond precision). All times stored
        and transmitted as UTC; conversion to local only at display time.
  - [x] **C1.4.** Domain enums: `SampleStatus`, `CheckoutAction`,
        `RoleKind`, `ContainerKind` (compartment/shelf/rack/drawer/custom).
  - **Tests:** unit tests covering equality, ordering, invalid-construction
    rejection, JSON round-trip.

- [x] **C2. `IStorageBackend` interface** (`src/storage/IStorageBackend.h`).
  Pseudocode is in PRD §5 — turn it into the real header.
  - [x] **C2.1.** Declare `IStorageBackend`, `ITransaction`, `IRepository<T>`,
        `Capabilities`, `IsolationLevel`, `SchemaVersion`.
  - [x] **C2.2.** Define a typed query spec DSL (`Query<Sample>::where(...)
        .order_by(...).limit(...)`). Must support: equality, range,
        IN-list, JSON-path equality (for custom fields), pagination, sort,
        soft-delete-aware default filter.
  - [x] **C2.3.** Define `BackendError` hierarchy
        (`UniqueViolation`, `ForeignKeyViolation`, `SerializationFailure`,
        `Unavailable`, etc.) so callers can react portably.
  - **⚠ Watch:** the abstraction MUST allow Postgres-only features (RLS,
    JSONB GIN indexes, LISTEN/NOTIFY) behind a `Capabilities` flag without
    leaking dialect into callers. Do not put SQL strings in this header.

- [x] **C3. Backend conformance test suite** (`tests/backend_conformance/`).
  Parameterized GoogleTest fixtures runnable against any backend impl.
  Adding a new backend = passing this suite.
  - [x] **C3.1.** CRUD on every entity (insert, find, update, soft-delete).
  - [x] **C3.2.** Transaction isolation: serializability tests with two
        concurrent transactions on overlapping rows.
  - [x] **C3.3.** Box-position uniqueness invariant under concurrent
        placement (50 threads × 1000 placements; zero double-bookings).
  - [x] **C3.4.** Soft-delete visibility: tombstoned rows excluded from
        default queries but findable via `include_tombstoned()`.
  - [x] **C3.5.** Audit hook: every mutating call appends to `audit_event`
        within the same transaction; commit fails if audit append fails.
  - [x] **C3.6.** Migration: forward-migrate, then downgrade, then
        forward-migrate again, against representative seed data.
  - **⚠ Watch:** these tests will be re-run by every storage backend
    contributor in the future. Fixtures must NOT bake in dialect-specific
    setup beyond what `IStorageBackend::migrate_to_latest()` performs.

- [~] **C4. SQLite reference backend** (`src/storage/sqlite/`).
  - [x] **C4.1.** `SqliteBackend` implementing `IStorageBackend`. Use
        SQLite ≥ 3.45 with WAL mode, foreign keys ON, busy-timeout 5 s,
        json1 extension required.
  - [x] **C4.2.** Schema migrations under `src/storage/sqlite/migrations/`,
        named `0001_init.sql`, `0002_*.sql`. Migrations are atomic and
        recorded in `schema_migrations` table.
  - [ ] **C4.3.** Generated columns + indexes on JSON paths declared
        `indexed: true` in `CustomFieldDefinition`. Re-generate when a
        definition changes.
  - [x] **C4.4.** Pass full conformance suite from C3.
  - **⚠ Watch:** SQLite is single-writer. Document this as a hard
    deployment limit. Do NOT silently serialize app-level writers around
    a mutex — let the backend return `Unavailable` on contention so callers
    can retry with backoff.

- [~] **C5. PostgreSQL reference backend** (`src/storage/postgres/`).
  - [x] **C5.1.** `PostgresBackend` using libpqxx; connection pool sized by
        config. Use Postgres ≥ 16.
  - [x] **C5.2.** (Done inline in `PostgresBackend.cc`, not a `migrations/` dir.)
        Migrations under `src/storage/postgres/migrations/` with
        the same numbering scheme as SQLite. Migration runner refuses to
        proceed if SQLite and Postgres migration counts diverge.
  - [x] **C5.3.** Row-Level Security policies on every domain table keyed
        on `app.current_user_id` and `app.current_lab_ids` settings set
        per-connection by the auth layer (see D3).
  - [ ] **C5.4.** JSONB columns for `custom_fields_json`; GIN indexes on
        fields marked indexable.
  - [x] **C5.5.** Pass full conformance suite from C3. (CI `postgres:16` service.)
  - **⚠ Watch:** RLS policies must `FORCE` and apply to table owners too,
    or app-account-by-default will bypass them. Add a test that flips the
    session vars to a non-member's lab and asserts queries return zero rows.

- [ ] **C6. Migration test harness** (`tests/migrations/`). For each
      migration: load representative pre-migration seed data, run up,
      assert post-state, run down, assert pre-state restored. PR
      cannot merge if a new migration is missing this test.

---

## Section D — Domain entities (M1)

> Each entity below is one task. Each task delivers: types in `core/`,
> repository methods on the backend, conformance-suite coverage, validation
> rules, and CLI commands in `freezerctl` for create/list/inspect.

- [ ] **D1. `Lab` & `User` & `LabMembership`.** First entities; everything
      else is scoped by `lab_id`.
  - [x] **D1.1.** Schema + types + repos.
  - [x] **D1.2.** Email-uniqueness enforced at DB level.
  - [ ] **D1.3.** First-run wizard creates the initial `SystemAdmin` user
        and the first `Lab`.
  - **⚠ Watch:** every later entity will carry `lab_id`; do NOT skip it on
    "lab-agnostic" tables (sessions, audit) — those still log `lab_id` for
    forensic queries.

- [x] **D2. `Role`, `Permission`, `RolePermission`, `LabMembership.role_id`.**
      Seed the five built-in roles. Permission table is a static catalog
      seeded from `src/core/permissions.h` (single source of truth).
  - **⚠ Watch:** `LabMembership.scope_filters_json` is enforced *additively*
    — a member with role `Member` + scope `freezer in {F1, F2}` may only
    write to F1 or F2. Decisions on scope syntax will affect E1 (RBAC
    middleware), so finalize the JSON schema here.
  - Scope-filter JSON schema is finalized: closed key set
    `{freezer_in, project_in, item_type_in}` of string-id arrays;
    validator rejects unknown keys. See `core::validate_scope_filter`.

- [x] **D3. `Freezer` and `StorageContainer`** (recursive). Adjacency-list
      with ordered children. Capacity hints are advisory only.

- [x] **D4. `ContainerType` and `BoxType` + `Position`.** A `BoxType`
      carries a list of positions; each position has `(label, row, col,
      optional z, accepts: list<size_class>)`.
  - [x] **D4.1.** Validation: position labels unique within a BoxType;
        `accepts` is non-empty; size_class tokens must reference an
        existing `ContainerType.size_class` in the same lab.
  - [x] **D4.2.** Standard library of BoxType templates (9×9 cryobox,
        10×10 cryobox, 96-well rack, the Eppendorf 3×3+2×2 mixed box) as
        seed JSON files importable by lab admins.

- [x] **D5. `Box`** (an instance of a `BoxType` placed in a
      `StorageContainer`).
  - **⚠ Watch:** `Box.parent_storage_container_id` cascades on delete only
    via tombstone propagation. **Never hard-cascade physical containers —
    you'd lose audit history of where samples used to live.**

- [x] **D6. `ItemType` (hierarchical) + `CustomFieldDefinition`.**
  - [x] **D6.1.** `ItemType` adjacency-list; cycle prevention enforced at
        write time AND by a DB-level trigger (Postgres) / app guard (SQLite).
  - [x] **D6.2.** `CustomFieldDefinition.scope = (lab_id, scope_kind,
        item_type_id_nullable)`. Inherited from ancestors; a descendant may
        narrow validation but not remove a required ancestor field.
  - [x] **D6.3.** Validator engine (`src/core/custom_field_validator.h`)
        that turns a definition into a per-write check. Supported types:
        `string`, `int`, `float`, `bool`, `date`, `datetime`, `enum`,
        `reference` (FK to another sample by ID).
  - [x] **D6.4.** **`is_phi: true`** flag routes the field through the
        encryption layer (Section H) and the redaction layer (Section L).
        (Schema and type support implemented; enforcement deferred to H3.)
  - **⚠ Watch:** field key uniqueness is enforced per `(lab_id, scope_kind,
    item_type_id, key)`, NOT globally. Two labs may have a `patient_id`
    field with different validation rules.

- [x] **D7. `Sample` + `Project` + `SampleProject` + `CheckoutEvent`.**
  - [x] **D7.1.** Schema with constraints:
    - `unique (box_id, position_label) WHERE status IN ('active',
      'checked_out')` — partial unique index, the core no-double-booking
      invariant.
    - `ContainerType.size_class ∈ Position.accepts` enforced in the
      placement RPC and in a DB trigger (Postgres) / app guard (SQLite).
  - [x] **D7.2.** Lifecycle state machine: `active → checked_out → active`,
        `active → depleted`, `* → tombstoned` (soft-delete), `tombstoned →
        hard-deleted` only by `SystemAdmin` with `sample.delete_hard`.
  - [x] **D7.3.** Volume/mass tracking optional per item type. Each
        `CheckoutEvent` may carry a `volume_delta`; reaching zero
        auto-marks `depleted`.
  - [x] **D7.4.** Parent–child lineage:
    - Child is independent — depleting parent does NOT deplete children;
      depleting child does NOT affect parent.
    - Lineage is preserved across soft-delete; UI shows the "parent: X
      (depleted)" hint.
  - [ ] **D7.5.** Move atomicity: `move(sample_id, dst_box, dst_pos)` is
        ONE transaction. Property-test: 50 threads moving the same
        sample concurrently — exactly one succeeds.
  - **⚠ Watch:** PHI-tagged custom fields go in `phi_fields_enc_json`,
    NOT in `custom_fields_json`, even though they share a definition
    table. The split exists so an unauthorized read still returns the
    non-PHI fields.

- [x] **D8. `ShareRequest`** (cross-lab sharing).
  - State machine: `pending → approved | rejected | revoked`. Approval
    requires three signatures (source lab admin + target lab admin +
    system admin). All transitions audited.
  - **⚠ Watch:** approving a share request grants read-only visibility
    into the scoped subset to the target lab's members. The query layer
    must compute "visible labs" as `{home_lab} ∪ {labs sharing TO me}`
    and apply this both in the app guard AND in the Postgres RLS policy.

- [~] **D9. `Session` entity & device tracking.** PRD §7.1 requires
      server-side opaque sessions but no schema task currently exists.
  - [x] **D9.1.** Schema: `(id, user_id, token_hash, created_at,
        last_seen_at, ip_inet, user_agent, revoked_at)`. Token stored
        as Argon2id hash; only the prefix is plaintext for lookup.
        Also includes `ApiToken` (id, user_id, lab_id, name, scope_json,
        token_hash, token_prefix, expires_at, revoked_at).
  - [~] **D9.2.** (`ListSessions` + `RevokeSession` shipped; revoke-all 🔲.)
        RPCs: `list_my_sessions`, `revoke_session(id)`,
        `revoke_all_sessions` ("log me out everywhere"). All audited.
        Deferred to F2 (gRPC layer).
  - [x] **D9.3.** Auto-expire idle sessions (configurable; default 12 h
        idle / 7 d absolute). Last-seen update rate-limited in auth
        middleware (E3); repository stores whatever it is given.
  - **⚠ Watch:** revoking a session must take effect within one
    request — caches keyed on session id must consult the revocation
    flag, not just TTL.

- [ ] **D10. `Lab.is_phi_enabled` toggle workflow.** PRD §4.1 names
      the field but no task covers **enabling PHI mode on a lab that
      already has samples**.
  - [ ] **D10.1.** RPC `lab.enable_phi`: validate that no PHI-tagged
        custom-field column already contains data (legacy plaintext);
        if it does, refuse with a structured migration plan.
  - [ ] **D10.2.** On enable, flip the flag, start enforcing PHI
        redaction in logs, and lazily provision per-record DEKs on
        first PHI write. Disabling PHI is **not supported** without a
        SystemAdmin escape hatch (audit-loud).
  - [ ] **D10.3.** Double audit row: one `lab.config_changed`, one
        `phi.mode_enabled` with the SystemAdmin actor.
  - **⚠ Watch:** SystemAdmin-only RPC. Document in the operator
    handbook that disabling PHI is destructive to compliance posture.

---

## Section E — AuthN / AuthZ / Audit (M2)

- [x] **E1. `IAuthProvider` interface** (`src/auth/IAuthProvider.h`) and
      session model. Sessions are server-side, opaque token in an
      `HttpOnly; Secure; SameSite=Strict` cookie for browser clients;
      Bearer for API clients.

- [x] **E2. `LocalAuthProvider`.** Argon2id (params: 64 MiB, 3 iterations,
      4 parallelism — review against current OWASP guidance before 1.0).
      TOTP (RFC 6238) enforced when user has `totp_secret_enc` set.
      Account lockout (in-memory): 5 failures → 1-hour lock, configurable.
  - [ ] **E2.1.** Password reset flow with single-use, 30-min, hashed tokens.
        Requires `IEmailSender` (Section O) and a new migration (0013).
  - [ ] **E2.2.** DB-backed account lockout: persist failure count + locked_until
        in a `login_attempts` table (migration 0013). Survives server restart.

- [x] **E3. RBAC middleware** (`src/rpc/auth_middleware.cc`).
  Every RPC declares its required permission via a static annotation.
  Middleware:
  1. Validates session/token.
  2. Computes effective permissions = (role perms) ∩ (scope filters).
  3. Sets Postgres session vars (`app.current_user_id`,
     `app.current_lab_ids`) — **this is what makes RLS work; missing this
     is a P0 bug**.
  4. Rejects with `PermissionDenied` if the RPC's required perm isn't held.
  - **⚠ Watch:** every new RPC MUST be added to a static `[ ]`-list of
    `(rpc, required_perm)` pairs. CI test asserts no RPC reaches the
    handler without going through the middleware.

- [ ] **E4. API tokens.** Per-user, per-scope, expiring (default 30 d).
      Stored as Argon2id hashes; plaintext shown once at creation. Token
      prefix is plaintext for identification (`fmgr_pat_<uuid>_<secret>`).
      Per-token rate limit configurable per role.

- [~] **E5. Audit log** (`src/audit/`).
  - [x] **E5.1.** `audit_event` schema with `prev_hash`, `this_hash`.
        Insert is the only allowed write; no UPDATE, no DELETE, enforced
        with a DB trigger.
  - [x] **E5.2.** (PR #26) Canonical-JSON serializer (RFC 8785 / JCS) for
        `before_json`/`after_json` so hashes are reproducible.
  - [x] **E5.3.** (PR #13) Hash-chain verifier CLI: `freezerctl audit verify`
        walks the chain and reports the first divergence.
  - [ ] **E5.4.** Nightly checkpoint job: HMAC-SHA-256 the latest hash
        with a key sourced from `IKmsProvider` (Section H), persist the
        checkpoint to a separate `audit_checkpoint` table.
  - [x] **E5.5.** PHI-read audit kind: a distinct event when a user reads
        a PHI-tagged field; includes the field key but NOT the value.
  - **⚠ Watch:** audit append happens in the same transaction as the
    mutating write. Conformance test C3.5 must pass for every backend.

- [ ] **E6. OIDC, LDAP, mTLS providers** (M7 polish, but interface in M2):
      stub implementations that throw `NotImplemented` so production
      compiles; real impls land in M7. Document config schema now so
      ops docs aren't churned later.

- [ ] **E7. Audit browse / query RPC + UI.** Paginated query by
      `(actor_user_id, entity_kind, entity_id, time_range, action,
      lab_id)`. Read access requires `audit.read`; CSV export requires
      `audit.export` and emits a chain-of-custody-grade signed file
      (PRD §13). Streaming live audit feed for admins (per F7).
  - **⚠ Watch:** each audit-browse query is itself audited (meta-audit)
    so retroactive forensics is possible. Avoid logging the *value* of
    PHI-read audit rows in the browse response unless the caller also
    holds `phi.read`.

---

## Section F — RPC layer & client transports (M3)

- [x] **F1. `.proto` definitions** under `proto/fmgr/v1/`. One file per service
      (`auth.proto`, `lab.proto`, `sample.proto`, `audit.proto`, etc.; 10 files).
      Versioned `package fmgr.v1;`. **Source of truth — never edit generated
      code.**

- [x] **F2. gRPC server** in `src/server/`. All 9 services implemented. Each RPC
      handler:
  1. Goes through E3 middleware.
  2. Opens a transaction via `IStorageBackend`.
  3. Performs work via the typed repos.
  4. Writes audit row in the same transaction.
  5. Commits.

- [~] **F3. REST/JSON gateway** (`src/rest/`). Drogon HTTP listener inside
      `freezerd`, forwarding to the gRPC services over `Server::InProcessChannel`
      — reuses the RBAC gate + audit + transactions with no logic duplicated.
      JSON↔proto via proto3 JSON mapping (`JsonProtoMapping`); gRPC status →
      HTTP via `RestErrorTranslation`. Verb-style routes `/api/v1/<service>/<verb>`.
  - [x] Auth + Session + Lab + Sample wired end-to-end (login → bearer → RBAC →
        unary CRUD → JSON), positive/negative authz + missing-bearer + REST e2e
        tests green.
  - [x] Fan out the remaining 5 services (Box, ItemType, Role, Audit, Share) —
        mechanical copies of the route pattern.
  - [~] Streaming RPCs bridged to SSE / WebSocket (live sample-list, audit feed,
        bulk-import progress). Audit feed + `/api/v1/sample/watch` routes exist;
        bulk-import progress 🔲.
  - **⚠ Watch:** the REST gateway is what the React SPA and Python
    client speak. Any breaking change to a `.proto` must increment the
    `v1` package label.

- [ ] **F4. TLS configuration**. (In progress: PR #27, gRPC TLS.) TLS 1.3 only; HSTS; modern ciphers.
      Self-signed cert for dev, documented refusal-to-start without a
      cert in production mode (`FMGR_ENV=production`).

- [~] **F5. Health/metrics endpoints**. (Routes + tests exist; default
      localhost binding of `/metrics` unverified.) `/health` (liveness + readiness),
      `/metrics` (Prometheus). Both unauthenticated; `/metrics` SHOULD
      be bound to localhost or behind reverse-proxy ACL by default.

- [~] **F6. Qt 6 desktop client** (`src/qt/`). gRPC client. Built as modules
      M0–M5; headless logic (service clients + table/tree/grid models +
      scan controller) is unit-tested without a QApplication, GUI widgets are
      thin glue covered by manual e2e.
  - [x] **F6.1.** Login screen + TOTP prompt + session manager (wired into the
        app shell; Connect → LoginDialog → AuthService → SessionManager →
        authenticated splitter). Keychain persistence still 🔲.
  - [x] **F6.2.** Sample browser — virtualized `QTableView` with cursor paging
        (`fetchMore`) for 100k+ rows + structured filters (status / box /
        item-type / barcode). Full-text + custom-field filter 🔲 (needs server
        `ListSamples` support, L10).
  - [x] **F6.3.** Box view — `QGraphicsView` grid; drag-and-drop placement via
        `MoveSample`; server rejection surfaces as a "size mismatch" toast.
  - [x] **F6.4.** Bulk check-in/out with barcode-scanner focus mode (HID field
        auto-submits on Enter; `ListSamples(barcode)` → `CheckoutSample`).
        Configurable inactivity-gap auto-submit still 🔲.
  - [x] **F6.5.** (PR #24) CSV import wizard (dry-run first; show validation report;
        confirm; import). **Server side done** — `SampleService.ImportSamples`
        RPC (gRPC + REST `/api/v1/sample/import`), transactional + dry-run,
        reuses the CLI importer core. Remaining: the Qt wizard
        (`SampleServiceClient::importSamples` + file picker → dry-run report →
        confirm).
  - [x] **F6.6.** CSV export from the sample list view (`ExportSamplesCsv`).

- [ ] **F7. Live updates over streaming RPCs.** Push sample-list deltas
      within an open freezer view; push admin audit feed in real time;
      push bulk-import progress.

- [ ] **F8. Cursor-based pagination spec.** Every list RPC uses opaque
      cursors, never `offset`. Document the semantic guarantee
      ("stable in face of new inserts; consistent with the snapshot
      timestamp encoded in the cursor"). Cursors are signed so callers
      cannot forge them. Default page size 50; max 500.
  - **⚠ Watch:** custom-field sort orders complicate cursors —
    the cursor must encode `(sort_key_value, primary_key)` to stay
    deterministic across ties.

- [ ] **F9. Bulk-operation RPCs.** `bulk_move`, `bulk_check_out`,
      `bulk_check_in`, `bulk_tombstone`. Streaming progress: per-row
      result so UIs can render partial-success reports. Default mode
      is "best-effort per row" (each row in its own transaction);
      caller may opt into "all-or-nothing" (single transaction, max
      1000 rows, hard 30 s timeout). Idempotency-key required.

---

## Section G — Web UI (M4)

- [ ] **G1. SPA scaffold** in `src/web/` with Vite + React + TypeScript.
      Component lib: TanStack Table for grids; defer styling library
      decision until G2 reveals real needs.

- [ ] **G2. Auth flows**: login, OIDC redirect, TOTP, password reset,
      session expiry handling. Tokens never stored in `localStorage`;
      use `HttpOnly` cookie set by the REST gateway.

- [ ] **G3. Feature parity with Qt** for the core flows in F6, except
      USB scanner (browser limitation; fall back to manual paste with
      a focused input).

- [ ] **G4. Dashboards**: freezer fill heatmap, sample-age histogram,
      check-out activity. Server returns aggregated data via dedicated
      RPCs — **do NOT** ship raw row dumps to the client for aggregation.

---

## Section H — Cryptography, PHI mode, KMS, Backups (M5)

- [x] **H1. `IKmsProvider` interface** (`src/kms/IKmsProvider.h`):
      `wrap_dek(dek) → wrapped`, `unwrap_dek(wrapped) → dek`.

- [~] **H2. KMS implementations**:
  - [~] **H2.1.** (Shipped; production-refusal behavior unverified.)
        `EnvVarKms` — for tests/dev only. Refuses to load
        if `FMGR_ENV=production`.
  - [x] **H2.2.** `OsKeyringKms` — systemd-creds backed; default for
        production single-server deployments.
  - [ ] **H2.3.** `VaultKms` — HashiCorp Vault transit engine;
        configurable mount path and key name.

- [x] **H3. Field-level PHI encryption.** Per-record DEK, generated at
      first-write, stored wrapped in the row. AEAD: libsodium
      `crypto_secretbox` (XChaCha20-Poly1305). Associated data binds
      ciphertext to `(lab_id, sample_id, field_key)` so cut-and-paste
      across rows fails to decrypt.
  - **⚠ Watch:** the `phi.read` permission gates access *before*
    decryption; do not decrypt and then check perm — this leaks the
    plaintext into the process memory.

- [ ] **H4. PHI redaction in logs** (`src/core/redact.h`). Type-level:
      a `PhiString` newtype that won't compile into `spdlog` formatters
      without an explicit `redacted()` call. Every PHI field flows
      through `PhiString`. CI lint forbids `fmt::format` of `PhiString`.

- [ ] **H5. Backup runner.**
  - [ ] **H5.1.** Postgres path: `pg_basebackup` baseline + WAL
        archiving for PITR. Encrypt with backup key (separate from
        master key) using libsodium streaming API.
  - [x] **H5.2.** SQLite path: `sqlite3_backup` hot copy + nightly
        rotation. Same encryption. (In-server `BackupScheduler` thread
        drives `backup::run_backup_tick`: create-if-due + GFS retention
        prune (`RetentionPolicy`, default 30 daily / 12 monthly / 7
        yearly) + weekly drill; `backup.create`/`backup.prune`/
        `backup.drill` audit events. Enabled by `FMGR_BACKUP_DIR`.)
  - [x] **H5.3.** `freezerctl backup run | list | restore` CLI.
        (`run` = one scheduler tick on the real clock; `list` enumerates
        a backup dir newest-first without decrypting; `restore` ✅ from
        M5 slice 3.)
  - [x] **H5.4.** Weekly restore-drill job: pick a random recent
        backup, restore into a temp DB, run integrity checks, audit
        the result. Failures page (or email) the system admin.
        (`BackupScheduler` runs the drill via `run_backup_verify`;
        random pick via seeded RNG, cadence tracked by a `.last_drill`
        marker; failure → spdlog error-level page + `backup run` exit 1.)
  - **⚠ Watch:** backup key MUST live separately from the master KEK.
    Document this in the operator runbook and assert it at server
    startup if both are configured to the same source.

- [ ] **H6. Key rotation procedures.** Three keys rotate independently:
      master KEK, backup key, audit-checkpoint HMAC key.
  - [ ] **H6.1.** Rotate master KEK: re-wrap every existing
        per-record DEK with the new KEK; old wrapped DEKs remain
        decryptable for a configurable grace window (default 30 d) so
        a botched rotation can be reverted. Emit a distinct
        `kms.master_rotated` audit event.
  - [ ] **H6.2.** Rotate backup key: future backups encrypt under the
        new key; restore tooling still accepts old keys for the
        retention window. Document operator-side coordination.
  - [ ] **H6.3.** Rotate audit-checkpoint HMAC key: persist all keys
        ever used (`audit_checkpoint_key_history`) so verifier can
        check old checkpoints; emit `audit.checkpoint_key_rotated`.
  - [ ] **H6.4.** `freezerctl key rotate {master|backup|checkpoint}`
        CLI; runs as a single auditable operation with a confirmation
        prompt that quotes the operator's name and the key name.

- [ ] **H7. Master-key vs backup-key sameness check.** Refuse to
      start if `IKmsProvider` resolves both keys to the same KMS
      path / env var / Vault key. PRD §8 + §14 require strict
      separation. Test: configure the same key for both and assert
      the server exits with a non-zero code and a clear message.

---

## Section I — Public API & external client (M6)

- [ ] **I1. `freezerctl-py`** (`src/py/`): thin Python wrapper over the
      REST gateway; bundles a Jupyter quick-start notebook with example
      plots (fill histogram, sample-age distribution, check-out volume).
      Auth via API token from environment variable.

- [ ] **I2. Token-management UI** (Qt + Web): create / list / revoke
      tokens; show plaintext exactly once at creation.

- [ ] **I3. Cross-lab share workflow UI**. Lab admin creates a share
      request → target lab admin reviews and approves/rejects → system
      admin co-signs → samples become read-visible to target lab.

- [ ] **I4. API rate limiting**. Configurable per role (default
      `Member` 60 req/min, `LabAdmin` 300 req/min, `ApiClient`
      inherits from owning user). `429 Too Many Requests` with
      `Retry-After` header.

---

## Section J — Hardware abstraction (low priority, anytime ≥ M3)

- [ ] **J1. Interfaces in `src/hw/`**: `IBarcodeScanner`,
      `ILabelPrinter`, `IRfidReader`, `ITemperatureSensor`. Reference
      impl: `HidKeyboardScanner` only.

- [ ] **J2. Plugin loader**: at server start, dlopen any `.so` files in
      `/etc/freezerd/plugins/` and register hardware adapters they
      expose. Document an example skeleton in `doc/plugins.md`.

---

## Section K — Packaging & release (M7)

- [ ] **K1. Debian/Ubuntu `.deb` package** with systemd unit
      (`freezerd.service`), default config in `/etc/freezerd/`,
      logrotate config, dedicated `freezerd` user.
- [ ] **K2. RPM package** (Fedora/RHEL/Rocky).
- [ ] **K3. Official Docker image** (Debian-slim base, multi-stage,
      runs as non-root). Publish to GHCR.
- [ ] **K4. Reproducible-build verification** in CI: build twice, diff
      the binary, fail on differences.
- [ ] **K5. First-run wizard** (CLI + web): interactively create system
      admin, first lab, master key (or wire to KMS), TLS cert.
- [ ] **K6. Operator handbook** (`doc/operations.md`): install, upgrade,
      backup/restore drill, key rotation, incident response.
- [ ] **K7. External security review** before tagging 1.0. Scope: auth,
      RBAC, crypto, audit chain, RLS bypass attempts.

---

## Section L — Cross-cutting infrastructure (M0 + ongoing)

- [~] **L1. Structured logging** (JSON sink + required fields shipped; `redact()` waits on H4) (spdlog → JSON sink to stdout).
      Required fields on every record: `ts`, `level`, `request_id`,
      `actor_user_id` (nullable), `lab_id` (nullable), `event`. PHI
      goes through `redact()` (H4).

- [~] **L2. Request-id propagation**. (REST → gRPC → audit row shipped; every-log-line unverified.) Generate at the RPC entry; carry
      through to the audit row and every log line.

- [ ] **L3. OpenTelemetry tracing** behind an env-var flag
      (`FMGR_OTLP_ENDPOINT`). Disabled by default.

- [ ] **L4. Configuration loader**. TOML at `/etc/freezerd/freezerd.toml`,
      env-var overrides, `--config` CLI flag. Validate at startup; fail
      fast with clear errors.

- [ ] **L5. Fuzz harnesses** (libFuzzer). Targets: RPC parsers
      (per-message), custom-field validator, CSV importer, audit
      canonical-JSON serializer. Run nightly in CI for ≥ 30 min each.

- [ ] **L6. Test coverage gates** (advisory M0–M2; required ≥ 80 % from M3).

- [ ] **L7. End-to-end smoke test** (`tests/e2e/`): start server in a
      container, run a Python script via `freezerctl-py` that creates a
      lab, freezer, box, samples, performs check-out, exports CSV,
      verifies audit chain, takes a backup, wipes the DB, restores, and
      confirms identical state. Required green before any release tag.

- [ ] **L8. Performance benchmark suite** (`tests/benchmark/`). Google
      Benchmark targets for hot paths: sample placement, list-with-filter,
      audit append, canonical-JSON serializer, custom-field validator.
      Track p50/p95/p99 in CI; alert on >20% regression vs. the prior
      release tag. **Distinct from concurrency stress (M-section)** —
      benchmarks measure per-op cost, not invariant safety.

- [ ] **L9. `freezerctl` CLI conventions** (`doc/cli.md`). Spec:
      command tree (`freezerctl <noun> <verb>` — e.g. `sample list`,
      `audit verify`, `key rotate`); `--json` machine-readable output
      mode; exit-code policy (`0` ok, `1` user error, `2` system
      error, `3` auth, `4` conflict); `--quiet`/`--verbose`; bash +
      zsh completion scripts. CI lint rejects new commands without
      a help string and an exit-code declaration.

- [ ] **L10. Full-text search backend.**
  - [ ] **L10.1.** Postgres impl: `tsvector` columns + GIN indexes on
        sample name, label, and indexable text custom-fields.
  - [ ] **L10.2.** SQLite impl: FTS5 virtual table mirroring the same
        surface; rebuilt on relevant inserts via triggers.
  - [ ] **L10.3.** Query DSL: add `.match("query")` predicate behind a
        `Capabilities.full_text_search` flag; backends without FTS
        return `UnsupportedOperation`.
  - **⚠ Watch:** PHI-tagged custom-fields must NEVER be indexed — the
        FTS index becomes a plaintext leak vector. Validator must
        reject `is_phi=true AND indexed=true`.

---

## Section N — Documentation completeness (M0 + ongoing)

> The PRD repeatedly says things are "documented separately" or "in
> deployment docs" without naming a task to write them. This section
> closes that gap.

- [ ] **N1. Lab-member onboarding guide** (`doc/users/quickstart.md`):
      log in, find a sample, check it out, scan a barcode, run and
      save a search, export CSV. Screenshots from the Qt and Web
      clients side-by-side.

- [ ] **N2. Auto-generated REST API reference.** Generate from
      `.proto` (F1) + REST gateway (F3) annotations into `doc/api/`.
      Published with every release tag. CI fails if `.proto` files
      change without regenerating the doc.

- [ ] **N3. Schema reference doc** (`doc/schema/`). Auto-generated
      from migration SQL into a per-table reference, including a
      Mermaid ER diagram refreshed on each migration. Re-run as a
      pre-commit hook for migration authors.

- [ ] **N4. Architecture Decision Records (ADRs)** at `doc/adr/` with
      a numbered template. **Mandatory ADR** for any new pluggable
      interface (`IStorageBackend`, `IAuthProvider`, `IKmsProvider`,
      `IEmailSender`, `I*Hardware`) or any cross-module protocol
      change. Code review checklist references the ADR list.

- [ ] **N5. Custom-field guide for lab admins**
      (`doc/users/custom-fields.md`): how inheritance through `ItemType`
      works, how to mark a field PHI, how to add validation, what
      happens when a constraint is tightened on existing data.

- [ ] **N6. Trademark & branding policy** (`TRADEMARK.md`). PRD §18
      reserves the project name and logo. Document the policy:
      reuse for forks discouraged, "powered by FreezerManager"
      attribution allowed, logo SVG license terms.

---

## Section O — Notifications & Email (M2 → M5)

> Password reset (E2.1), share approvals (D8/I3), account lockout
> (E2.2), backup failure (H5.4), and restore-drill failure (K6) all
> need email but no transport abstraction is defined. This section
> adds it.

- [ ] **O1. `IEmailSender` interface** (`src/notify/IEmailSender.h`):
      `send(EmailMessage)` returning a delivery handle. Implementations:
  - [ ] **O1.1.** `SmtpSender` — production; STARTTLS or implicit TLS;
        retries with exponential backoff; bounce handling deferred.
  - [ ] **O1.2.** `LogSender` — dev only; writes the rendered email
        to `/tmp/fmgr-mail/` and stdout. Refuses to load if
        `FMGR_ENV=production`.
  - [ ] **O1.3.** `MockSender` — tests; captures sent messages in an
        in-memory list for assertions.

- [ ] **O2. Email template engine** (`templates/email/`). Mustache or
      fmt-based. Templates: `password_reset`, `share_request`,
      `share_approved`, `share_rejected`, `account_locked`,
      `backup_failed`, `restore_drill_failed`, `phi_mode_enabled`.
  - **⚠ Watch:** PHI must NEVER appear in template variables. Use
    `redact()` at the binding site; lint rule rejects passing a
    `PhiString` (H4) to the template renderer.

- [ ] **O3. In-app notification entity.** Schema:
      `(id, lab_id, recipient_user_id, kind, payload_json,
      created_at, read_at)`. Server-streamed RPC for real-time
      delivery to Qt + Web. Generated alongside the email send so the
      user sees the alert even with broken SMTP.

- [ ] **O4. Optional webhook delivery** (`IWebhookSender`). Lab
      admins configure HTTPS endpoints to receive `share_approved`,
      `audit_digest`, or `backup_status` events. Each delivery
      signed `HMAC-SHA-256(per_webhook_secret, body)` in an
      `X-Fmgr-Signature` header. Retry with backoff; dead-letter
      after 24 h.

- [ ] **O5. Email transport configuration.** TOML section under
      `[notify.email]`; SMTP credentials sourced via
      `IKmsProvider` (never plaintext in config). Refuse to start
      if `FMGR_ENV=production` and the configured sender is
      `LogSender` or has no credentials.

---

## Section P — Internationalization & Accessibility (M3 → M7)

> PRD §1.2 commits to "UTF-8 everywhere; tr()/i18n() wrapped strings;
> English-only at v1." Without a scaffolding task, v1 will ship
> un-i18n-ready and retrofitting is expensive.

- [ ] **P1. UTF-8 audit at the data layer.** Migration runners assert
      SQLite `PRAGMA encoding='UTF-8'` and Postgres `LC_COLLATE` /
      `LC_CTYPE` are UTF-8 locales. Refuse to migrate against a
      non-UTF-8 DB with a clear error. Test: run against a
      `LATIN1`-encoded Postgres and assert refusal.

- [ ] **P2. Qt i18n scaffolding.** Every user-facing string wrapped
      in `tr()`. Integrate `lupdate` / `lrelease` into CMake; commit
      `src/qt/locales/en_US.ts`. CI lint (custom clang-tidy or
      regex check) forbids non-tr()-wrapped string literals in
      widget constructors and `setText()` calls.

- [ ] **P3. Web i18n scaffolding.** `react-i18next` integrated;
      `src/web/locales/en.json` committed; ESLint rule
      `i18next/no-literal-string` enabled. Translation keys follow
      `feature.context.string-id` convention.

- [ ] **P4. WCAG 2.1 AA targeting for Web UI.** Run `axe-core` as a
      CI check on the SPA; require Lighthouse a11y score ≥ 90 on the
      core flows (login, sample browser, box view, check-out).
      Advisory until G3 lands; blocking after.

---

## Section M — 1.0 release gates (do not tag 1.0 until all green)

- [ ] All abstract backend tests pass on SQLite and Postgres.
- [ ] ASan + UBSan + TSan builds green.
- [ ] Concurrency stress: 50 simulated members, 10k placements, zero
      invariant violations.
- [ ] 24-hour audit-chain fuzz with random RPC interleavings + process
      restarts; verifier remains green.
- [ ] PHI-mode E2E test: encrypted PHI never appears in plaintext in
      logs, in backups (without backup key), or to users without
      `phi.read`.
- [ ] Backup → wipe → restore → all data + audit chain intact.
- [ ] External security reviewer sign-off (K7).
- [ ] Operator handbook (K6) published.
- [ ] Email delivery: production SMTP path tested end-to-end with a
      real third-party mailbox; bounces and TLS failures handled
      without dropping critical security alerts.
- [ ] Key rotation drill: rotate master KEK, decrypt a PHI sample
      written before rotation; rotate backup key, restore from a
      backup written before rotation.
- [ ] i18n: every UI string extractable to a `.ts` / locale JSON
      file; CI gate confirms no hardcoded English in UI source.
