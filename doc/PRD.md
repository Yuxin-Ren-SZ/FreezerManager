# Freezer Manager — Product Requirements & Design Document

**Status:** Pre-alpha — active implementation (design baseline 2026-05-06; last synced 2026-06-12).
Core domain, both reference backends (SQLite + PostgreSQL), auth foundation, audit
chain, and the full gRPC service layer are implemented; security-remediation pass done.
See the [README Roadmap](../README.md#roadmap) for live, milestone-by-milestone status.
**Owner:** Yuxin Ren (yxren_CN@outlook.com)
**Implementation language:** C++ (C++20)
**Target OS:** Linux (Ubuntu LTS primary)
**Methodology:** TDD — tests written before implementation for every public interface.

---

## 1. Context & Motivation

Lab research groups need to track biological/chemical samples stored in
freezers (–20 °C, –80 °C, LN₂). Existing solutions are commercial,
closed-source, expensive, or assume a fixed schema and a fixed hardware
layout. They are also rarely PHI-safe out of the box.

This project provides an **open-core, self-hostable, secure, extensible**
freezer-management system suitable for academic labs that may handle
sensitive (potentially PHI) biospecimen data.

### 1.1 Design priorities (in order)
1. **Data safety** — no silent loss; every mutation is auditable; recoverable.
2. **Security** — encryption at rest + in transit; least-privilege RBAC; PHI-safe.
3. **Extensibility** — pluggable storage backend; user-defined schema; pluggable GUI; pluggable hardware.
4. **Multi-user concurrency** — many users on one shared dataset, simultaneous reads and writes.
5. **Usability** — Qt desktop GUI + React web GUI + scriptable Python API for analysis.

### 1.2 Goals
- Track samples down to position-in-box, with full chain-of-custody.
- Support arbitrary lab-defined item types and custom fields.
- Support real-world freezer hierarchies and mixed-format storage boxes.
- Be deployable by a department's IT with standard Linux tooling.
- Be safe to operate without a dedicated DBA (sane defaults, automated backups).

### 1.3 Non-goals (v1)
- Multi-tenant SaaS with strict customer-isolation guarantees beyond per-lab RLS.
- Mobile native apps (the web UI must be responsive but native iOS/Android is out).
- Offline-first desktop client with sync.
- LIMS-grade workflow engine (assays, instrument scheduling, etc.).
- ELN (electronic lab notebook) functionality.
- Built-in IoT/temperature monitoring (interface only; reference impl out of scope).

---

## 2. Decisions Summary

| Area | Decision |
| --- | --- |
| Deployment | Single self-hosted server, **multi-lab on one deployment** with strict isolation |
| PHI | Designed-for; field-level encryption + stricter audit when PHI mode is on |
| Default storage backends | **SQLite** (dev/tiny labs) and **PostgreSQL** (production-recommended) |
| Storage abstraction | `IStorageBackend` interface; reference impls for SQLite & Postgres |
| Auth providers | Local (Argon2id + TOTP), OIDC/OAuth2, LDAP/AD, mTLS for machine clients |
| Authorization | RBAC with per-resource scopes; custom roles inside a lab |
| Audit | Append-only, hash-chained, signed checkpoints, ≥6 yr default retention |
| Network protocol | **gRPC** primary; REST/JSON gateway for web SPA & Python clients |
| Web stack | C++ backend (Drogon-class) + React/TypeScript SPA |
| Desktop GUI | Qt 6 (Widgets + QML), online-only |
| Public API | External REST + Python client lib + scoped, expiring tokens |
| Custom fields | JSON column + per-(lab, item-type) `CustomFieldDefinition` registry |
| Item types | Hierarchical taxonomy with field inheritance |
| Box geometry | Position-set model with per-position size-class compatibility |
| Hardware | Abstract interfaces only; mandatory CSV import/export |
| Lifecycle | Parent–child lineage, check-out/in, volume tracking, soft-delete only |
| Crypto | LUKS at rest + libsodium/pgcrypto field-level for PHI; master key in OS keyring/Vault |
| Backup/DR | Built-in encrypted scheduled backups + Postgres WAL/PITR; admin hooks |
| License | **Dual: AGPLv3 + commercial**; CLA required from contributors |
| i18n | UTF-8 everywhere; tr()/i18n() wrapped strings; English-only at v1 |
| Build/dist | CMake ≥3.25 + Conan 2 (lockfile); Debian/RPM packages + Docker image |

---

## 3. Personas & Roles

### Personas
- **Lab Member** — daily user. Logs in, stores samples, pulls samples, runs reports, exports CSV, calls the Python API for plotting. May or may not see PHI.
- **Lab Admin** — owns one Lab. Configures freezer layouts, defines item types and custom fields, manages members, approves cross-lab share requests, exports audit logs for their lab.
- **System Admin** — owns the deployment. Provisions Labs, configures auth providers, manages encryption keys, runs backups/restores, approves cross-lab shares (second signature), reviews global audit.
- **API/Machine Client** — a script or instrument. Uses an API token or mTLS cert; bound to a single user's permissions or a service account.

### Built-in roles
| Role | Scope | Notes |
| --- | --- | --- |
| `SystemAdmin` | global | Cannot read PHI fields by default unless PHI-admin role granted explicitly. |
| `LabAdmin` | one Lab | Full control within their lab. |
| `Member` | one Lab | Read+write samples; configurable per-freezer/project scope restrictions. |
| `ReadOnly` | one Lab | Read-only access; cannot check out. |
| `ApiClient` | one Lab, one user | Bundle inheriting from a parent user; expires. |

### Permission model
Permissions are `(action, entity)` pairs. Examples:
`sample.read`, `sample.write`, `sample.checkout`, `sample.delete_soft`,
`sample.delete_hard` (SystemAdmin only), `box.configure`, `freezer.configure`,
`custom_field.define`, `item_type.define`, `user.invite`, `audit.read`,
`audit.export`, `backup.run`, `share.request`, `share.approve`.

Lab admins may **define custom roles** inside their lab by combining permissions
that are themselves within their lab's scope. Roles can be further restricted by
**scope filters** (e.g., "Member, but only freezers F1 & F2").

PHI fields require a separate `phi.read` permission, granted only to users
explicitly authorized; viewing PHI is logged with a stronger audit event.

---

## 4. Domain Model

### 4.1 Core entities

```
Lab
  id, name, contact, created_at, settings_json
  is_phi_enabled (bool)

User
  id, primary_email, display_name, status, created_at
  auth_bindings: list of {provider, external_id}
  totp_secret_enc (nullable)
  default_lab_id

LabMembership
  user_id, lab_id, role_id, scope_filters_json
  invited_by, joined_at, revoked_at

Role / Permission / RolePermission        -- as described in §3

Freezer
  id, lab_id, name, location, model, temp_target_c
  layout_root_id  -> StorageContainer (the freezer's top-level container)

StorageContainer  (recursive: compartment / shelf / rack / drawer)
  id, lab_id, parent_id (nullable -> StorageContainer)
  kind (enum: compartment, shelf, rack, drawer, custom)
  name / label, ordering_index
  capacity_hint (rows, cols, depth — optional, advisory)

BoxType
  id, lab_id, name, manufacturer, sku
  positions: list of Position

Position  (defined as part of BoxType)
  label (e.g. "A1"), row, col, optional z (for tube racks)
  accepts: list of ContainerType.size_class

Box
  id, lab_id, box_type_id, parent_storage_container_id, label
  serial / barcode

ContainerType
  id, lab_id, name (e.g. "1.5mL Eppendorf", "50mL Falcon")
  size_class (string token; matched against Position.accepts)
  outer_dims_mm, material, supplier_sku

ItemType  (hierarchical)
  id, lab_id, parent_id (nullable), name
  -- e.g. liquid > blood, liquid > csf, solid > tissue, powder > enzyme
  -- field defs are inherited from ancestors

CustomFieldDefinition
  id, lab_id, scope_kind (sample|box|freezer|...), item_type_id (nullable for global)
  key, label, data_type (string|int|float|bool|date|datetime|enum|reference),
  required, validation_json, indexed (bool), is_phi (bool)

Sample
  id, lab_id, item_type_id, name / label, barcode
  container_type_id          -- physical tube/vial
  box_id (nullable), position_label (nullable)
  volume_value, volume_unit, mass_value, mass_unit (any may be null)
  status enum: active | checked_out | depleted | destroyed | tombstoned
  parent_sample_id (nullable)  -- aliquot lineage
  created_by, created_at, last_modified_by, last_modified_at
  custom_fields_json           -- validated against CustomFieldDefinition
  phi_fields_enc_json          -- per-field envelope-encrypted PHI columns

Project (optional grouping)
  id, lab_id, name, owner_user_id

SampleProject
  sample_id, project_id

CheckoutEvent
  id, sample_id, user_id, action (out|in|destroy), reason, at,
  volume_delta (nullable), location_after (nullable)

ShareRequest
  id, source_lab_id, target_lab_id, requested_by,
  scope_json (which samples/projects), state (pending|approved|rejected|revoked),
  approvals: [source_admin, target_admin, system_admin], created_at, decided_at

AuditEvent
  id, at, actor_user_id, actor_session_id, lab_id (nullable),
  action, entity_kind, entity_id,
  before_json, after_json, request_id, prev_hash, this_hash

Session
  id, user_id, created_at, last_seen_at, expires_at, revoked_at (nullable)
  mfa_complete (bool)          -- false until verify_totp() succeeds
  token_hash                   -- BLAKE2b-256 of the opaque server-side token

ApiToken
  id, user_id, lab_id, scope_json, created_at, expires_at, revoked_at (nullable)
  token_hash                   -- Argon2id hash; prefix stored plaintext for lookup
  -- partial unique index prevents prefix collisions among active tokens
```

### 4.2 Box geometry & size compatibility

A **BoxType** is a template; a **Box** is an instance of a BoxType placed inside
a `StorageContainer`. Each `Position` on a `BoxType` declares which
`ContainerType.size_class` tokens it accepts.

Server invariants enforced at the data layer (and re-tested in the abstract
backend test suite):
- A sample can only occupy a position whose `accepts` set includes its
  `ContainerType.size_class`.
- A position can hold **at most one active sample** (status ∈ {active,
  checked_out}). Tombstoned/destroyed samples may remain referenced for audit
  but do not occupy the slot.
- Moves are atomic: a `move(sample, src→dst)` is one transaction; either both
  the vacate and the place succeed, or neither does.

This generalizes both standard uniform grids (e.g., 9×9 cryobox; every position
accepts `cryovial_2mL`) and mixed-format boxes (e.g., the Eppendorf box with
3×3 positions for 50 mL tubes plus 2×2 positions for 15 mL tubes).

### 4.3 Item-type taxonomy & custom fields

Item types form a tree per lab (e.g., `liquid → blood`, `liquid → csf`,
`solid → tissue`, `powder → enzyme`). A `CustomFieldDefinition` may be attached
at any node; descendants inherit it. A child node may **add** new fields and
**tighten** validation on inherited fields, but cannot remove an inherited
required field. (Removal requires editing the ancestor and triggers a
migration check.)

All members of a Lab share visibility of all of that Lab's item types and
fields. Cross-lab visibility requires an approved `ShareRequest`.

### 4.4 Sample lifecycle
- **Aliquoting / parent–child:** A child sample records `parent_sample_id`.
  The child is independent: depleting parent does NOT deplete children, and
  vice-versa. The lineage edge is preserved across soft-delete so children
  retain provenance even if the parent is tombstoned.
- **Check-out / check-in:** A user "checks out" a sample to take it for use;
  status transitions `active → checked_out`. They check it back in (possibly
  with an updated volume) or mark it destroyed. Open check-outs are visible
  on dashboards and time-stamped for chain-of-custody.
- **Volume/mass tracking:** Optional per item type. Each `CheckoutEvent` may
  carry a `volume_delta`; reaching zero auto-marks the sample `depleted`.
- **Soft-delete only:** End-user "delete" sets status `tombstoned` and hides
  the row from default queries. The row remains for audit. A
  `sample.delete_hard` permission, granted only to `SystemAdmin`, exists for
  legal-hold or right-to-erasure scenarios; hard delete is itself audited and
  produces a tombstone in the audit chain.

---

## 5. Storage Abstraction

```cpp
// Pseudocode interface — exact API decided by TDD test suite first.
class IStorageBackend {
public:
  virtual ~IStorageBackend() = default;

  // Lifecycle
  virtual void migrate_to_latest() = 0;
  virtual SchemaVersion current_version() const = 0;

  // Transactions
  virtual std::unique_ptr<ITransaction> begin(IsolationLevel) = 0;

  // Capability flags so callers can degrade gracefully:
  virtual Capabilities caps() const = 0;
  // e.g. { row_level_security, jsonb_indexes, native_uuid, listen_notify }

  // Generic CRUD via typed repositories obtained through the transaction:
  //   ITransaction::repo<Sample>(), repo<Box>(), ...
  // Each repository has find_by_id, query(spec), insert, update, soft_delete.
  // Spec is a typed query DSL (no raw SQL leaked to higher layers).

  // Audit hook: every mutating call inside a tx appends to the audit chain
  // before commit; commit fails atomically if audit append fails.
};
```

- v1 reference impls: `SqliteBackend` and `PostgresBackend`.
- Both impls run the **same abstract test suite** (Google Test parameterized
  fixtures). Adding a new backend is "implement the interface, pass the test
  suite."
- Postgres impl additionally enables row-level security policies keyed on a
  per-connection `app.current_user_id` / `app.current_lab_ids` setting, so
  even a SQL-injection-style escape inside the app cannot bypass lab
  isolation. SQLite enforces the same in code with an explicit acknowledgment
  in the deployment docs that DB-admin trust is required.

---

## 6. Network Protocol

- **`.proto` files are the source of truth.** All RPCs, messages, and error
  codes are generated.
- **gRPC** is used by the Qt client and by performance-sensitive server-to-server
  callers.
- A **REST/JSON gateway** (e.g., `grpc-gateway` semantics, hand-written if needed
  for a pure-C++ stack) exposes the same RPCs at `/api/v1/*` for the React SPA
  and for the Python client.
- **Streaming RPCs** are used for: live sample-list updates within a freezer view,
  live audit feed for admins, and bulk import progress.
- All endpoints require auth except `/health` and `/auth/*`.
- TLS 1.3 only; HSTS; modern cipher suites only. Self-signed cert OK for dev,
  not for production (deployment doc states this clearly).

---

## 7. Authentication, Authorization, Audit

### 7.1 Authentication
`IAuthProvider` interface; v1 implementations:
- `LocalAuthProvider`: Argon2id-hashed passwords; **TOTP required** for
  `LabAdmin` and `SystemAdmin`, configurable per-lab for `Member`.
- `OidcAuthProvider`: standard OIDC discovery + PKCE; per-lab issuer config.
- `LdapAuthProvider`: bind+search; configurable group → role mapping.
- `MtlsAuthProvider`: client certs for machine clients; cert pinned to a
  service-account user.

API tokens are short-lived (default 30 d), per-scope, revocable, and shown
once. Tokens are stored as Argon2id hashes; the prefix is plaintext for
identification.

### 7.2 Authorization
- Every RPC declares a required permission.
- The server resolves the caller's effective permissions from
  (role memberships) ∪ (scope filters) once per session and caches them.
- Lab isolation is enforced at two layers: app-level guard + Postgres RLS.

### 7.3 Audit
- Append-only `audit_event` table.
- Each row stores `prev_hash` + `this_hash = SHA-256(prev_hash || canonical_row)`.
- A nightly scheduled job signs the latest `this_hash` with an HMAC key kept
  outside the DB (systemd-creds / Vault) producing a checkpoint.
- Tampering is detected on any subsequent verification run.
- PHI-field reads are audited as a distinct event kind.
- Audit export is itself audited. Audit rows are immutable; "fix" is a new
  compensating event, never an UPDATE.

---

## 8. Cryptography & PHI Handling

- **At-rest:** LUKS or equivalent disk encryption is **required** for
  production deployments (documented; not enforced by the app).
- **Field-level encryption:** PHI-tagged fields are encrypted with libsodium
  `crypto_secretbox` (or pgcrypto in Postgres mode) using a per-record DEK
  wrapped by a master KEK.
- **Key sources (pluggable `IKmsProvider`):**
  - `OsKeyringKms` — default, key from systemd-creds.
  - `EnvVarKms` — for development/tests only.
  - `VaultKms` — HashiCorp Vault transit engine.
  - Future: AWS/GCP KMS adapters.
- **Backups** are encrypted with a **separate** backup key; loss of the live
  master key alone does not decrypt backups, and vice versa.
- **In-transit:** TLS 1.3 server-side; mTLS optional for machine clients.

---

## 9. Local GUI (Qt 6)

- C++ Qt 6 app, **online-only**, talks to server via gRPC.
- Uses Qt Widgets for dense forms and tables (sample browser, box layout
  grids), and QML for visualization panes (freezer 3D layout, fill heatmaps).
- Critical UX flows v1:
  - Sample search (full-text + structured filter + custom-field filter).
  - Box view: visual grid; drag-and-drop placement; placement-rejection
    surfaces a clear "size mismatch" error from server.
  - Bulk check-in/check-out with barcode scanner focus mode.
  - CSV import wizard with dry-run validation report.
  - CSV export for any list view.
- LGPL-compliant linkage; build instructions document dynamic-link requirement
  if commercial license isn't held.

---

## 10. Web UI

- React + TypeScript SPA, served as static assets by the C++ backend (or any
  reverse proxy).
- Same feature surface as the Qt client except where intrinsically desktop
  (e.g., USB scanner integration is browser-API-limited).
- Component lib: a permissively-licensed grid (e.g. AG Grid Community / TanStack
  Table); decision deferred — not architectural.
- Live updates via WebSocket/SSE bridging streaming gRPC RPCs through the REST
  gateway.

---

## 11. Public Scripting / Plotting API

- External REST/JSON only. **No code runs on the server.**
- Per-user, per-scope, expiring tokens. Tokens created in the UI; one-time
  display; revocable.
- Server enforces same RBAC as a UI session, plus rate limits configurable per
  role (default 60 req/min for `Member`-token, higher for `LabAdmin`).
- A small reference Python client (`freezerctl-py`) wraps the API and ships
  with a Jupyter quick-start notebook showing how to plot freezer fill
  histograms, sample age distributions, etc.
- Every API call audited with the token's identity.

---

## 12. Hardware Abstraction

```cpp
class IBarcodeScanner { virtual std::string read() = 0; ... };
class ILabelPrinter   { virtual void print(const LabelTemplate&, const FieldMap&) = 0; ... };
class IRfidReader     { ... };
class ITemperatureSensor { virtual Reading sample() = 0; ... };
```

- v1 ships **no production-grade hardware drivers**. A `HidKeyboardScanner`
  pass-through (any USB barcode wedge that types into the focused field) is
  the only adapter. CSV export covers any other workflow.
- Users are documented as the integration point: "Implement these C++
  interfaces, drop a shared library into `/etc/freezerd/plugins`, restart."

---

## 13. CSV Import / Export

- All entity tables are CSV-exportable through a single export RPC.
- Import supports: samples, boxes, item types, custom-field definitions,
  users (admin only).
- Imports are **transactional** with a dry-run mode that produces a per-row
  validation report without writing.
- Exported CSVs include a header comment with schema version + lab id +
  export timestamp + signature for chain-of-custody-grade exports.

---

## 14. Backup & DR

- Built-in scheduled backup runner inside the server process:
  - Postgres: `pg_basebackup` baseline + continuous WAL archiving for PITR.
  - SQLite: `sqlite3_backup` hot copy + daily snapshot rotation.
- All backups encrypted with a backup key distinct from the live master key.
- Default schedule: nightly full + 5-min WAL on Postgres; nightly hot copy on SQLite.
- Configurable retention (default: 30 daily, 12 monthly, 7 yearly).
- A **restore drill** command (`freezerctl restore --dry-run`) verifies a
  randomly-selected backup once a week; failures page the system admin.
- All backup events audited.

---

## 15. Test Strategy (TDD)

Tests are written **before** implementation for every public interface.

- **Unit tests** (Google Test): every module, each public function.
- **Property tests** (RapidCheck): box geometry invariants (no double-occupancy;
  size compatibility; move atomicity); audit-chain integrity under random
  interleavings.
- **Abstract backend conformance suite**: parameterized over every
  `IStorageBackend` impl. Adding a backend = passing this suite.
- **Integration tests**: real PostgreSQL in a container; real SQLite file.
- **Migration tests**: every schema migration has an up+down test against
  representative seed data; PR cannot merge if migration test missing.
- **Fuzz tests** (libFuzzer): all RPC parsers, custom-field validators,
  CSV importer.
- **Authorization tests**: every RPC has at least one positive and one
  negative authorization test (one user who can call it, one who can't).
- **Concurrency tests**: stress-test concurrent box-position writes for
  the no-double-occupancy invariant.
- **End-to-end smoke** (in CI): start server in a container, run a Python
  client script that creates a lab, a freezer, a box, samples, checks out,
  exports CSV, restores from a backup.

CI gates: all of the above + clang-tidy + ASan + UBSan + TSan builds.

---

## 16. Build, Packaging, Deployment

- **Build:** CMake ≥3.25 + Conan 2 with a committed lockfile for reproducible
  dependency resolution.
- **Compiler:** GCC 13+ / Clang 17+; C++20.
- **Distribution:**
  - Debian/Ubuntu `.deb` and Fedora/RHEL `.rpm` packages with a
    `freezerd.service` systemd unit, `journald` logging, log-rotate config,
    and a default `/etc/freezerd/freezerd.toml`.
  - Official Docker image (Alpine or Debian-slim base) for evaluation and
    container-native deployments.
- **Reproducible builds**: pinned dependency versions; `cmake --preset
  release-deterministic`.
- **First-run wizard** (CLI + web): create system admin, create first lab,
  generate master key (or wire to KMS).

---

## 17. Logging & Observability

- Structured JSON logs to stdout (journald-friendly).
- Prometheus `/metrics` endpoint (RPC latency histograms, error rates, audit
  append latency, backup status).
- OpenTelemetry tracing optional via env-var configuration.
- No PHI ever written to logs (enforced by a `redact()` wrapper around
  request-logging middleware; tested).

---

## 18. License & Contribution

- **Dual license:**
  - Public/research/non-commercial use: **AGPLv3**.
  - Commercial use: separate commercial license sold by the project owner
    (Yuxin Ren / future entity).
- **Contributor License Agreement (CLA):** required from all external
  contributors, assigning copyright/patent rights to the project owner so
  the dual-license model remains viable.
- The owner retains the right to file patents on novel inventions in the
  code; the AGPLv3 grant covers downstream users for the patents the
  AGPLv3's patent clause covers.
- Trademark policy: documented separately; project name and logo reserved.

---

## 19. Roadmap (rough)

> **Live status lives in the [README Roadmap](../README.md#roadmap)** — that table
> is the single source of truth and is updated per merged PR. The week estimates
> below are the original plan; status tags here are a coarse snapshot only.

**M0 — Skeleton (weeks 1–2). ✅ Complete.** Repo, CI, CMake, license headers, CLA bot,
abstract `IStorageBackend` + SQLite stub passing minimal conformance tests.

**M1 — Core domain (weeks 3–6). ⚙️ In progress.** Lab/User/Membership/Role; Freezer/
StorageContainer/BoxType/Box/ContainerType/Position; Sample with placement
rules. TDD throughout. CSV export of samples. Local CLI `freezerctl`.
(Domain + sample CSV export + `freezerctl audit verify` done; CSV import +
remaining CLI nouns outstanding.)

**M2 — AuthN/Z & audit (weeks 7–9). ✅ Local auth + RBAC + audit chain complete.**
Local auth + TOTP; RBAC; audit chain; RLS in Postgres impl. (OIDC/LDAP, audit
export, PHI-read kind, signed checkpoints deferred to later milestones.)

**M3 — gRPC server + Qt client (weeks 10–14). ⚙️ gRPC server complete; Qt client planned.**
First end-to-end usable product. All 10 gRPC services (Auth, Session, Lab, Box,
ItemType, Sample, Role, Audit, Share) implemented; REST gateway + online-only
desktop client still to come.

**M4 — Web UI (weeks 15–18). 🔲 Planned.** REST gateway, React SPA covering core flows.

**M5 — PHI mode + KMS + backups (weeks 19–22). 🔲 Planned.** Field-level encryption,
KMS adapters, backup/restore, restore drills.

**M6 — Public Python API & sharing (weeks 23–26). 🔲 Planned.** External API tokens,
`freezerctl-py`, cross-lab share-request workflow.

**M7 — Polish & 1.0 (weeks 27–30). 🔲 Planned.** OIDC + LDAP auth, packaging, docs,
security review.

---

## 20. Risks & Mitigations

| Risk | Mitigation |
| --- | --- |
| Silent data loss from a buggy migration | Mandatory up+down migration tests; restore drill weekly. |
| PHI leak via logs or error messages | `redact()` wrapper + lint rule + tests on log output. |
| Concurrent placement double-booking a position | DB-level unique constraint on (box_id, position_label) WHERE status active; concurrency stress test. |
| AGPL scaring off academic IT | Clear documentation that internal academic use is fine; commercial license available for industry partners. |
| Abstraction layer leaking SQL dialects | All higher-level code uses typed query DSL; raw SQL banned by code review checklist. |
| SQLite single-writer surprise in production | Docs + first-run wizard warn if concurrent users > 5 on SQLite and recommend Postgres. |

---

## 21. Verification Plan (for the eventual implementation)

When code is later written, the following must all be true before tagging 1.0:
- [ ] All abstract backend tests pass on both SQLite and Postgres impls.
- [ ] ASan + UBSan + TSan CI builds green.
- [ ] Concurrency stress test: 50 simulated members performing 10k placements
      yields zero invariant violations.
- [ ] Audit chain verifier passes after a 24-hour fuzz run that interleaves
      mutating RPCs and process restarts.
- [ ] PHI-mode end-to-end test: encrypted PHI field never appears in plaintext
      in logs, backups (without backup key), or to users without `phi.read`.
- [ ] Backup → wipe DB → restore → all data + audit chain intact.
- [ ] An independent security reviewer (external) signs off on auth, RBAC, and
      crypto code.

---

## 22. Items Explicitly Deferred Past v1

- Multi-tenant SaaS hosting model.
- Mobile native apps.
- Offline desktop sync.
- In-server sandboxed scripting.
- Real-time IoT temperature monitoring (interface yes, drivers no).
- Translations beyond English.
- Built-in plate (96/384-well) workflows beyond modeling them as a BoxType.
