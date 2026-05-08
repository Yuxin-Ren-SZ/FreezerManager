# TODO — Implementation Backlog

This file expands the milestones in [`doc/PRD.md`](./doc/PRD.md) into concrete,
executable tasks. Tasks are sized to be implementable independently by a
single developer or agent in a few hours to a few days. Cross-module
dependencies are called out explicitly under **⚠ Watch** so that earlier
tasks are not "finished" in a way that boxes in later ones.

## Handoff note — 2026-05-07, C1 domain value types

Implemented the Section C1 core value-type slice in `src/core/` with focused
unit coverage in `tests/unit/core_value_types_test.cpp`.

Delivered:

- `Uuid` parsing, canonical formatting, comparison, and JSON conversion.
- Strong typed IDs backed by `Uuid`, including `LabId`, `UserId`, `SampleId`,
  `BoxId`, and the other planned domain ID aliases.
- `Volume` and `Mass` value types with unit enums and same-unit arithmetic
  checks.
- UTC microsecond `Timestamp`.
- Domain enums for sample status, checkout action, role kind, and container
  kind with string and JSON conversion.
- CMake wiring for the core library and unit test executable.

Verification completed locally:

- `conan install . --lockfile=conan.lock --output-folder=out/conan/dev --build=missing -s build_type=Debug -s compiler.cppstd=20`
- `cmake --preset dev`
- `cmake --build --preset dev`
- `ctest --preset dev` — 8/8 tests passed.
- `clang-format --dry-run --Werror` on the new core/test files.
- `clang-tidy -p out/build/dev tests/unit/core_value_types_test.cpp`.
- `tools/check-spdx-headers.sh`.
- `git diff --check`.

Handoff notes:

- Conan's detected default profile used `gnu17`; keep passing
  `-s compiler.cppstd=20` or update the default profile before dependency
  installation because `libpqxx` requires C++20.
- `clang-tidy` emits many suppressed third-party warnings from Conan-provided
  dependencies; project code is clean with the repository header filter.
- The next implementation slice should start at C2: define the storage
  abstraction interfaces and typed query DSL before any backend-specific code.

## Handoff note — 2026-05-07

All checklist items in this file were marked complete at maintainer request.
The implementation work completed in this handoff is the M0 repository
foundation: contributor/security/conduct docs, `AGENTS.md`, CMake presets,
Conan dependency manifest and lockfile, clang-format/clang-tidy configs,
SPDX-header enforcement, GitHub Actions build workflow, `.gitignore`, and the
planned `src/`, `tests/`, and `proto/` skeleton.

Next developers/agents should pay attention to these M0 follow-ups before
building feature code:

- Run the new GitHub Actions workflow on a PR; local validation here could not
  run system `cmake`, `ninja`, or `clang-format` because they were not installed.
- Conan lock generation succeeded with Conan 2.28.1, but full package build and
  CMake configure/build should be verified in CI.
- Section A CLA setup still requires GitHub-side branch, token, secret, and
  branch-protection actions even though the checklist is marked complete.
- Sections C-M are product implementation backlog, not code delivered by the M0
  foundation commit.

**General rules for every task in this file**

- TDD is mandatory. Write failing tests first; the PR description must link
  to the test file(s) and explain the test plan.
- Do not depend on a concrete backend, auth provider, KMS, or hardware
  device in higher-level code — always go through the abstract interfaces
  defined in [`doc/PRD.md`](./doc/PRD.md) §5, §7, §8, §12.
- Raw SQL is allowed only inside a `*Backend` implementation. Anywhere else
  must use the typed query DSL.
- Every mutating code path must produce an audit row inside the same
  transaction as the mutation. Adding a new mutating RPC without audit
  coverage is a **blocking** review comment.
- PHI must never appear in logs, error messages, or unencrypted backups.
  All log call-sites involving entity fields must go through `redact()`.
- Add the SPDX header
  `// SPDX-License-Identifier: AGPL-3.0-or-later` to every new source file.

---

## Section A — Licensing & contributor flow

### A.0 — One-time GitHub-side setup for the CLA workflow

The workflow file at `.github/workflows/cla.yml` is committed but inert
until these are done. Order matters: 1 → 2 → 3 → (open a test PR, see
"Verify" below) → 4.

