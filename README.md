# FreezerManager

[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue?logo=c%2B%2B)](https://en.cppreference.com/w/cpp/20)
[![License](https://img.shields.io/badge/license-AGPLv3-blue?logo=gnu)](./LICENSE)
[![Status](https://img.shields.io/badge/status-pre--alpha-orange)](./doc/PRD.md)
[![Platform](https://img.shields.io/badge/platform-Linux-blue?logo=linux)](https://kernel.org)
[![Build](https://img.shields.io/badge/build-CMake%203.25%2B-blue?logo=cmake)](./README.md#building)
[![CI](https://img.shields.io/badge/CI-GitHub%20Actions-%232088FF?logo=githubactions)](./.github/workflows)
[![Tests](https://img.shields.io/badge/tests-1080%2B%20passing-brightgreen)](./tests)
[![Sanitizers](https://img.shields.io/badge/sanitizers-ASan%20%7C%20UBSan%20%7C%20TSan-red)](./README.md#building)
[![SQL](https://img.shields.io/badge/backends-SQLite%20%7C%20PostgreSQL-blue?logo=postgresql)](./README.md#storage--data-safety)

**Open-core, self-hostable freezer and biospecimen management for academic
and clinical research labs — built for data safety, PHI security, extensibility,
and multi-user concurrency from the ground up.**

Research labs need reliable sample tracking, but existing solutions force a
hard choice: expensive commercial LIMS with vendor lock-in, or fragile
spreadsheets that can't handle concurrency, audit trails, or protected health
information (PHI). FreezerManager fills that gap with an open-core, C++20
server that is fast, correct, and safe to operate — even without a dedicated DBA.

**Why FreezerManager?**

- **Data safety by design.** Append-only hash-chained audit log. Every mutation
  is atomic, traceable, and verifiable. Soft-delete only — no rows are ever
  silently lost.
- **PHI-aware.** Field-level encryption, PHI-read audit events, and
  least-privilege RBAC ensure compliance without slowing down daily workflows.
- **Pluggable storage.** Swap between SQLite (dev / small labs) and PostgreSQL
  (production) without touching a line of domain code. Both pass the same
  abstract backend conformance suite.
- **Real freezer modeling.** Recursive container hierarchies (compartment →
  shelf → rack → drawer), mixed-format box templates with per-position size
  compatibility, and atomic sample moves.
- **Multi-user, multi-lab.** Strict per-lab isolation enforced at both the
  application layer and the database (Row-Level Security on PostgreSQL).
  Five built-in roles; lab admins define custom roles with scoped permissions.
- **Full chain of custody.** Aliquot lineage, check-out/check-in with volume
  tracking, and cross-lab share requests with three-signature approval.
- **Modern toolchain.** C++20, CMake 3.25+, Conan 2 lockfiles, clang-tidy,
  sanitizer CI builds, and TDD throughout (1080+ tests).

> **Status:** Pre-alpha — active implementation. Core domain, both reference
> backends (SQLite + PostgreSQL), auth foundation, the audit chain, the full
> gRPC service layer (9 services), the M1 CLI/CSV surface, and the REST/JSON
> gateway (Drogon front door over the gRPC in-process channel, all 9 services)
> are complete; security-remediation pass done. The **Qt 6 desktop client** is
> now usable end-to-end (login, lab/freezer/box tree, virtualized sample browser,
> box drag-and-drop grid, barcode scan, CSV export + **CSV import wizard** with a
> dry-run validation report); local password enrolment
> (`freezerctl user set-password`) makes login work end-to-end. Next: SSE
> streaming (live sample list + import progress), web/Python clients.
> See [`doc/PRD.md`](./doc/PRD.md) for the full product requirements & design
> document and the [Roadmap](#roadmap) below for current progress.

---

## Contents

- [Features](#features)
  - [Storage & Data Safety](#storage--data-safety)
  - [Domain Model](#domain-model)
  - [Security & Authentication](#security--authentication)
  - [Audit](#audit)
  - [Interfaces & Clients](#interfaces--clients)
  - [Operations](#operations)
  - [Developer Experience](#developer-experience)
- [Roadmap](#roadmap)
- [Building](#building)
  - [Code Coverage](#code-coverage)
  - [Performance Tests](#performance-tests)
- [Documentation](#documentation)
- [License](#license)

---

## Features

Status indicators: ✅ implemented · ⚙️ in progress · 🔲 planned

### Storage & Data Safety

- ✅ `IStorageBackend` pluggable abstraction — swap backends without touching domain code
- ✅ Typed query DSL — equality, range, IN-list, JSON-path predicates, pagination, soft-delete-aware filters
- ✅ Rich backend error hierarchy — `UniqueViolation`, `SerializationFailure`, `Unavailable`, etc.
- ✅ SQLite reference backend (dev / small labs) — WAL mode, busy-timeout, json1 extension
- ✅ PostgreSQL reference backend (production-recommended) — connection pool, RLS policies, migrations 0001–0013, JSONB, full domain repositories; conformance + repository suites green against a live `postgres:16`
- ✅ Atomic sample moves — single transaction, either both vacate + place succeed or neither does (`storage::move_sample`)
- ✅ No-double-booking invariant — partial unique index on `(box_id, position_label)` for active samples; property-tested
- ✅ Cross-lab referential integrity — sample foreign keys (box, item-type, container-type, parent, project, checkout-event) pinned to the owning lab; parent-lineage cycles rejected
- ✅ Soft-delete only — end-user "delete" tombstones the row; hard delete is `SystemAdmin`-only and audited

### Domain Model

- ✅ Strongly-typed domain IDs (`LabId`, `UserId`, `SampleId`, `BoxId`, …) — compile-time mix-up prevention
- ✅ `Volume` / `Mass` value types with unit enums — arithmetic across units rejected at compile time
- ✅ UTC microsecond `Timestamp` — all storage and transport in UTC; local conversion at display only
- ✅ Domain enums — `SampleStatus`, `CheckoutAction`, `RoleKind`, `ContainerKind` with JSON conversion
- ✅ `Lab`, `User`, `LabMembership` — every entity scoped by `lab_id`; strict multi-lab isolation
- ✅ `Role`, `Permission`, `RolePermission` — five built-in roles; lab admins define custom roles
- ✅ `Freezer` + `StorageContainer` — recursive adjacency-list hierarchy (compartment / shelf / rack / drawer)
- ✅ `BoxType` + `Position` — template with per-position `size_class` acceptance list; supports mixed-format boxes
- ✅ `Box` — instance of a `BoxType` placed in a `StorageContainer`
- ✅ `ContainerType` — physical tube/vial type with `size_class` token
- ✅ `ItemType` — hierarchical taxonomy per lab; custom field definitions inherited from ancestors
- ✅ `CustomFieldDefinition` — `string | int | float | bool | date | datetime | enum | reference`; `is_phi` flag
- ✅ `Sample` — full lifecycle: `active → checked_out → active`, `→ depleted`, `→ tombstoned`; no-double-booking enforced by partial unique index
- ✅ Parent–child aliquot lineage — child is independent; lineage preserved across soft-delete
- ✅ Volume / mass tracking — `CheckoutEvent.volume_delta`; auto-depletes at zero
- ✅ `Project` grouping + `SampleProject` many-to-many links
- ✅ `CheckoutEvent` — append-only chain-of-custody records with volume delta tracking
- ✅ `ShareRequest` cross-lab sharing workflow — pending/approved/rejected/revoked state machine; three-signature approval (source_admin + target_admin + system_admin); append-only `ShareRequestApproval` audit records
- ✅ `Session` + `ApiToken` — server-side opaque sessions and scoped API tokens; Argon2id hash + plaintext prefix lookup scheme; soft-delete by `revoked_at`; partial unique index prevents prefix collisions among active tokens

### Security & Authentication

- ✅ `IAuthProvider` interface — `authenticate`, `validate_token`, `verify_totp`, `revoke_session`, `revoke_all_sessions`; 5 pure-virtual methods; mock compiles
- ✅ `LocalAuthProvider` — Argon2id (64 MiB / 3 iter / 4 parallelism); BLAKE2b-256 session-token hashing; TOTP (RFC 6238 + RFC 4226 HMAC-SHA1 via OpenSSL EVP_MAC); in-memory account lockout (configurable threshold + duration); API-token validation path
- ✅ Local password enrolment — `freezerctl user set-password` (reads the password from stdin, never argv) writes the `{provider:local, hash}` binding `authenticate` consumes; a password-only user (no TOTP secret) logs straight in. TOTP enrolment 🔲
- ✅ TOTP helper (`Totp.h`) — `base32_decode`, `totp_generate`, `totp_verify`; ±1-step window; RFC 6238 known-answer test vectors
- ✅ `Session.mfa_complete` — false until `verify_totp()` succeeds; propagated through `SessionContext`
- 🔲 `OidcAuthProvider` — OIDC discovery + PKCE; per-lab issuer config
- 🔲 `LdapAuthProvider` — bind + search; configurable group → role mapping
- 🔲 `MtlsAuthProvider` — client certs for machine/instrument clients
- ✅ `AuthMiddleware` — 4-step RBAC gate (token → MFA → permission → lab); per-lab permission resolution; `inject_rls_vars()` sets Postgres session vars; static RPC permission registry; session expiry (12 h idle / 7 d absolute); permission-context cache (5 min TTL, invalidated on revoke **and** on `users.authz_version` bump, so role/membership downgrades take effect next request)
- ✅ API-token scope enforcement — fail-closed scope parsing (`["*"]` = unrestricted); token restricted to its `lab_id`; disabled-user check on every validation
- ⚙️ PHI field-level encryption — Sample PHI custom fields split out of the plaintext blob and AEAD-encrypted (`crypto_secretbox`) with a fresh per-record DEK wrapped by the master KEK; decrypted on read only for callers holding `phi.read`. `is_phi`∧`indexed` rejected at definition time. (Sample done; other entities 🔲)
- ✅ `IKmsProvider` envelope KMS + keyring — `KeyringKms` holds an active KEK plus retired KEKs (records wrapped under an older KEK still decrypt during rotation); `EnvVarKms` (dev/test, `FMGR_MASTER_KEK` + `FMGR_MASTER_KEK_PREVIOUS`) and `OsKeyringKms` (production, systemd-creds `$CREDENTIALS_DIRECTORY/master_kek`). `VaultKms` 🔲
- ✅ Key rotation — `freezerctl key rotate` re-wraps every sample's per-record DEK under the active KEK (field ciphertext untouched, no plaintext exposed); idempotent, audited. `key.rotate` permission reserved for a future online RPC
- 🔲 TLS 1.3 only — HSTS; modern cipher suites; self-signed cert for dev, required for production

### Audit

- ✅ Append-only `audit_event` table — `prev_hash` + `this_hash = SHA-256(prev ‖ canonical_row)`; INSERT-only DB trigger; chain tail found by link structure under an advisory lock (fork-safe)
- ✅ Canonical-JSON serializer (RFC 8785 / JCS) — deterministic compact form for reproducible hashes
- ✅ Repository-derived snapshots — `before_json`/`after_json` are produced from authoritative entity state inside the repository, never supplied by the caller; no forgeable audit channel
- ✅ Hash-chain verifier — `freezerctl audit verify` walks the chain and reports first divergence
- ✅ PHI-read audit kind — distinct `action="phi.read"` chain event appended on each PHI disclosure via `ITransaction::note_phi_read`; records disclosed field key names only, never values
- 🔲 Signed nightly checkpoints — HMAC-SHA-256 with KMS key; stored in `audit_checkpoint` table
- 🔲 Audit export is itself audited; rows are immutable; corrections are compensating events

### Interfaces & Clients

- ✅ `.proto` definitions under `proto/` — source of truth; never edit generated code; 10 service files
- ✅ gRPC server — all 9 services; every RPC handler opens a transaction, does work, appends audit row, commits
- ✅ REST / JSON gateway — Drogon HTTP front door at `/api/v1/*`; forwards to the gRPC services over the in-process channel (reuses the RBAC gate + audit + transactions, no logic duplicated); JSON↔proto via proto3 JSON mapping. **All 9 services wired** (Auth/Session/Lab/Sample/Box/ItemType/Role/Audit/Share) with positive/negative authz integration tests. ⚙️ SSE streaming: live audit feed
(`GET /api/v1/audit/watch`, server-streaming `WatchAuditFeed` bridged to
`text/event-stream`); live sample list + bulk-import progress 🔲
- ⚙️ Qt 6 desktop client — ✅ login + session shell, ✅ Lab→Freezer→Container→Box tree, ✅ virtualized sample browser (cursor paging + structured filters), ✅ box drag-and-drop grid (server-validated placement, "size mismatch" toast), ✅ barcode-scanner focus mode (bulk check-in/out), ✅ CSV export from the sample list, ✅ CSV import wizard (file pick → server dry-run report → all-or-nothing commit → list refresh)
- 🔲 React / TypeScript SPA — feature parity with Qt client; live updates via SSE
- 🔲 `freezerctl-py` Python client — thin REST wrapper; Jupyter quick-start notebook with example plots
- ⚙️ CSV import — `freezerctl sample import` and the `SampleService.ImportSamples` RPC (gRPC + REST `/api/v1/sample/import`): transactional, all-or-nothing; `--dry-run`/`dry_run` per-row validation report (the RPC additionally probes each row against committed state); RFC 4180 reader skips the export header block. Samples done; remaining entity tables 🔲
- ✅ CSV export — chain-of-custody `freezerctl sample export` + `SampleService.ExportSamplesCsv` (gRPC + REST) for samples

### Operations

- ⚙️ Encrypted backups — SQLite: online `sqlite3_backup` hot copy, stream-encrypted (`crypto_secretstream`) under a separate backup KEK, with a content-hash manifest (`freezerctl backup create` / `restore`). In-server scheduled runner (`BackupScheduler`, enabled by `FMGR_BACKUP_DIR`) + GFS retention (default 30 daily / 12 monthly / 7 yearly) + `freezerctl backup run | list` ✅. PostgreSQL backup 🔲
- ✅ Separate backup key — `IKmsProvider` backup KEK (`kms::make_backup_kms`: `backup_kek` systemd-cred / `FMGR_BACKUP_KEK`), independent of the master KEK, so loss of the live master key alone does not decrypt backups
- ✅ Restore-drill — `freezerctl backup verify` decrypts a backup, runs `PRAGMA integrity_check`, and verifies its audit hash chain; never throws on a bad backup (reports FAIL) so it is schedule-safe. Weekly automated drill (random-backup pick, `backup.drill` audit, error-level page on failure) runs inside `BackupScheduler` and on each `backup run`
- 🔲 Prometheus `/metrics` — RPC latency histograms, error rates, audit append latency, backup status
- 🔲 OpenTelemetry tracing — opt-in via `FMGR_OTLP_ENDPOINT` env var
- 🔲 Structured JSON logs to stdout — journald-friendly; PHI never written to logs
- 🔲 TOML configuration — `/etc/freezerd/freezerd.toml`; env-var overrides; fail-fast validation

### Developer Experience

- ✅ CMake 3.25+ with C++20; Conan 2 lockfile for reproducible dependency resolution
- ✅ CMake presets — `dev`, `release`, `asan`, `ubsan`, `tsan`, `release-deterministic`, `coverage`
- ✅ GitHub Actions CI — gcc-13 / clang-17 × Debug / Release × sanitizer builds; Conan cache
- ✅ clang-format + clang-tidy enforced in CI; SPDX header checks on every source file
- ✅ CLA Assistant Lite bot — required before any PR is merged
- ✅ Abstract backend conformance suite — parameterized GoogleTest fixtures; new backend = pass the suite (SQLite + in-memory pass; Postgres suite skips without `FMGR_TEST_POSTGRES_URL`)
- ⚙️ Property tests (RapidCheck) — box geometry invariants (no-double-booking), audit-chain integrity (intact/tampered)
- ⚙️ Fuzz harnesses — rate-limiter invariants, UUID generation, custom-field validator crash-safety; CSV importer, canonical-JSON serializer 🔲
- ⚙️ Authorization tests — `AuthMiddleware` 4-step gate covered in `auth_middleware_test.cpp`; per-RPC positive/negative tests land with the gRPC server

---

## Roadmap

| Milestone | Scope | Status |
|---|---|---|
| **M0 — Foundation** | Repo skeleton, CI/CD, CMake + Conan, clang-format/tidy, SPDX enforcement, CLA bot | ✅ Complete |
| **C1/C2 — Core types & storage interface** | Domain value types, `IStorageBackend` + typed query DSL, 15 unit tests | ✅ Complete |
| **D1–D9 — Domain entities** | Lab / User / Role / Freezer / Box / Sample / ShareRequest / Session entities, SQLite backend, 259 tests | ✅ Complete |
| **E1/E2 — Auth foundation** | `IAuthProvider` interface; `LocalAuthProvider` (Argon2id + TOTP + lockout); 357 tests total | ✅ Complete |
| **E3 — RBAC middleware** | `AuthMiddleware` (4-step gate, RLS injection, RPC registry); session expiry + permission cache (D9.3) | ✅ Complete |
| **C5 — PostgreSQL backend** | `PostgresBackend` + connection pool + Postgres-dialect migrations 0001–0013 + RLS policies + full domain repositories; conformance + repository suites green against live `postgres:16` in CI | ✅ Complete |
| **M1 — Full domain + CSV + CLI** | PostgreSQL domain repositories ✅, CI Postgres service ✅, sample CSV export ✅, sample CSV import (transactional + dry-run) ✅, `freezerctl` skeleton + `audit verify` ✅, freezer/box/item-type `list` + `inspect` ✅, CLI `create` nouns (6 entities) ✅, non-sample CSV import (item-type/box/custom-field-def/user) ✅ | ✅ Complete |
| **Security remediation** | Per-lab authz, API-token scope, `authz_version` cache invalidation, cross-lab integrity, fork-safe audit chain, repository-derived audit snapshots | ✅ Complete |
| **M2 — Auth & Audit** | OIDC/LDAP, audit export, PHI-read audit kind, signed checkpoints | 🔲 Planned |
| **M3 — gRPC + Qt client** | Proto definitions ✅, gRPC server (9 services) ✅, REST gateway — all 9 services over Drogon ✅, `SampleService.ImportSamples` RPC (transactional CSV import + dry-run) ✅, SSE streaming — live audit feed ✅ (sample list + import progress 🔲), Qt 6 desktop client — login + tree + sample browser + box grid + barcode + CSV export ✅ + CSV import wizard ✅ | ⚙️ In progress |
| **M4 — Web UI** | React / TypeScript SPA, live updates via SSE | 🔲 Planned |
| **M5 — PHI + KMS + Backups** | Sample PHI field-level encryption ✅, `IKmsProvider` + `EnvVarKms` + `OsKeyringKms` (keyring) ✅, PHI-read audit kind ✅, key rotation (`freezerctl key rotate`) ✅, separate backup KEK ✅, encrypted SQLite backup/restore + restore-drill verify (`freezerctl backup`) ✅, in-server scheduled backups + GFS retention + weekly restore drill (`BackupScheduler`, `freezerctl backup run | list`) ✅; `VaultKms`, Postgres backup 🔲 | ⚙️ In progress |
| **M6 — Public API & Sharing** | API tokens, `freezerctl-py`, cross-lab share-request workflow | 🔲 Planned |
| **M7 — Polish & 1.0** | OIDC + LDAP auth, `.deb` / `.rpm` / Docker packaging, external security review | 🔲 Planned |

---

## Building

Prerequisites: CMake ≥ 3.25, Conan 2, Ninja, GCC 13+ or Clang 17+.

```sh
conan profile detect --force
conan install . --lockfile=conan.lock --output-folder=out/conan/dev \
    --build=missing -s build_type=Debug -s compiler.cppstd=20
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

For sanitizer builds substitute `dev` with `asan`, `ubsan`, or `tsan`.

> **Note:** Always pass `-s compiler.cppstd=20` to `conan install` — Conan's
> auto-detected default profile may use an older standard, and `libpqxx`
> requires C++20.

### Code Coverage

Install `gcovr` (≥ 7.0), then use the `coverage` preset:

```sh
# Install coverage tool
sudo apt install gcovr

# Configure, build, and test with coverage instrumentation
conan install . --output-folder=out/conan/coverage --build=missing \
    -s build_type=Debug -s compiler.cppstd=20
cmake --preset coverage
cmake --build --preset coverage
ctest --preset coverage

# Generate HTML coverage report (like Go's `go tool cover -html`)
gcovr -r . --filter 'src/' --html --html-details -o coverage.html

# Terminal summary only
gcovr -r . --filter 'src/' --print-summary
```

The build directory `out/build/coverage/` is isolated from the default `dev`
build and can coexist with it.

### Performance Tests

Performance tests are planned under `tests/benchmark/` (to be committed in a
follow-up PR).  They will use GTest with manual `std::chrono` timing rather
than Google Benchmark, to avoid a static library template-instantiation linker
issue (see below).

**Status:** No tests are committed yet.  The investigation below documents the
plan and the known blocker.

| Benchmark | Status | What it measures |
|---|---|---|
| Canonical JSON + audit hash pipeline | 🔲 Planned | `Sample → to_json → canonical_json → compute_audit_hash` |
| Custom-field validator | 🔲 Planned | `validate_custom_fields()` against 1–100 field definitions |
| Sample placement (insert + commit) | 🔲 Blocked | End-to-end `SampleRepository::insert()` + audit append + commit |
| Sample list/filter (query) | 🔲 Blocked | `Query<Sample>` with compound filter, sort, and limit |
| Audit append (mutation + audit row) | 🔲 Blocked | `Lab::insert()` + atomic audit row append + commit |

**Known blocker (cross-TU static library linker issue):** The
`register_*_repositories()` template functions (`register_identity_repositories`,
`register_layout_repositories`, etc.) instantiate `std::function`-wrapped
lambdas inside static library object files. When these registration calls live
in a separate translation unit from the test code that calls
`ITransaction::repo<Entity>()`, the linker may drop some template
instantiations, causing runtime `UnsupportedOperation: repository is not
available for entity type`. The workaround is to call the registration
functions directly from the same `.cc` file as the test code. A proper fix
should replace the per-entity `register_repository_factory` template with a
single non-template function that registers all entities at once, so the
template instantiations are never split across translation units.

---

## Documentation

- [`doc/PRD.md`](./doc/PRD.md) — full product requirements & design document
- [`TODO.md`](./TODO.md) — detailed implementation backlog by milestone
- [`CONTRIBUTING.md`](./CONTRIBUTING.md) — branching model, commit style, how to run tests
- [`SECURITY.md`](./SECURITY.md) — vulnerability-disclosure process and contact
- [`CODE_OF_CONDUCT.md`](./CODE_OF_CONDUCT.md) — Contributor Covenant 2.1
- [`CLA.md`](./CLA.md) — Contributor License Agreement

---

## License

FreezerManager is **dual-licensed**:

- **AGPLv3** for open-source / academic / non-commercial use — see
  [`LICENSE`](./LICENSE).
- **Commercial license** available from the project owner — see
  [`LICENSING.md`](./LICENSING.md).

All contributions require signing the Contributor License Agreement — see
[`CLA.md`](./CLA.md). The CLA Assistant bot will prompt you automatically
when you open a pull request. You may also add `Signed-off-by: Your Name
<email>` to every commit (`git commit -s`) as a secondary record.

Contact: Yuxin Ren — `yxren_CN@outlook.com`
