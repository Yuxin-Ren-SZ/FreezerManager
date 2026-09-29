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
| C-1 | High | Auth | `LocalAuthProvider.cc:752` | Lockout map is in-memory, resets on restart → persist failed-attempt state (DB table + TTL) or external limiter. | first prod tag (M3.5/M4) |
| C-7 | High | Audit | `CanonicalJson.cc:13` | Canonical JSON not RFC 8785; nlohmann version drift can break the audit chain. Pin algorithm + CI golden-vector test, or implement JCS. | before 1.0 (M7) |
| C-3 | Medium | Auth | `LocalAuthProvider.cc:272` | `totp_secret_enc` stored/used plaintext despite `_enc`. Encrypt under master KEK via existing `FieldCipher`. | M5 |
| C-11 | Medium | Server | `GrpcErrorTranslation.h` | `INTERNAL` may leak raw error text (schema probing). Mask in prod, log real error server-side. | M3.5 |
| C-12 | Low | Server | `SampleServiceImpl.cc:47` | `request_id = ""`. Extract `x-request-id` from gRPC metadata → `MutationContext::request_id`. | M3.5 (§17 obs) |
| C-2 | Low | Auth | `validate_token()` | Sessions not IP/UA-bound; no replay detection. Optional IP-binding, off by default (NAT-friendly). | backlog / v2 |
| C-4 | Low | Auth | `SampleServiceImpl.cc:541` | `SoftDeleteSample` two-phase authz bypasses the RPC-registry test. Register a wildcard perm or add `authorize_entity` middleware. | M3.5 |
| C-6 | Low | KMS | `KeyringKms.h:43` | Raw KEK bytes in `std::vector`, no mlock. Wrap in `SecureBuffer` (`sodium_mlock`/`memzero`, optional `mprotect`). | M5 |
| C-8 | Low | Storage | `QuerySqlBuilder.h:216` | Sort direction is the only non-parameterized SQL fragment (enum-gated, safe now). Add `static_assert`/stern comment so a future string-typed sort can't inject. | quick, any slice |
| C-5 | Info | Auth | `Totp.cc:161` | TOTP code compare `==` not constant-time. Switch to `sodium_memcmp` (robust if digit count grows). | quick, any slice |

Resolved since this review: **C-9** — gRPC TLS implemented with fail-closed
cert loading and optional mTLS. **C-10** — the inbound cap existed but its units
were the real bug: split into `max_grpc_memory_bytes` and `max_grpc_threads`, with
a send cap added. Both in `aa44d6d`; see
`doc/handoffs/2026-09-28-27-m35-grpc-tls.md`.

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

- [~] **F4. TLS configuration**. gRPC TLS landed (`aa44d6d`):
      `FMGR_TLS_CERT` / `FMGR_TLS_KEY`, optional mTLS via `FMGR_TLS_CLIENT_CA`,
      fail-closed certificate loading, and `FMGR_ENV=production` refusing to start
      a plaintext listener without `FMGR_REQUIRE_TLS`. Remaining: HSTS, an explicit
      TLS-1.3-only / cipher-suite policy, and the same production guard for the
      REST listener, which still logs "REST (plaintext) listening" — see the
      handoff note.

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

> **Delegation plan (written 2026-09-27).** Each `G<n>.<m>` item is one
> issue and one PR, titled `[G1.2] …`. Every item names the files it owns,
> the locks it needs, what must merge first, and when it is done, so the lead
> can copy it straight into `.github/ISSUE_TEMPLATE/agent-task.md`. The
> suggested wave order is at the end of this section.
> Spec: PRD §10 (Web UI), §9 (the flows it must match), §6 (REST/SSE),
> §7.1 (sessions). The SPA talks only to the REST gateway (F3).

### G-arch — Decisions every G task follows

These are settled so that parallel workers don't each choose differently.
Changing one needs a lead `DECISION` on the issue and an edit here.

1. **Stack:** Vite, React, TypeScript (`strict`), React Router, TanStack
   Query (server state), TanStack Table + TanStack Virtual (grids),
   react-i18next. Styling is CSS Modules with design tokens as CSS custom
   properties. Radix UI primitives are used only where accessibility is hard
   (dialog, menu, popover, tooltip). No other component kit.
2. **Toolchain:** Node 22 LTS (`.nvmrc` + `engines`) and npm with a committed
   `package-lock.json`; install only with `npm ci`. ESLint (typescript-eslint
   strict, react-hooks, jsx-a11y, `i18next/no-literal-string`) and Prettier.
   Tests use Vitest, React Testing Library and MSW.
3. **Dependency budget:** G1.1 installs the whole baseline above. After
   that, adding a runtime dependency needs `lock:deps` (which from G1.1 on
   also covers `src/web/package.json` + `package-lock.json`) and a one-line
   justification in the PR. No CDN assets, hosted web fonts, analytics or
   error-reporting services.