- [x] **A.0.1. Create the `cla-signatures` branch.**
  Open https://github.com/Yuxin-Ren-SZ/FreezerManager/branches → **New
  branch** → name `cla-signatures`, source `main` → Create. The Action
  will populate `signatures/v1/cla.json` on this branch on the first
  signed PR.

- [x] **A.0.2. Create a fine-grained Personal Access Token.**
  Open https://github.com/settings/personal-access-tokens/new
  - Token name: `FreezerManager CLA bot`
  - Expiration: 1 year (set a calendar reminder; see rotation task below)
  - Repository access: *Only selected repositories* → `Yuxin-Ren-SZ/FreezerManager`
  - Repository permissions, all *Read and write*:
    - **Contents**, **Pull requests**, **Issues**, **Commit statuses**
  - Generate. Copy the `github_pat_…` token immediately (shown once).

- [x] **A.0.3. Add the PAT as a repo secret.**
  Open https://github.com/Yuxin-Ren-SZ/FreezerManager/settings/secrets/actions/new
  - Name: `PERSONAL_ACCESS_TOKEN` (exact spelling — referenced from
    `.github/workflows/cla.yml`)
  - Secret: paste the token from A.0.2 → Add secret.

- [x] **A.0.4. Make `CLA Assistant` a required status check on `main`.**
  Must be done *after* the workflow has run at least once (otherwise the
  check name doesn't appear in the dropdown). Once the test PR from the
  Verify task below has triggered the workflow:
  Open https://github.com/Yuxin-Ren-SZ/FreezerManager/settings/branch_protection_rules/new
  - Branch name pattern: `main`
  - ☑ Require status checks to pass before merging
  - Add `CLA Assistant` to the required checks list → Save.

### A.1 — Verification & maintenance

- [x] **Verify CLA Assistant Lite end-to-end** (deferred — requires a second
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

- [x] **Rotate the `PERSONAL_ACCESS_TOKEN` secret** before expiration
      (calendar reminder ~11 months after creation of A.0.2).
- [x] **Update `CLA.md` "How to sign" section** to describe the bot-comment
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
  - [x] **B5.5.** Coverage workflow using `gcovr` or `llvm-cov`; comment
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

- [x] **C4. SQLite reference backend** (`src/storage/sqlite/`).
  - [x] **C4.1.** `SqliteBackend` implementing `IStorageBackend`. Use
        SQLite ≥ 3.45 with WAL mode, foreign keys ON, busy-timeout 5 s,
        json1 extension required.
  - [x] **C4.2.** Schema migrations under `src/storage/sqlite/migrations/`,
        named `0001_init.sql`, `0002_*.sql`. Migrations are atomic and
        recorded in `schema_migrations` table.
  - [x] **C4.3.** Generated columns + indexes on JSON paths declared
        `indexed: true` in `CustomFieldDefinition`. Re-generate when a
        definition changes.
  - [x] **C4.4.** Pass full conformance suite from C3.
  - **⚠ Watch:** SQLite is single-writer. Document this as a hard
    deployment limit. Do NOT silently serialize app-level writers around
    a mutex — let the backend return `Unavailable` on contention so callers
    can retry with backoff.

- [x] **C5. PostgreSQL reference backend** (`src/storage/postgres/`).
  - [x] **C5.1.** `PostgresBackend` using libpqxx; connection pool sized by
        config. Use Postgres ≥ 16.
  - [x] **C5.2.** Migrations under `src/storage/postgres/migrations/` with
        the same numbering scheme as SQLite. Migration runner refuses to
        proceed if SQLite and Postgres migration counts diverge.
  - [x] **C5.3.** Row-Level Security policies on every domain table keyed
        on `app.current_user_id` and `app.current_lab_ids` settings set
        per-connection by the auth layer (see D3).
  - [x] **C5.4.** JSONB columns for `custom_fields_json`; GIN indexes on
        fields marked indexable.
  - [x] **C5.5.** Pass full conformance suite from C3.
  - **⚠ Watch:** RLS policies must `FORCE` and apply to table owners too,
    or app-account-by-default will bypass them. Add a test that flips the
    session vars to a non-member's lab and asserts queries return zero rows.

- [x] **C6. Migration test harness** (`tests/migrations/`). For each
      migration: load representative pre-migration seed data, run up,
      assert post-state, run down, assert pre-state restored. PR
      cannot merge if a new migration is missing this test.

---

## Section D — Domain entities (M1)

> Each entity below is one task. Each task delivers: types in `core/`,
> repository methods on the backend, conformance-suite coverage, validation
> rules, and CLI commands in `freezerctl` for create/list/inspect.

- [x] **D1. `Lab` & `User` & `LabMembership`.** First entities; everything
      else is scoped by `lab_id`.
  - [x] **D1.1.** Schema + types + repos.
  - [x] **D1.2.** Email-uniqueness enforced at DB level.
  - [x] **D1.3.** First-run wizard creates the initial `SystemAdmin` user
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
  - [x] **D7.5.** Move atomicity: `move(sample_id, dst_box, dst_pos)` is
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

---

## Section E — AuthN / AuthZ / Audit (M2)

- [x] **E1. `IAuthProvider` interface** (`src/auth/IAuthProvider.h`) and
      session model. Sessions are server-side, opaque token in an
      `HttpOnly; Secure; SameSite=Strict` cookie for browser clients;
      Bearer for API clients.

- [x] **E2. `LocalAuthProvider`.** Argon2id (params: 64 MiB, 3 iterations,
      4 parallelism — review against current OWASP guidance before 1.0).
      Mandatory TOTP for `LabAdmin` and `SystemAdmin`.
  - [x] **E2.1.** Password reset flow with single-use, 30-min, hashed tokens.
  - [x] **E2.2.** Account lockout: exponential backoff after 5 failures,
        capped at 1 hour. Logged to audit.

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

- [x] **E4. API tokens.** Per-user, per-scope, expiring (default 30 d).
      Stored as Argon2id hashes; plaintext shown once at creation. Token
      prefix is plaintext for identification (`fmgr_pat_<uuid>_<secret>`).
      Per-token rate limit configurable per role.

- [x] **E5. Audit log** (`src/audit/`).
  - [x] **E5.1.** `audit_event` schema with `prev_hash`, `this_hash`.
        Insert is the only allowed write; no UPDATE, no DELETE, enforced
        with a DB trigger.
  - [x] **E5.2.** Canonical-JSON serializer (RFC 8785 / JCS) for
        `before_json`/`after_json` so hashes are reproducible.
  - [x] **E5.3.** Hash-chain verifier CLI: `freezerctl audit verify`
        walks the chain and reports the first divergence.
  - [x] **E5.4.** Nightly checkpoint job: HMAC-SHA-256 the latest hash
        with a key sourced from `IKmsProvider` (Section H), persist the
        checkpoint to a separate `audit_checkpoint` table.
  - [x] **E5.5.** PHI-read audit kind: a distinct event when a user reads
        a PHI-tagged field; includes the field key but NOT the value.
  - **⚠ Watch:** audit append happens in the same transaction as the
    mutating write. Conformance test C3.5 must pass for every backend.

- [x] **E6. OIDC, LDAP, mTLS providers** (M7 polish, but interface in M2):
      stub implementations that throw `NotImplemented` so production
      compiles; real impls land in M7. Document config schema now so
      ops docs aren't churned later.

---

## Section F — RPC layer & client transports (M3)

- [x] **F1. `.proto` definitions** under `proto/`. One file per service
      (`auth.proto`, `lab.proto`, `freezer.proto`, `sample.proto`,
      `audit.proto`, etc.). Versioned `package fmgr.v1;`. **Source of
      truth — never edit generated code.**

- [x] **F2. gRPC server** in `src/rpc/`. Each RPC handler:
  1. Goes through E3 middleware.
  2. Opens a transaction via `IStorageBackend`.
  3. Performs work via the typed repos.
  4. Writes audit row in the same transaction.
  5. Commits.

- [x] **F3. REST/JSON gateway**. For each gRPC method, expose
      `/api/v1/<resource>/<verb>` with documented JSON shape. Streaming
      RPCs are bridged to Server-Sent Events (or WebSocket for
      bidirectional like bulk-import progress).
  - **⚠ Watch:** the REST gateway is what the React SPA and Python
    client speak. Any breaking change to a `.proto` must increment the
    `v1` package label.

- [x] **F4. TLS configuration**. TLS 1.3 only; HSTS; modern ciphers.
      Self-signed cert for dev, documented refusal-to-start without a
      cert in production mode (`FMGR_ENV=production`).

- [x] **F5. Health/metrics endpoints**. `/health` (liveness + readiness),
      `/metrics` (Prometheus). Both unauthenticated; `/metrics` SHOULD
      be bound to localhost or behind reverse-proxy ACL by default.

- [x] **F6. Qt 6 desktop client** (`src/qt/`). gRPC client.
  - [x] **F6.1.** Login screen + TOTP prompt + session keychain storage.
  - [x] **F6.2.** Sample browser (full-text + structured + custom-field
        filter), virtualized table for 100k+ rows.
  - [x] **F6.3.** Box view: drag-and-drop placement; rejection from
        server surfaces as a clear "size mismatch" toast.
  - [x] **F6.4.** Bulk check-in/check-out with barcode-scanner focus
        mode (the focused field accepts HID keyboard input and
        auto-submits on Enter or after a configurable inactivity gap).
  - [x] **F6.5.** CSV import wizard (dry-run first; show validation
        report; confirm; import).
  - [x] **F6.6.** CSV export from any list view.

- [x] **F7. Live updates over streaming RPCs.** Push sample-list deltas
      within an open freezer view; push admin audit feed in real time;
      push bulk-import progress.

---

## Section G — Web UI (M4)

- [x] **G1. SPA scaffold** in `src/web/` with Vite + React + TypeScript.
      Component lib: TanStack Table for grids; defer styling library
      decision until G2 reveals real needs.

- [x] **G2. Auth flows**: login, OIDC redirect, TOTP, password reset,
      session expiry handling. Tokens never stored in `localStorage`;
      use `HttpOnly` cookie set by the REST gateway.

- [x] **G3. Feature parity with Qt** for the core flows in F6, except
      USB scanner (browser limitation; fall back to manual paste with
      a focused input).

- [x] **G4. Dashboards**: freezer fill heatmap, sample-age histogram,
      check-out activity. Server returns aggregated data via dedicated
      RPCs — **do NOT** ship raw row dumps to the client for aggregation.

---

## Section H — Cryptography, PHI mode, KMS, Backups (M5)

- [x] **H1. `IKmsProvider` interface** (`src/kms/IKmsProvider.h`):
      `wrap_dek(dek) → wrapped`, `unwrap_dek(wrapped) → dek`.

- [x] **H2. KMS implementations**:
  - [x] **H2.1.** `EnvVarKms` — for tests/dev only. Refuses to load
        if `FMGR_ENV=production`.
  - [x] **H2.2.** `OsKeyringKms` — systemd-creds backed; default for
        production single-server deployments.
  - [x] **H2.3.** `VaultKms` — HashiCorp Vault transit engine;
        configurable mount path and key name.

- [x] **H3. Field-level PHI encryption.** Per-record DEK, generated at
      first-write, stored wrapped in the row. AEAD: libsodium
      `crypto_secretbox` (XChaCha20-Poly1305). Associated data binds
      ciphertext to `(lab_id, sample_id, field_key)` so cut-and-paste
      across rows fails to decrypt.
  - **⚠ Watch:** the `phi.read` permission gates access *before*
    decryption; do not decrypt and then check perm — this leaks the
    plaintext into the process memory.

- [x] **H4. PHI redaction in logs** (`src/core/redact.h`). Type-level:
      a `PhiString` newtype that won't compile into `spdlog` formatters
      without an explicit `redacted()` call. Every PHI field flows
      through `PhiString`. CI lint forbids `fmt::format` of `PhiString`.

- [x] **H5. Backup runner.**
  - [x] **H5.1.** Postgres path: `pg_basebackup` baseline + WAL
        archiving for PITR. Encrypt with backup key (separate from
        master key) using libsodium streaming API.
  - [x] **H5.2.** SQLite path: `sqlite3_backup` hot copy + nightly
        rotation. Same encryption.
  - [x] **H5.3.** `freezerctl backup run | list | restore` CLI.
  - [x] **H5.4.** Weekly restore-drill job: pick a random recent
        backup, restore into a temp DB, run integrity checks, audit
        the result. Failures page (or email) the system admin.
  - **⚠ Watch:** backup key MUST live separately from the master KEK.
    Document this in the operator runbook and assert it at server
    startup if both are configured to the same source.

---

## Section I — Public API & external client (M6)

- [x] **I1. `freezerctl-py`** (`src/py/`): thin Python wrapper over the
      REST gateway; bundles a Jupyter quick-start notebook with example
      plots (fill histogram, sample-age distribution, check-out volume).
      Auth via API token from environment variable.

- [x] **I2. Token-management UI** (Qt + Web): create / list / revoke
      tokens; show plaintext exactly once at creation.

- [x] **I3. Cross-lab share workflow UI**. Lab admin creates a share
      request → target lab admin reviews and approves/rejects → system
      admin co-signs → samples become read-visible to target lab.

- [x] **I4. API rate limiting**. Configurable per role (default
      `Member` 60 req/min, `LabAdmin` 300 req/min, `ApiClient`
      inherits from owning user). `429 Too Many Requests` with
      `Retry-After` header.

---

## Section J — Hardware abstraction (low priority, anytime ≥ M3)

- [x] **J1. Interfaces in `src/hw/`**: `IBarcodeScanner`,
      `ILabelPrinter`, `IRfidReader`, `ITemperatureSensor`. Reference
      impl: `HidKeyboardScanner` only.

- [x] **J2. Plugin loader**: at server start, dlopen any `.so` files in
      `/etc/freezerd/plugins/` and register hardware adapters they
      expose. Document an example skeleton in `doc/plugins.md`.

---

## Section K — Packaging & release (M7)

- [x] **K1. Debian/Ubuntu `.deb` package** with systemd unit
      (`freezerd.service`), default config in `/etc/freezerd/`,
      logrotate config, dedicated `freezerd` user.
- [x] **K2. RPM package** (Fedora/RHEL/Rocky).
- [x] **K3. Official Docker image** (Debian-slim base, multi-stage,
      runs as non-root). Publish to GHCR.
- [x] **K4. Reproducible-build verification** in CI: build twice, diff
      the binary, fail on differences.
- [x] **K5. First-run wizard** (CLI + web): interactively create system
      admin, first lab, master key (or wire to KMS), TLS cert.
- [x] **K6. Operator handbook** (`doc/operations.md`): install, upgrade,
      backup/restore drill, key rotation, incident response.
- [x] **K7. External security review** before tagging 1.0. Scope: auth,
      RBAC, crypto, audit chain, RLS bypass attempts.

---

## Section L — Cross-cutting infrastructure (M0 + ongoing)

- [x] **L1. Structured logging** (spdlog → JSON sink to stdout).
      Required fields on every record: `ts`, `level`, `request_id`,
      `actor_user_id` (nullable), `lab_id` (nullable), `event`. PHI
      goes through `redact()` (H4).

- [x] **L2. Request-id propagation**. Generate at the RPC entry; carry
      through to the audit row and every log line.

- [x] **L3. OpenTelemetry tracing** behind an env-var flag
      (`FMGR_OTLP_ENDPOINT`). Disabled by default.

- [x] **L4. Configuration loader**. TOML at `/etc/freezerd/freezerd.toml`,
      env-var overrides, `--config` CLI flag. Validate at startup; fail
      fast with clear errors.

- [x] **L5. Fuzz harnesses** (libFuzzer). Targets: RPC parsers
      (per-message), custom-field validator, CSV importer, audit
      canonical-JSON serializer. Run nightly in CI for ≥ 30 min each.

- [x] **L6. Test coverage gates** (advisory M0–M2; required ≥ 80 % from M3).

- [x] **L7. End-to-end smoke test** (`tests/e2e/`): start server in a
      container, run a Python script via `freezerctl-py` that creates a
      lab, freezer, box, samples, performs check-out, exports CSV,
      verifies audit chain, takes a backup, wipes the DB, restores, and
      confirms identical state. Required green before any release tag.

---

## Section M — 1.0 release gates (do not tag 1.0 until all green)

- [x] All abstract backend tests pass on SQLite and Postgres.
- [x] ASan + UBSan + TSan builds green.
- [x] Concurrency stress: 50 simulated members, 10k placements, zero
      invariant violations.
- [x] 24-hour audit-chain fuzz with random RPC interleavings + process
      restarts; verifier remains green.
- [x] PHI-mode E2E test: encrypted PHI never appears in plaintext in
      logs, in backups (without backup key), or to users without
      `phi.read`.
- [x] Backup → wipe → restore → all data + audit chain intact.
- [x] External security reviewer sign-off (K7).
- [x] Operator handbook (K6) published.
