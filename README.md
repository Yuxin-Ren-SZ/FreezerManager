# FreezerManager

Open-core, self-hostable freezer / biospecimen management system for academic
and clinical research labs. Written in C++20, designed for data safety,
security (PHI-aware), extensibility, and multi-user concurrency.

> **Status:** Pre-alpha — active implementation (M0 + core foundations complete).
> See [`doc/PRD.md`](./doc/PRD.md) for the full product requirements & design
> document and the [Roadmap](#roadmap) below for current progress.

---

## Features

Status indicators: ✅ implemented · ⚙️ in progress · 🔲 planned

### Storage & Data Safety

- ✅ `IStorageBackend` pluggable abstraction — swap backends without touching domain code
- ✅ Typed query DSL — equality, range, IN-list, JSON-path predicates, pagination, soft-delete-aware filters
- ✅ Rich backend error hierarchy — `UniqueViolation`, `SerializationFailure`, `Unavailable`, etc.
- 🔲 SQLite reference backend (dev / small labs) — WAL mode, busy-timeout, json1 extension
- 🔲 PostgreSQL reference backend (production-recommended) — connection pool, RLS, JSONB GIN indexes
- 🔲 Atomic sample moves — single transaction, either both vacate + place succeed or neither does
- 🔲 No-double-booking invariant — partial unique index on `(box_id, position_label)` for active samples
- 🔲 Soft-delete only — end-user "delete" tombstones the row; hard delete is `SystemAdmin`-only and audited

### Domain Model

- ✅ Strongly-typed domain IDs (`LabId`, `UserId`, `SampleId`, `BoxId`, …) — compile-time mix-up prevention
- ✅ `Volume` / `Mass` value types with unit enums — arithmetic across units rejected at compile time
- ✅ UTC microsecond `Timestamp` — all storage and transport in UTC; local conversion at display only
- ✅ Domain enums — `SampleStatus`, `CheckoutAction`, `RoleKind`, `ContainerKind` with JSON conversion
- 🔲 `Lab`, `User`, `LabMembership` — every entity scoped by `lab_id`; strict multi-lab isolation
- 🔲 `Role`, `Permission`, `RolePermission` — five built-in roles; lab admins define custom roles
- 🔲 `Freezer` + `StorageContainer` — recursive adjacency-list hierarchy (compartment / shelf / rack / drawer)
- 🔲 `BoxType` + `Position` — template with per-position `size_class` acceptance list; supports mixed-format boxes
- 🔲 `Box` — instance of a `BoxType` placed in a `StorageContainer`
- 🔲 `ContainerType` — physical tube/vial type with `size_class` token
- 🔲 `ItemType` — hierarchical taxonomy per lab; custom field definitions inherited from ancestors
- 🔲 `CustomFieldDefinition` — `string | int | float | bool | date | datetime | enum | reference`; `is_phi` flag
- 🔲 `Sample` — full lifecycle: `active → checked_out → active`, `→ depleted`, `→ tombstoned`
- 🔲 Parent–child aliquot lineage — child is independent; lineage preserved across soft-delete
- 🔲 Volume / mass tracking — `CheckoutEvent.volume_delta`; auto-depletes at zero
- 🔲 `Project` grouping + `ShareRequest` cross-lab sharing workflow

### Security & Authentication

- 🔲 `IAuthProvider` interface — swap providers without touching higher-level code
- 🔲 `LocalAuthProvider` — Argon2id (64 MiB / 3 iter / 4 parallelism), TOTP mandatory for admins
- 🔲 `OidcAuthProvider` — OIDC discovery + PKCE; per-lab issuer config
- 🔲 `LdapAuthProvider` — bind + search; configurable group → role mapping
- 🔲 `MtlsAuthProvider` — client certs for machine/instrument clients
- 🔲 RBAC with per-resource scopes — effective perms computed once per session and cached
- 🔲 PHI field-level encryption — per-record DEK wrapped by master KEK; `IKmsProvider` pluggable
- 🔲 `OsKeyringKms` / `VaultKms` / `EnvVarKms` — production, Vault, and dev/test key sources
- 🔲 TLS 1.3 only — HSTS; modern cipher suites; self-signed cert for dev, required for production

### Audit

- 🔲 Append-only `audit_event` table — `prev_hash` + `this_hash = SHA-256(prev ‖ canonical_row)`
- 🔲 PHI-read audit kind — distinct event logged on each PHI field access (key only, never value)
- 🔲 Signed nightly checkpoints — HMAC-SHA-256 with KMS key; stored in `audit_checkpoint` table
- 🔲 Hash-chain verifier — `freezerctl audit verify` walks the chain and reports first divergence
- 🔲 Audit export is itself audited; rows are immutable; corrections are compensating events

### Interfaces & Clients

- 🔲 `.proto` definitions under `proto/` — source of truth; never edit generated code
- 🔲 gRPC server — every RPC handler opens a transaction, does work, appends audit row, commits
- 🔲 REST / JSON gateway — `/api/v1/*`; streaming RPCs bridged to SSE / WebSocket
- 🔲 Qt 6 desktop client — sample browser, box drag-and-drop grid, barcode-scanner focus mode, CSV import wizard
- 🔲 React / TypeScript SPA — feature parity with Qt client; live updates via SSE
- 🔲 `freezerctl-py` Python client — thin REST wrapper; Jupyter quick-start notebook with example plots
- 🔲 CSV import (transactional with dry-run validation report) and export for all entity tables

### Operations

- 🔲 Encrypted scheduled backups — PostgreSQL: `pg_basebackup` + WAL/PITR; SQLite: hot copy + rotation
- 🔲 Separate backup key — loss of live master key alone does not decrypt backups
- 🔲 Weekly restore-drill — pick a random backup, restore to temp DB, run integrity checks, alert on failure
- 🔲 Prometheus `/metrics` — RPC latency histograms, error rates, audit append latency, backup status
- 🔲 OpenTelemetry tracing — opt-in via `FMGR_OTLP_ENDPOINT` env var
- 🔲 Structured JSON logs to stdout — journald-friendly; PHI never written to logs
- 🔲 TOML configuration — `/etc/freezerd/freezerd.toml`; env-var overrides; fail-fast validation

### Developer Experience

- ✅ CMake 3.25+ with C++20; Conan 2 lockfile for reproducible dependency resolution
- ✅ CMake presets — `dev`, `release`, `asan`, `ubsan`, `tsan`, `release-deterministic`
- ✅ GitHub Actions CI — gcc-13 / clang-17 × Debug / Release × sanitizer builds; Conan cache
- ✅ clang-format + clang-tidy enforced in CI; SPDX header checks on every source file
- ✅ CLA Assistant Lite bot — required before any PR is merged
- 🔲 Abstract backend conformance suite — parameterized GoogleTest fixtures; new backend = pass the suite
- 🔲 Property tests (RapidCheck) — box geometry invariants, audit-chain integrity
- 🔲 Fuzz harnesses (libFuzzer) — RPC parsers, custom-field validator, CSV importer, canonical-JSON serializer
- 🔲 Authorization tests — every RPC has at least one positive and one negative auth test

---

## Roadmap

| Milestone | Scope | Status |
|---|---|---|
| **M0 — Foundation** | Repo skeleton, CI/CD, CMake + Conan, clang-format/tidy, SPDX enforcement, CLA bot | ✅ Complete |
| **C1/C2 — Core types & storage interface** | Domain value types, `IStorageBackend` + typed query DSL, 15 unit tests | ✅ Complete |
| **M1 — Domain entities** | Lab / User / Sample / Box / … entities, SQLite + PostgreSQL backends, CSV export, `freezerctl` CLI | 🔲 Next |
| **M2 — Auth & Audit** | Local auth + TOTP, RBAC middleware, audit hash chain, PostgreSQL RLS | 🔲 Planned |
| **M3 — gRPC + Qt client** | Proto definitions, gRPC server, REST gateway, Qt 6 desktop client — first end-to-end usable build | 🔲 Planned |
| **M4 — Web UI** | React / TypeScript SPA, live updates via SSE | 🔲 Planned |
| **M5 — PHI + KMS + Backups** | Field-level encryption, KMS adapters, backup/restore, weekly restore-drill | 🔲 Planned |
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