4. **API types are generated, never hand-written.** `@bufbuild/buf` +
   `protoc-gen-es` (both from npm, no system install) generate from
   `proto/fmgr/v1/*.proto` into `src/web/src/gen/`. That directory is
   gitignored, regenerated by `npm run gen`, and never edited. The gateway
   speaks proto3 JSON with **snake_case** names
   (`JsonProtoMapping.cc: preserve_proto_field_names`), int64 as strings,
   enums as names, and it omits default values. Serialize with
   `toJson(…, {useProtoFieldName: true})` and parse with
   `fromJson(…, {ignoreUnknownFields: true})`. `custom_fields_json` and
   similar fields are JSON strings inside the JSON.
5. **Transport:** every unary call is `POST /api/v1/<noun>/<verb>` through one
   wrapper, `src/web/src/api/client.ts`. Live data uses `EventSource` on the
   `…/watch` SSE routes. The SPA is **same-origin only**: `freezerd` (G0.3)
   or a reverse proxy in front of it serves it, and the gateway sends no
   CORS headers.
6. **Auth:** a browser session lives in an `HttpOnly; Secure;
   SameSite=Strict` cookie set by the gateway (G0.1). The session token never
   reaches JavaScript: not in memory, not in `localStorage`, not in URLs.
   Every mutation sends a CSRF header.
7. **PHI in the browser:** API data is never written to `localStorage`,
   `sessionStorage`, IndexedDB or the Cache API, and there is no service
   worker. `localStorage` holds UI preferences only (selected lab id, column
   layout). API payloads are never logged to the console. Logout, session
   expiry and any 401 clear the TanStack Query cache. URLs carry ids only.
   Fixtures and PR screenshots use synthetic demo data
   (`scripts/seed_demo.py`) only.
8. **UI permission gating is for UX, not security.** Controls hide or disable
   based on `WhoAmI` permissions (G0.2). The server remains the only
   enforcement point, so every screen still handles `PERMISSION_DENIED`.
9. **Time:** timestamps travel as UTC micros and are converted to the
   browser's zone only for display (C1.3).
10. **Testing (TDD applies):** write the component or hook test first. MSW
    fakes come from one factory with **per-RPC error injection**
    (`fakeApi({ fail: { 'sample/list': 'PERMISSION_DENIED' } })`), the web
    equivalent of AGENTS.md §6. Every screen tests its `UNAUTHENTICATED`,
    `PERMISSION_DENIED`, conflict (`ALREADY_EXISTS` / `FAILED_PRECONDITION`)
    and network-failure branches, not only the happy path. G5.1 runs
    Playwright against a real `freezerd`.
11. **Layout of `src/web/`:** `src/api/` (client, SSE, route table, query
    hooks), `src/gen/` (generated), `src/app/` (shell, router, providers),
    `src/ui/` (shared primitives), `src/features/<feature>/` (one directory
    per screen, each with its own i18next namespace in
    `locales/en/<feature>.json`), `src/test/` (fakes, render helpers), `e2e/`
    (Playwright). G1.3 registers a placeholder route and nav entry for
    every screen in the route map below, so a feature task edits only its
    own `features/<name>/` directory.
12. **Dev loop:** run `AGENT_SLOT=N source scripts/agent/env.sh`, start
    `freezerd` on the slot's ports, then run `npm run dev` in `src/web/`.
    Vite listens on `127.0.0.1:$FMGR_WEB_DEV_PORT` (5173 + 10·N,
    `strictPort`) and proxies `/api` to `$FMGR_REST_LISTEN` with
    `changeOrigin: false`, so the G0.1 Origin check sees matching hosts.

**Route map** (G1.3 creates all of these as placeholders):

| Route | Screen | Task |
|---|---|---|
| `/login`, `/login/mfa` | Sign-in, TOTP | G2.1 |
| `/` | Home dashboard | G4.2 |
| `/lookup` | Single-handed lookup | G3.5 |
| `/labs/:labId/samples` | Sample browser | G3.2 |
| `/labs/:labId/samples/new`, `/labs/:labId/samples/:sampleId` | Sample create / detail / edit | G3.3 |
| `/labs/:labId/layout`, `/labs/:labId/boxes/:boxId` | Layout tree, box view | G3.1, G3.4 |
| `/labs/:labId/scan` | Bulk check-in/out | G3.6 |
| `/labs/:labId/import` | CSV import | G3.7 |
| `/labs/:labId/admin/layout` | Freezer / container / box setup | G3.8 |
| `/labs/:labId/admin/item-types` | Item types, custom fields | G3.9 |
| `/labs/:labId/admin/members` | Members, roles | G3.10 |
| `/account` | Sessions, API tokens | G3.11 |
| `/labs/:labId/audit` | Audit viewer | G3.12 |
| `/labs/:labId/shares` | Share requests | G3.13 |

### G0 — Server prerequisites (C++)

The SPA can be built against fakes without these, but it cannot talk to a
real `freezerd` safely until G0.1–G0.3 land.

- [ ] **G0.1. Browser session cookie + CSRF in the REST gateway.** Today the
      gateway reads only `Authorization: Bearer`. The SSE routes also accept
      `?access_token=`, because `EventSource` can't set headers, and a token
      in a URL ends up in proxy and access logs.
  - Add `POST /api/v1/auth/browser/{login,submit-mfa,logout}`. `login`
    forwards to `AuthService.Login` and sets two cookies:
    `fmgr_session=<token>; HttpOnly; Secure; SameSite=Strict; Path=/api`
    (no `Max-Age`, because server-side idle and absolute expiry are the real
    limits) and a random, JS-readable
    `fmgr_csrf=<32 bytes base64url>; Secure; SameSite=Strict; Path=/`. It
    returns `{session_id, user_id, mfa_required}` with **no token in the
    body**. `logout` revokes the session and expires both cookies.
  - On every route, unary and SSE, an `Authorization` header wins. Without
    one, the `fmgr_session` cookie is forwarded as
    `authorization: Bearer …` metadata.
  - A cookie-authenticated `POST` must carry `X-CSRF-Token` equal to the
    `fmgr_csrf` cookie, and any `Origin` header must match the request host
    (or `FMGR_WEB_ORIGIN` when set). Otherwise the gateway answers
    `403 {"code":"PERMISSION_DENIED"}` without calling gRPC.
    Bearer-authenticated calls skip the check because they carry no ambient
    credential.
  - Remove the `?access_token=` fallback from `SseBridge.h`.
  - `Secure` is dropped only when `FMGR_DEV_INSECURE_COOKIES=1`, and startup
    refuses that flag when `FMGR_ENV=production`. The flag exists because
    Safari won't store `Secure` cookies from `http://127.0.0.1`.
  - The existing bearer routes stay unchanged for scripts and I1.
  - **Files:** `src/rest/BrowserSession.{h,cc}` (pure cookie/CSRF helpers),
    `src/rest/RestGateway.cc`, `src/rest/SseBridge.h`, `src/server/main.cc`,
    `tests/unit/`, `tests/integration/rest_gateway_integration_test.cpp`.
  - **Locks:** none. **Depends on:** the SSE shutdown use-after-free fix
    (board starter item 2, the owner's uncommitted `SseBridge.h` change).
  - **Done when:** integration tests show that login sets both cookies and
    returns no token; cookie auth passes RBAC on a unary route and on
    `/api/v1/sample/watch`; a missing or wrong CSRF header gets 403; a
    foreign `Origin` gets 403; bearer calls are unaffected; logout revokes
    the session server-side and clears both cookies; `?access_token=` no
    longer authenticates.

- [ ] **G0.2. `AuthService.WhoAmI` RPC** (`POST /api/v1/auth/whoami`). The
      SPA can't read the cookie, so after a reload it needs the server to say
      who is signed in, which labs they belong to and what they may do. No
      such RPC exists; the Qt client infers it from `ListLabs`.
  - Returns `user_id`, `email`, `display_name`, `session_id`,
    `mfa_complete`, the session's `expires_at`, the caller's global
    permission keys, and one entry per lab membership: `{lab_id, lab_name,
    role_id, role_name, permission keys, scope_filters_json,
    is_phi_enabled}`. Permission keys are the `src/core/permissions.h`
    strings (`sample.read`, …).
  - It requires a session but no permission, like `ListLabs`. While MFA is
    pending it succeeds with `mfa_complete=false` and no memberships, so the
    SPA can resume at the TOTP step. Register it in the `AuthMiddleware`
    registry. It is read-only, so it writes no audit row.
  - **Files:** `proto/fmgr/v1/auth.proto`,
    `src/server/AuthServiceImpl.{h,cc}`, `src/rest/RestGateway.cc` (one
    route), `src/web/src/api/routes.ts` once G1.2 exists, integration tests.
  - **Locks:** `lock:proto`. **Depends on:** G0.1, because both edit
    `RestGateway.cc`.
  - **Done when:** tests cover a Member, a LabAdmin of two labs, a
    SystemAdmin, `UNAUTHENTICATED` without a session, and the MFA-pending
    case; `RpcRegistryCoversAllExpectedMethods` passes.

- [ ] **G0.3. Serve the SPA from `freezerd`, with browser security headers.**
  - When `FMGR_WEB_ROOT=<dir>` is set, the REST listener serves the built SPA.
    Files are served as they are. Any other `GET` outside `/api/`,
    `/healthz` and `/metrics` returns `index.html` for client-side routing.
    Unknown `/api/*` paths still get a JSON 404, and path traversal is
    refused. When the variable is unset, no static files are served, as
    today.
  - Headers:
    `Content-Security-Policy: default-src 'self'; frame-ancestors 'none'; base-uri 'none'; object-src 'none'`
    (no `unsafe-inline`), `X-Content-Type-Options: nosniff`,
    `Referrer-Policy: no-referrer`, and `Strict-Transport-Security` when TLS
    is on. Every `/api/*` response gets `Cache-Control: no-store`, hashed
    `assets/*` get `immutable`, and `index.html` gets `no-cache`. Use a Drogon
    post-handling advice so `RestGateway.cc` doesn't change.
  - Drogon rejects request bodies over 1 MiB by default, so a large CSV
    import through `/api/v1/sample/import` fails with 413. Raise the limit to
    match the gRPC inbound cap from C-10 (~10 MiB), configurable as
    `FMGR_REST_MAX_BODY_BYTES`.
  - **Files:** `src/rest/StaticAssets.{h,cc}`, `src/server/main.cc`,
    `tests/integration/rest_static_assets_integration_test.cpp` (uses a
    temp dir with a stub `index.html`).
  - **Locks:** none. **Depends on:** none. It edits `main.cc`, so don't run
    it in the same wave as G0.1.
  - **Done when:** tests cover asset serving, the deep-link fallback, JSON
    404 on `/api/*`, refused `..` and percent-encoded traversal, every header
    above, and an accepted 2 MiB import body.

- [ ] **G0.4. Name/barcode search on `ListSamples`.** Lookup by typing part
      of a name is the most common daily operation (PRD §9), but
      `ListSamples` filters only by exact barcode. The Qt client therefore
      scans the whole lab on the client (see the FIXME in
      `SampleLookupWidget.cc`), which a browser can't do at 100k rows.
  - Add `optional string query` to `ListSamplesRequest`: a case-insensitive
    substring match over `name` and `barcode`, combined with the other
    filters and paginated as usual. At least 2 characters.
  - Add a typed-DSL predicate (e.g. `contains_ci`) and implement it in both
    backends with `LIKE … ESCAPE` / `ILIKE`, escaping `%`, `_` and `\`. Raw
    SQL stays inside the `*Backend` classes. Cover it in
    `tests/backend_conformance/`.
  - Custom fields and PHI are **not** searched. L10 may later replace the
    implementation behind the same field.
  - **Files:** `proto/fmgr/v1/sample.proto`, `src/storage/` (DSL + both
    backends), `src/server/SampleServiceImpl.cc`, conformance and
    integration tests.
  - **Locks:** `lock:proto`. **Depends on:** none.
  - **Done when:** conformance tests pass on SQLite and Postgres, including
    wildcard escaping and non-ASCII names; an integration test goes through
    REST; the `asan` and `ubsan` presets pass, since this changes storage.

### G1 — SPA foundation

- [x] **G1.1. Scaffold `src/web/`, its toolchain and a CI job.**
  - Create a Vite + React + TypeScript (strict) app with every G-arch baseline
    dependency, the ESLint and Prettier configs, Vitest + React Testing
    Library + MSW, and react-i18next with `locales/en/common.json` and the
    `no-literal-string` rule on. **This delivers P3.** Start with one page
    and one passing test.
  - npm scripts: `gen` (a stub until G1.2), `dev`, `build`, `test`, `lint`,
    `typecheck`, `format:check`, and `check`, which runs all of them and is
    what CI runs.
  - Bundle budget: `build` fails if the initial JS is over 250 KiB gzipped.
  - Extend `tools/check-spdx-headers.sh` to `*.ts`, `*.tsx`, `*.js`, `*.mjs`
    and `*.cjs` (`// SPDX-…`) and to `*.css` (`/* SPDX-… */`). Add
    `src/web/.gitignore` for `node_modules/`, `dist/` and `src/gen/`.
  - Add a `web` job to `.github/workflows/build.yml` on `ubuntu-24.04`:
    `actions/setup-node` reading `.nvmrc`, npm cache, then
    `npm ci && npm run check`. Keep it independent of the C++ matrix so it
    finishes in minutes.
  - Export `FMGR_WEB_DEV_PORT` from `scripts/agent/env.sh` and configure the
    Vite proxy per G-arch 12. `src/web/CMakeLists.txt` stays a no-op, so the
    C++ build never needs Node.
  - Write `doc/dev/web.md` (setup, scripts, dev loop, the G-arch rules).
    Add the web commands to AGENTS.md §2 and §4, and note the npm lockfile
    in the `lock:deps` row.
  - **Locks:** `lock:ci`, `lock:deps`. **Depends on:** none.
  - **Done when:** `npm ci && npm run check` passes locally and in CI; the
    SPDX check covers web files; an agent new to the repo can start the dev
    loop from `doc/dev/web.md` alone.

- [x] **G1.2. API layer: codegen, client, SSE and test fakes.**
  - `npm run gen` runs `buf generate` over `../../proto` into `src/gen/`.
    `build`, `test` and `typecheck` run it first.
  - `src/api/routes.ts` maps each RPC to its REST path. `npm run check`
    also runs `scripts/check-routes.mjs`, which reads the `FMGR_ROUTE(...)`
    lines in `src/rest/RestGateway.cc` and fails if an RPC is missing on
    either side. From then on, a C++ PR that adds a route also adds its
    `routes.ts` line.
  - `src/api/client.ts` exposes a typed `call(rpc, request)`. It sends
    `X-CSRF-Token` (read from the `fmgr_csrf` cookie) and a fresh
    `X-Request-Id`. It turns `{code, message}` errors into a typed `ApiError`
    (gRPC code name, HTTP status, request id), turns a network failure into
    `ApiError('UNAVAILABLE')`, and notifies a session-expired listener on
    `UNAUTHENTICATED`.
  - `src/api/sse.ts` wraps `EventSource` with typed frames. It surfaces
    `event: error` frames as `ApiError`, reconnects with capped backoff (the
    browser resends `Last-Event-ID`), and closes on unmount.
  - `src/api/hooks/` holds TanStack Query hooks per service. `useSamples` is
    a `useInfiniteQuery` over `page_token`, mutations invalidate the matching
    keys, and every query key includes `lab_id`.
  - `src/test/fakeApi.ts` has MSW handlers for every route in `routes.ts`,
    backed by an in-memory demo lab, with per-RPC error injection, optional
    latency and an SSE fake. Add a `renderWithProviders()` helper.
  - Helpers: micros to `Date`, display labels for enums, and `ApiError` to an
    i18n message.
  - **Locks:** none. **Depends on:** G1.1.
  - **Done when:** there are unit tests for every error branch of the
    client, the CSRF and request-id headers, SSE reconnect, error frames and
    cleanup, the route checker failing on a planted mismatch, and per-RPC
    fault injection in the fakes.

- [x] **G1.3. App shell and UI kit.**
  - Providers (QueryClient, i18n, router, current-lab context), a top bar
    (lab picker, user menu, live-connection indicator), side nav, an error
    boundary, toasts, a 404 page and a "no access" page.
  - Placeholder routes and nav entries for **every** screen in the route
    map, each behind its future permission.
  - `src/ui/`: Button, IconButton, TextField, Select, Checkbox, Dialog,
    ConfirmDialog, Toast, Table (a TanStack wrapper with virtualization,
    sticky header and column visibility), Tabs, EmptyState, ErrorState,
    Spinner/Skeleton, a status Badge and Kbd. Design tokens go in
    `src/ui/tokens.css`, with light and dark themes via
    `prefers-color-scheme` and WCAG AA contrast.
  - Every control is keyboard-reachable with a visible focus ring, and `/`
    focuses the global lookup box. Layouts work down to 360 px wide (PRD
    §1.3).
  - **Locks:** none. **Depends on:** G1.1. It can run alongside G1.2, using
    a stubbed current user until G1.2's fakes land.
  - **Done when:** each `ui/` component has render, keyboard and axe tests
    (`vitest-axe`), and the shell renders every placeholder route.

### G2 — Auth flows

- [ ] **G2.1. Sign-in, MFA and session lifecycle** (`features/auth/`).
  - `/login` sends email and password to `auth/browser/login`. If
    `mfa_required`, it goes to `/login/mfa`, which takes a 6-digit code
    (`autocomplete="one-time-code"`, paste-friendly) and calls
    `auth/browser/submit-mfa`. A failed login always says "email or password
    incorrect", so accounts can't be enumerated. `RESOURCE_EXHAUSTED`
    (lockout or rate limit) is shown with advice on when to retry.
  - At app load the SPA calls `auth/whoami`. A 200 shows the shell, a 401
    goes to `/login?next=…` (`next` accepts same-origin paths only), and a
    pending MFA goes to `/login/mfa`.
  - Any later `UNAUTHENTICATED` clears the query cache, closes SSE streams
    and goes to `/login?next=…` with a "session expired" notice. Logout in
    the user menu calls `auth/browser/logout`, clears the cache and returns
    to `/login`.
  - `useCan(permission, labId?)` reads the WhoAmI permissions. The lab
    picker lists memberships, and the selected lab id is remembered in
    `localStorage`.
  - **Locks:** none. **Depends on:** G1.2, G1.3. Build against the fakes
    using the G0.1 + G0.2 contract, and check against a real `freezerd` once
    both have merged.
  - **Done when:** tests cover each branch above, including a wrong password,
    a wrong TOTP code, lockout, a session that expires during a mutation, an
    open-redirect attempt through `next`, and the cache being cleared on
    logout.

- [ ] **G2.2. OIDC sign-in.** **Blocked** on `OidcAuthProvider` (E6; PRD
      §7.1 schedules OIDC for M4). The web part is a "Sign in with
      <provider>" button driven by server-advertised config, the redirect,
      and a callback route that finishes through a browser-session route.
      Specify the server part under E6 first.

- [ ] **G2.3. Password reset pages.** **Blocked** on E2.1 (reset tokens) and
      O1 (email). The request page always answers "if that address exists,
      we sent a link"; the reset page takes the token from the link and a
      new password entered twice.

### G3 — Core flows (parity with the Qt client, F6 + PRD §9)

Each task owns `src/web/src/features/<name>/` and its locale namespace, and
none needs a lock. Every list screen uses cursor paging via `page_token`,
has empty, loading and error states, and handles `PERMISSION_DENIED`
inline.

- [x] **G3.1. Lab layout tree** (`features/layout/`). `useLabLayout(labId)`
      loads the lab's freezers, storage containers, box types and boxes once
      (every page) and derives the tree plus a `locationPath(boxId,
      position)` helper (freezer → … → box → position). The helper keeps the
      cycle and orphan guards of `src/qt/LocationPathResolver.cc`. The tree
      is collapsible, shows counts, and selecting a box opens the box view.
      G3.2–G3.5 and G3.8 reuse the hook. **Depends on:** G1.2, G1.3.
      **Done when:** the tree and path helper are tested with an orphaned
      container, a cycle, archived nodes (hidden) and an RPC failure partway
      through loading.

- [x] **G3.2. Sample browser** (`features/samples/`; F6.2, F6.6, F7). A
      virtualized TanStack table with infinite cursor paging that stays
      smooth at 100k rows. Filters: status, box, item type, barcode, and
      free text via G0.4, all kept in the URL. A column chooser includes
      custom-field columns from the lab's field definitions. CSV export calls
      `sample/export` and downloads `samples-<lab>-<date>.csv`. Live updates
      from `sample/watch` merge into the list cache, and tombstoned rows
      drop out. **Watch frames never carry PHI:** merge them into list
      caches only, and invalidate `sample/get` entries rather than overwrite
      them. **Depends on:** G3.1 and G0.4.
      **Done when:** tests cover paging, each filter, export, live insert,
      update and tombstone, SSE reconnect and the error paths, and a
      100k-row fake never renders more than about 100 rows at once.

- [x] **G3.3. Sample detail, create and edit** (`features/sample-detail/`).
  - The detail view shows every field. Custom fields render according to
    their definition type (string, int, float, bool, date, datetime, enum,
    reference). PHI fields appear only when the response includes them (the
    server filters by `phi.read`) and are marked as PHI. It also shows the
    parent link ("parent: X (depleted)"), the location path, and history
    from `audit/list` filtered to this sample when the user has
    `audit.read`.
  - The create/edit form is generated from the item type's inherited field
    definitions. Its client-side validation mirrors
    `src/core/custom_field_validator.h`, but the server decides: a server
    `INVALID_ARGUMENT` message is shown on the field.
  - Actions: check out / in / discard (volume used, reason), move (box plus a
    picker of free positions), and soft delete with confirmation.
  - **Depends on:** G3.1.
  - **Done when:** tests cover form generation for each data type,
    inherited fields, display of server rejections, and every action,
    including `ALREADY_EXISTS` (position taken) and a size-mismatch
    rejection.

- [ ] **G3.4. Box view** (`features/box/`; F6.3). The grid is drawn from the
      box type's positions (row and column, including mixed formats such as
      the Eppendorf 3×3 + 2×2 box). Occupied cells show name and status
      colour, and clicking one opens its detail. Samples move by drag and
      drop, or by keyboard (select a sample, then a target cell). A server
      rejection appears as a "size mismatch" or "position taken" toast. The
      grid refreshes live via `sample/watch?box_id=`. A printable box map and
      label sheet use print CSS (`@media print`, then the browser's "Save as
      PDF") instead of the Qt PDF export. **Depends on:** G3.1.
      **Done when:** layouts are tested for the 9×9, 10×10, 96-well and mixed
      templates (D4.2 seeds), as are a successful and a rejected move, a live
      update, and the print stylesheet.

- [ ] **G3.5. Single-handed lookup** (`features/lookup/`; PRD §9, the most
      common daily flow). One large autofocused field ("scan or type a
      barcode or name"). Enter tries an exact barcode match first, then the
      G0.4 `query` search. One hit shows a large location-path card
      (freezer → … → position) with the status and a check-out button.
      Several hits show a keyboard-navigable pick list, and no hit shows a
      clear message. After each lookup the field regains focus with its text
      selected, so the next scan overwrites it. USB and Bluetooth HID barcode
      scanners type into the focused field and press Enter, which is the
      same `HidKeyboardScanner` pass-through the Qt client uses, so no
      special browser API is needed. **Depends on:** G3.1, G0.4.
      **Done when:** tests cover a barcode hit, a name hit, several hits, no
      hit, an unplaced sample, the error paths, and a fast burst of keystrokes
      like a scanner produces.

- [ ] **G3.6. Bulk check-in/out scan mode** (`features/scan/`; F6.4). The
      user picks an action (out, in or discard) and an optional reason and
      volume, then scans repeatedly. Each scan calls `sample/list?barcode`
      then `sample/checkout` and adds a line to a session log with its
      result (done, not found, wrong state, or denied). Auto-submit after an
      inactivity gap, for scanners that send no Enter, is optional and off by
      default. There is no undo, because the audit trail records every
      action; the screen explains how to reverse one instead.
      **Depends on:** G1.2, G1.3.
      **Done when:** tests cover each per-row outcome, a duplicate scan, and
      the field keeping focus.

- [ ] **G3.7. CSV import wizard** (`features/import/`; F6.5). The user picks
      or drops a file, which is size-checked against the G0.3 limit. The
      wizard sends it to `sample/import` with `dry_run=true`, shows a per-row
      report (filterable to failures, with a `header_error` banner), and on
      confirmation runs the real import and summarizes it with links to the
      new samples. A CSV template matching the export columns can be
      downloaded. **Depends on:** G1.2, G1.3, G0.3.
      **Done when:** tests cover a header error, mixed passing and failing
      rows, a successful commit, a commit that fails after a clean dry run
      (the data changed in between), and an oversized file.

- [ ] **G3.8. Lab layout admin** (`features/admin-layout/`). Create, edit and
      archive freezers, storage containers (reorder and re-parent by drag and
      drop), box types (a position editor with live grid preview, and import
      of the D4.2 templates), container types (size classes) and boxes.
      Requires `freezer.configure` / `box.configure`. **Depends on:** G3.1.
      **Done when:** each create, edit and archive path, the position-editor
      validation (D4.1) and a size-class reference to a missing container
      type are tested.

- [ ] **G3.9. Item types and custom fields admin**
      (`features/admin-item-types/`; N5 documents the rules). An item-type
      tree editor with cycle-safe re-parenting, and a field-definition
      editor per node that shows inherited fields read-only. Each definition
      has a data type, required flag, validation (enum values, ranges),
      `indexed` and `is_phi`. `is_phi` is offered only when the lab has PHI
      mode on, and `is_phi` together with `indexed` is refused (see L10).
      Requires `item_type.define` / `custom_field.define`.
      **Depends on:** G1.2, G1.3.
      **Done when:** tests cover inheritance display, the rule that a child
      may tighten but not drop a required parent field, the PHI + indexed
      refusal, and a cycle rejected by the server.

- [ ] **G3.10. Members and roles admin** (`features/admin-members/`). The
      member list, inviting by email with a role, and revoking. The role
      list, creating a custom role, granting and revoking permissions (a
      checkbox grid grouped by entity), and a scope-filter editor for
      `freezer_in`, `project_in` and `item_type_in` (the D2 schema).
      Requires `user.invite` or the role permissions.
      **Depends on:** G1.2, G1.3.
      **Done when:** tests cover invite, revoke, role create, grant and
      revoke, scope-filter validation, and the denied paths.

- [ ] **G3.11. Account page** (`features/account/`; the web part of I2 and
      D9.2). My sessions (device, IP and last seen, with the current session
      marked) with revoke. API tokens: create with a name, scope, lab and
      expiry, after which the plaintext is shown **once** with a copy button
      and a warning that it can't be shown again; list; revoke.
      **Depends on:** G1.2, G1.3.
      **Done when:** tests cover revoking another session and the current
      one (which ends at `/login`), and that the token plaintext is gone
      after the dialog closes.

- [ ] **G3.12. Audit viewer** (`features/audit/`; the web part of E7). A
      paginated list filtered by entity kind, entity id and time range (the
      filters `ListAuditEvents` supports today; actor and action filters wait
      for E7). Event detail shows a before/after JSON diff. It also offers a
      "verify chain" action with its result, CSV export when the user has
      `audit.export`, and a live-feed toggle using `audit/watch`. Requires
      `audit.read`. **Depends on:** G1.2, G1.3.
      **Done when:** tests cover each filter, the diff for insert, update and
      delete events, both verify results, and live-feed reconnect.

- [ ] **G3.13. Cross-lab share requests** (`features/shares/`; the web part
      of I3). Incoming and outgoing lists; creating a request (target lab and
      scope); approving, rejecting and revoking, with the approval chain
      showing which signatures are present and which are pending.
      **Depends on:** G1.2, G1.3.
      **Done when:** tests cover each state transition and a user who is
      allowed to see a request but not to approve it.

### G4 — Dashboards

- [ ] **G4.1. `ReportService.GetLabSummary` RPC**
      (`POST /api/v1/report/lab-summary`). This is the server side of the
      home dashboard, and the Qt dashboard in PRD §9 will use it too. For one
      lab it returns:
  - sample counts by status;
  - open check-outs: the count, plus the 20 oldest with sample, user and
    start time;
  - the 20 boxes with the least free space (label, capacity, occupied);
  - fill per freezer (occupied / capacity);
  - a sample-age histogram with fixed buckets (< 1 month, 1–6 months,
    6–12 months, 1–2 years, 2–5 years, > 5 years);
  - daily check-out and check-in counts for the last 30 days.

  Aggregation happens in the storage layer, through new backend methods in
  both backends covered by conformance tests. Raw rows are never sent to the
  client to aggregate there. It requires `sample.read` in the lab, counts
  only what the caller's scope filters allow, and returns no PHI. Register
  it in the RPC registry.
  - **Files:** `proto/fmgr/v1/report.proto` (new),
    `src/server/ReportServiceImpl.{h,cc}`, `src/server/FreezerServer.cc`,
    `src/rest/GatewayStubs.h`, `src/rest/RestGateway.cc`, `src/storage/`,
    `src/web/src/api/routes.ts`, tests.
  - **Locks:** `lock:proto`. **Depends on:** G0.2 and G0.4 (the proto lock
    and `RestGateway.cc`).
  - **Done when:** conformance tests pass on both backends, including an
    empty lab and a scope-restricted member; positive and negative authz
    tests pass; a 100k-sample SQLite fixture answers in under 500 ms.

- [ ] **G4.2. Home dashboard** (`features/dashboard/`, route `/`). Shows the
      status counts, open check-outs (oldest first, each linking to its
      sample), boxes low on space, a per-freezer fill heatmap, the
      sample-age histogram and 30-day check-out activity, all from G4.1.
      Charts are inline SVG components with text alternatives that work in
      both themes; a chart library needs the lead's approval.
      **Depends on:** G4.1, G1.3.
      **Done when:** tests cover an empty lab, a full lab, the error state,
      and axe checks on the charts.

### G5 — End-to-end tests, shipping and docs

- [ ] **G5.1. Playwright end-to-end and accessibility gate**
      (`src/web/e2e/`; delivers P4). A CI job runs after the C++ build. It
      starts `freezerd` with a temp SQLite DB and
      `FMGR_WEB_ROOT=src/web/dist`, seeds it with `scripts/seed_demo.py`,
      and runs these flows: login with TOTP, lookup by barcode and by name,
      check out and in, a box move including a rejected one, import dry run
      and commit, export, and logout. `@axe-core/playwright` must report no
      serious or critical violations on any page visited. The job also checks
      that API responses are `no-store` and that `localStorage` holds only UI
      preferences afterwards.
      **Locks:** `lock:ci`. **Depends on:** G0.1–G0.3, G2.1, G3.2–G3.7.

- [ ] **G5.2. Ship the SPA with `freezerd`.** A CMake `install()` rule copies
      `src/web/dist` to `share/freezerd/web` when it exists. An installed
      `freezerd` uses that path when `FMGR_WEB_ROOT` is unset, and the
      release workflow builds the SPA before packaging (feeding K1–K3).
      **Locks:** `lock:ci`. **Depends on:** G0.3, G1.1.

- [ ] **G5.3. User and operator docs.** Add web sections to N1 (the
      quickstart, with Qt and web screenshots of demo data side by side).
      Add an nginx or Caddy reverse-proxy example with TLS, explain the
      same-origin requirement, and document `FMGR_WEB_ROOT`,
      `FMGR_WEB_ORIGIN` and `FMGR_REST_MAX_BODY_BYTES` for operators.
      **Depends on:** G5.1.

### Not planned yet (needs server work first)

These web features are in PRD §9/§10 but have no server support. Each needs
its own server item before a G task can be written:

- TOTP enrolment (no enrol RPC exists; the web can only verify codes);
- "Log me out everywhere" (D9.2, revoke-all);
- batch aliquot creation and quick-add/draft samples (PRD §9; needs F9 bulk
  RPCs and a draft state);
- a live bulk-import progress stream (F3/F7);
- the web first-run wizard (K5, D1.3);
- a children list on the sample detail page (`ListSamples` has no
  `parent_sample_id` filter);
- the SQLite amber banner (PRD §20; no server signal exists).

### Suggested waves (at most 3 workers; lock and file conflicts checked)

| Wave | Tasks | Notes |
|---|---|---|
| 1 | G0.1, G0.4, G1.1 | Separate areas: `src/rest/`; storage + proto; `src/web/` + CI. If the SSE fix hasn't merged, run G0.3 in place of G0.1. |
| 2 | G0.2, G1.2, G1.3 | G0.2 needs G0.1 merged (`RestGateway.cc`). G1.2 (`api/`, `test/`) and G1.3 (`app/`, `ui/`) don't overlap. |
| 3 | G0.3, G2.1, G3.1 | After this wave a user can sign in to a real server. |
| 4 | G3.2, G3.4, G3.5 | All build on G3.1's `useLabLayout`. |
| 5 | G3.3, G3.6, G3.7 | **After this wave the daily flows are done (M4 "core flows").** |
| 6 | G4.1, G3.8, G3.9 | G4.1 takes `lock:proto` again. |
| 7 | G4.2, G3.10, G3.11 | |
| 8 | G3.12, G3.13, G5.1 | G5.1 takes `lock:ci`. |
| 9 | G5.2, G5.3 | |

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

- [x] **P3. Web i18n scaffolding.** (Delivered by G1.1; keys live in
      `locales/en/<namespace>.json` per G-arch 11, not the single
      `locales/en.json` this line originally named.) `react-i18next` integrated;
      `src/web/locales/en.json` committed; ESLint rule
      `i18next/no-literal-string` enabled. Translation keys follow
      `feature.context.string-id` convention.

- [ ] **P4. WCAG 2.1 AA targeting for Web UI.** (Delivered by G1.3 + G5.1.) Run `axe-core` as a
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
