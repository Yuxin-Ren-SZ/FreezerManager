# FreezerManager Codebase Review — 2026-07-03

Reviewer: Hermes Agent
Branch/worktree reviewed: `feat/grpc-tls-c9` working tree

## Scope and verification

This review inspected the current working tree, not a clean committed baseline.
At review time the worktree had substantial uncommitted changes:

- 30 modified tracked files
- 14 untracked files
- notable untracked/changed areas: gRPC TLS, login-attempt persistence, TLS docs/fixtures

Commands run:

```sh
git status --short
git branch --show-current
git log --oneline -5
cmake --build --preset dev -j 2
ctest --preset dev -N
ctest --preset dev -j 2 --output-on-failure
```

Observed verification result:

- Build: PASS
  - `cmake --build --preset dev -j 2` completed successfully.
  - Build emitted warnings in tests; details below.
- Test discovery: PASS
  - `ctest --preset dev -N` reported 1559 tests before rebuild and 1560 tests after rebuild.
- Full dev test run: FAIL
  - `ctest --preset dev -j 2 --output-on-failure`
  - Result: 97% tests passed; 49 tests failed out of 1560.
  - Many Postgres tests skipped because `FMGR_TEST_POSTGRES_URL` was unset.

This review focuses on architecture, maintainability, security/operability, and test reliability. It does not attempt to fix the worktree.

## Executive summary

FreezerManager is unusually mature for a pre-alpha academic-lab system. The strengths are real:

- strong C++20 domain modeling;
- a coherent storage abstraction with SQLite and PostgreSQL implementations;
- serious audit-chain design;
- PHI-aware field encryption work;
- TLS and request-hardening work already underway;
- broad test coverage across unit, integration, property, fuzz-style, e2e, and backend conformance tests.

The main risks are now second-order engineering risks rather than basic missing architecture:

1. Test isolation is currently unreliable under parallel CTest.
2. Integration-test harness logic is duplicated across many files.
3. `freezerd` production startup path is still SQLite-only despite PostgreSQL being the recommended production backend.
4. API pagination contract says cursor-based but some handlers still implement offset pagination.
5. Current documentation/status claims can drift ahead of actually verified behavior.
6. Some production-hardening gaps remain: full TOML config, REST TLS enforcement, real signed audit checkpoints, and stable canonical JSON/JCS semantics.

Recommended priority order:

1. Fix parallel test DB collisions immediately.
2. Centralize integration test harness setup.
3. Add server-side backend factory support for PostgreSQL.
4. Replace offset page tokens with opaque cursor tokens.
5. Make web scaffold either real or clearly absent from committed source.
6. Tighten production config validation and docs around TLS/KMS/REST/health/metrics.

## Codebase composition snapshot

A rough line/file survey excluding `.git`, `out`, `build`, caches, virtualenvs, and `node_modules` produced:

| Extension | Files | Lines |
|---|---:|---:|
| `.cpp` | 83 | 31,101 |
| `.cc` | 104 | 27,478 |
| `.h` | 160 | 12,523 |
| `.md` | 39 | 7,604 |
| `.json` | 11 | 4,545 |
| `.py` | 10 | 1,646 |
| `.proto` | 10 | 1,116 |
| `.sql` | 9 | 479 |

Large/hot files inspected or noted:

| File | Lines | Comment |
|---|---:|---|
| `tests/unit/sample_repository_test.cpp` | 2270 | broad repository coverage, but large enough to become difficult to navigate |
| `tests/unit/cli_test.cpp` | 1964 | broad CLI/backend matrix coverage |
| `tests/unit/local_auth_provider_test.cpp` | 1354 | important security surface |
| `src/storage/sqlite/SqliteBackend.cc` | 1281 | migrations + backend state + transaction logic in one large file |
| `tests/integration/sample_service_integration_test.cpp` | 1211 | duplicated integration harness and seed logic |
| `src/storage/sqlite/SampleRepositories.cc` | 1118 | core domain persistence logic |
| `src/storage/postgres/PostgresBackend.cc` | 1080 | migrations + pool + transaction/audit logic in one large file |
| `src/cli/CliApp.cc` | 1019 | command registration/dispatch is becoming large |
| `src/server/SampleServiceImpl.cc` | 909 | high-value service with PHI, import, watch, and lifecycle logic |
| `src/auth/LocalAuthProvider.cc` | 886 | critical auth/security file |

## What is strong

### 1. Security posture is unusually strong for this project class

Evidence:

- Password hashing uses libsodium Argon2id path in `src/auth/LocalAuthProvider.cc:200-211`.
- Session/API-token hashing uses libsodium BLAKE2b in `src/auth/LocalAuthProvider.cc:674-683`.
- Token hash comparison is constant-time via `sodium_memcmp` in `src/auth/LocalAuthProvider.cc:685-700`.
- gRPC TLS 1.3-only credentials are built with hot-reloaded file watcher provider in `src/server/FreezerServer.cc:63-90`.
- gRPC server refuses plaintext when `require_tls` is set in `src/server/FreezerServer.cc:125-132`.
- Internal error masking is process-wide and default-safe in release in `src/server/GrpcErrorTranslation.h:21-35` and `src/server/FreezerServer.h:67-75`.
- PHI custom fields are separated and encrypted in `src/server/SampleServiceImpl.cc:191-247`.
- PHI disclosure merges decrypted fields only for authorized readers and records key names for audit in `src/server/SampleServiceImpl.cc:250-277`.
- API-token scopes are parsed fail-closed and validate unknown permission keys at creation in `src/auth/LocalAuthProvider.cc:53-104`.

Why this matters:

Most lab inventory systems start as CRUD apps and bolt on security later. FreezerManager has security concepts in the domain, storage, auth, and server layers early enough that the architecture can still hold them.

Suggested follow-up:

Keep this as a differentiator, but avoid marking security features as “done” until they have:

- a production runbook;
- a negative test proving failure is safe;
- operator config validation;
- documentation of residual risk.

### 2. Storage abstraction and backend conformance strategy are solid

Evidence:

- `IStorageBackend` defines portable capabilities, portable error hierarchy, transactions, query DSL, and repository APIs in `src/storage/IStorageBackend.h`.
- Query DSL supports equality, range, `IN`, JSON path equality, sort, limit/offset in `src/storage/IStorageBackend.h:137-248`.
- SQL rendering is dialect-separated in `src/storage/detail/QuerySqlBuilder.h:30-135`.
- SQLite and Postgres both use repository registration and backend conformance tests.
- PostgreSQL tests exist but skip when `FMGR_TEST_POSTGRES_URL` is unset.

Why this matters:

The project is not simply “SQLite code with a Postgres someday plan.” There is a real backend abstraction and a conformance-test culture, which is exactly what this domain needs.

Suggested follow-up:

Consolidate backend opening/registration logic so production server, CLI, tests, and backup tooling do not drift.

### 3. Audit-chain design is serious

Evidence:

- SQLite append-only triggers: `src/storage/sqlite/SqliteBackend.cc:624-637`.
- SQLite metadata/audit schema bootstrapping: `src/storage/sqlite/SqliteBackend.cc:922-968`.
- Postgres append-only triggers: `src/storage/postgres/PostgresBackend.cc:103-120`.
- Postgres audit append obtains an advisory lock before appending: `src/storage/postgres/PostgresBackend.cc:833-846`.
- Postgres tail lookup uses link structure rather than timestamp/id order: `src/storage/postgres/PostgresBackend.cc:837-846`.
- PHI-read audit event records field keys only: `src/storage/postgres/PostgresBackend.cc:803-811`.

Why this matters:

For biospecimen/PHI-sensitive systems, “audit log exists” is not enough. Append-only semantics and hash-chain verification are important. The implementation is directionally correct.

Suggested follow-up:

The outstanding concern is canonicalization. `src/audit/CanonicalJson.cc:13-17` relies on `nlohmann::json::dump()` and map key order. That is deterministic enough for this implementation but is not a complete RFC 8785/JCS implementation.

Before 1.0:

1. Decide whether the guarantee is “FreezerManager internal canonicalization v1” or full RFC 8785/JCS.
2. Add golden vectors to CI.
3. Version the canonicalization scheme in audit checkpoint metadata.
4. Never change canonicalization behavior without a migration/checkpoint strategy.

## Critical findings and instructions

### C1. Parallel CTest currently races shared SQLite database paths

Severity: Critical for developer reliability and CI trust

Observed failure:

`ctest --preset dev -j 2 --output-on-failure` failed 49 tests. Common errors:

- `prepare sqlite identity statement: no such table: labs`
- `prepare sqlite session statement: no such table: sessions`
- `prepare sqlite box geometry statement: no such table: container_types`

Representative failed tests:

- `SampleServiceTest.ImportSamplesDryRunDoesNotPersist`
- `SampleServiceTest.ImportSamplesAsAdminCommits`
- `RestGatewayTest.EndToEndCreateThenGetLab`
- `E2ESmokeTest.FullAuthFlowLoginLogout`
- multiple Box/ItemType/Role/Sample/Share/REST integration tests

Likely root cause:

Several integration fixtures use temp DB names that are unique only inside a single test process. But `gtest_discover_tests()` causes CTest to run individual tests as separate processes. A `static std::atomic<int> counter` resets to 0 in each process. Under `ctest -j 2`, two tests from the same executable can use and delete the same DB path concurrently.

Evidence:

- `tests/integration/sample_service_integration_test.cpp:56-60`
  - returns `fmgr-sample-test-<counter>.db`
- `tests/integration/sample_service_integration_test.cpp:76-84`
  - removes, creates, registers, migrates that DB path
- `tests/integration/server_integration_test.cpp:52-68`
  - same static-counter pattern
- `tests/integration/rest_gateway_integration_test.cpp:97-105`
  - uses fixed path `fmgr-rest-it.db`
- Other integration files show repeated temp DB creation + removal patterns.

Why this matters:

This undermines the entire test suite. A nondeterministic test harness can:

- produce false negatives that waste time;
- hide real failures as “probably flaky”; 
- make CI reruns unreliable;
- corrupt state between tests;
- mask migration/registration defects.

Instructions to fix:

1. Create a shared temp SQLite helper, e.g. `tests/support/TempSqliteDb.h`.
2. Generate DB names using at least:
   - process id;
   - high-resolution timestamp or random UUID;
   - sanitized test suite/test name if available.
3. Remove any use of fixed DB names in tests.
4. Remove any use of static counters as the sole uniqueness source.
5. Ensure cleanup removes all three SQLite files:
   - `<path>`
   - `<path>-wal`
   - `<path>-shm`
6. Run `ctest --preset dev -j 2 --output-on-failure` repeatedly until stable.
7. Add a short comment explaining why PID/randomness is required under `gtest_discover_tests()`.

Suggested helper shape:

```cpp
class TempSqliteDb {
public:
  explicit TempSqliteDb(std::string_view prefix);
  ~TempSqliteDb();

  [[nodiscard]] const std::filesystem::path& path() const;
  void cleanup();

private:
  std::filesystem::path path_;
};
```

Implementation notes:

- Use `core::generate_uuid_v4()` if available in test support.
- Otherwise use `std::random_device` plus PID; for tests this is enough.
- Keep cleanup idempotent and noexcept in destructor.

Acceptance criteria:

- `ctest --preset dev -j 2 --output-on-failure` passes locally.
- Re-running the command 3 times does not reproduce “no such table” errors.
- No integration fixture uses a fixed path under `/tmp` except via the helper.

### C2. Integration test harness logic is duplicated across many files

Severity: High

Evidence:

Repeated blocks appear across:

- `tests/integration/server_integration_test.cpp`
- `tests/integration/lab_service_integration_test.cpp`
- `tests/integration/box_service_integration_test.cpp`
- `tests/integration/item_type_service_integration_test.cpp`
- `tests/integration/sample_service_integration_test.cpp`
- `tests/integration/role_service_integration_test.cpp`
- `tests/integration/audit_service_integration_test.cpp`
- `tests/integration/share_service_integration_test.cpp`
- `tests/integration/rest_gateway_integration_test.cpp`

Common duplication:

- construct `SqliteBackend`;
- register all repositories;
- run `migrate_to_latest()`;
- configure fast Argon2id;
- seed users/labs/roles;
- start `FreezerServer`;
- create gRPC stubs;
- set bearer metadata;
- cleanup DB/WAL/SHM files.

Representative repeated registration block:

- `tests/integration/sample_service_integration_test.cpp:202-214`
- `tests/integration/rest_gateway_integration_test.cpp:162-174`
- `tests/integration/server_integration_test.cpp:143-155`

Why this matters:

The current failing test isolation issue is exactly the kind of bug duplicated harnesses create. Every new repository, migration, test fixture, TLS option, KMS setting, or seed convention must be manually kept in sync across many files.

Instructions to fix:

1. Add shared test-support headers/sources:
   - `tests/support/TempSqliteDb.h`
   - `tests/support/RegisterRepositories.h`
   - `tests/support/IntegrationSeed.h`
   - `tests/support/GrpcServerHarness.h`
   - optionally `tests/support/RestGatewayHarness.h`
2. Move repository registration into one function:
   - `register_all_sqlite_repositories(storage::SqliteBackend&)`
3. Move fast auth config into one function:
   - `fast_auth_config()`
4. Move common seeded identities into one fixture object:
   - admin, member, read-only, outsider, lab1, lab2, password.
5. Convert one integration suite first, prove the pattern, then migrate the rest.
6. Delete local duplicate helpers after migration.

Acceptance criteria:

- New repository registration must require editing exactly one test-support function.
- Integration tests still compile and pass.
- `search_files` for `static void register_all_repositories` under `tests/integration` should return zero or near-zero results.

### C3. `freezerd` startup path is SQLite-only despite Postgres being production-recommended

Severity: High before production packaging

Evidence:

README positions PostgreSQL as production-recommended:

- `README.md:30-32`
- `README.md:87-88`

But server main only reads `FMGR_DB_PATH` and constructs `SqliteBackend`:

- `src/server/main.cc:146-163`
- repository registration is SQLite-specific at `src/server/main.cc:162-178`

The CLI already has a backend factory that supports exactly one of SQLite or Postgres:

- `src/cli/BackendFactory.cc:73-104`

Why this matters:

Production users will start `freezerd`, not a CLI helper. If production deployment is supposed to use Postgres, the server entry point must support Postgres natively and validate configuration fail-fast.

Instructions to fix:

1. Extract `BackendFactory` out of `src/cli` into a shared library location, e.g.:
   - `src/storage/BackendFactory.h/.cc`, or
   - `src/server/BackendFactory.h/.cc` if it remains server-specific.
2. Support server env vars such as:
   - `FMGR_SQLITE_PATH`
   - `FMGR_POSTGRES_URL`
   - `FMGR_PG_PASSWORD`
3. Enforce exactly one backend:
   - SQLite path set XOR Postgres URL set.
4. Keep password out of logs.
5. Register all repositories through one shared helper.
6. Update README and deployment docs with production examples.
7. Add integration tests for server startup with SQLite and, when `FMGR_TEST_POSTGRES_URL` is set, with Postgres.

Acceptance criteria:

- `freezerd` can boot against Postgres using an env-provided connection string.
- `freezerd` fails clearly if both SQLite and Postgres are configured.
- `freezerd` fails clearly if neither backend is configured in production mode.
- CLI and server use the same repository-registration list.

### C4. API pagination contract says cursor-based, but implementation uses offset tokens

Severity: High for API stability before external clients

Evidence:

Proto contract:

- `proto/fmgr/v1/common/types.proto:14-17`
  - says `PageRequest` is “Opaque cursor-based pagination.”

Storage warning:

- `src/storage/IStorageBackend.h:240-243`
  - says RPC handlers MUST use cursor-based pagination and not OFFSET scans.

Actual implementation:

- `src/server/SampleServiceImpl.cc:470-479`
  - parses `page_token` as `std::stoull()` offset.
- `src/server/SampleServiceImpl.cc:496-499`
  - returns next token as `offset + samples.size()`.

Why this matters:

Offset pagination is unstable under concurrent inserts/deletes and expensive on large tables. If Python/web clients adopt integer page tokens, changing later becomes a breaking API behavior.

Instructions to fix:

1. Define an opaque token encoding.
   - Recommended: base64url JSON with version, sort keys, and direction.
2. For samples, use a stable tuple cursor:
   - `(last_modified_at_micros, id)` for mutation-ordered lists; or
   - `(created_at_micros, id)` for creation-ordered lists.
3. For audit events, use:
   - `(at_micros, id)`.
4. Add query DSL support for lexicographic tuple cursor or add service-specific repository methods.
5. Reject malformed page tokens as `INVALID_ARGUMENT`, not `INTERNAL`.
6. Document token opacity: clients must not parse it.
7. Add concurrency tests:
   - page 1, insert row, page 2 should not duplicate or skip unexpectedly for the selected ordering.

Acceptance criteria:

- No RPC handler parses `page_token` as a numeric offset.
- `proto/fmgr/v1/common/types.proto` accurately describes implemented behavior.
- Tests cover malformed token and stable pagination under insertion.

### C5. Web source tree is inconsistent: lockfile exists, package manifest absent, node_modules present locally

Severity: Medium

Evidence:

- `src/web/package-lock.json` exists and is large.
- `src/web/package.json` is absent.
- `src/web/CMakeLists.txt` contains only: `# React/TypeScript web application placeholder.`
- `src/web/node_modules/` exists locally but is ignored by `.gitignore`.
- `.gitignore:24` ignores root `package-lock.json`, but not `src/web/package-lock.json`.

Why this matters:

This creates ambiguity for contributors:

- Is the web client started or not?
- Should `package-lock.json` be committed?
- Which npm command should be run?
- Why is there a lockfile without a manifest?

Instructions to fix:

Choose one of two paths.

Path A — web is not started yet:

1. Remove `src/web/package-lock.json` from tracked source.
2. Keep `src/web/CMakeLists.txt` as placeholder.
3. Add `src/web/package-lock.json` to `.gitignore` until the manifest exists.
4. README should say web UI is planned, no source scaffold yet.

Path B — web scaffold is started:

1. Commit a minimal `src/web/package.json`.
2. Commit `src/web/package-lock.json` generated from that manifest.
3. Add scripts:
   - `npm run dev`
   - `npm run build`
   - `npm run test` or explicitly omit tests for now.
4. Update `src/web/CMakeLists.txt` or docs to explain build integration.
5. Add at least a smoke build CI job.

Acceptance criteria:

- `npm ci` in `src/web` either works or web is clearly absent.
- No committed lockfile exists without a manifest.

### C6. REST TLS / production plaintext guard is weaker than gRPC guard

Severity: Medium

Evidence:

- gRPC has `require_tls` guard in `src/server/FreezerServer.cc:125-132`.
- REST listener config in `src/server/main.cc:247-259` enables TLS if cert/key are both present, otherwise starts plaintext.
- Comments say health/metrics should be bound behind reverse-proxy ACL or localhost in production: `src/server/main.cc:230-245`.

Why this matters:

If REST exposes auth endpoints or session-bearing calls, plaintext REST is just as risky as plaintext gRPC. The operator may set `FMGR_REQUIRE_TLS=1` expecting all public endpoints to be protected, but the current guard only visibly protects gRPC.

Instructions to fix:

1. Define semantics of `FMGR_REQUIRE_TLS`:
   - all external listeners must be TLS; or
   - only gRPC must be TLS, with REST expected behind reverse proxy.
2. If all external listeners:
   - require `FMGR_REST_TLS_CERT` and `FMGR_REST_TLS_KEY` when REST binds non-loopback and `FMGR_REQUIRE_TLS=1`.
3. If reverse-proxy model:
   - add `FMGR_REST_TRUST_PROXY=true` or equivalent;
   - default REST bind to `127.0.0.1:8080` in production examples;
   - document reverse-proxy TLS termination explicitly.
4. Add startup validation tests.

Acceptance criteria:

- A production config cannot accidentally expose REST bearer tokens over plaintext on `0.0.0.0`.
- Docs show secure REST deployment examples.

### C7. Health and metrics are unauthenticated and may leak operational details

Severity: Medium

Evidence:

- Health route is unauthenticated by design: `src/server/main.cc:230-241`.
- Metrics route is unauthenticated by design: `src/server/main.cc:243-245`.
- `probe_kms()` returns `key_id=` details at `src/server/main.cc:73-89`.

Why this matters:

Unauthenticated health/metrics is often acceptable on loopback/private networks, but unsafe if exposed. Details like backup target writability, KMS status, and key IDs can help attackers map the system.

Instructions to fix:

1. Add config options:
   - `FMGR_HEALTH_PUBLIC=true|false`
   - `FMGR_METRICS_PUBLIC=true|false`
   - or bind health/metrics to a separate listener/address.
2. Default production docs to loopback-only or reverse-proxy ACL.
3. Consider a shallow unauthenticated liveness endpoint and an authenticated readiness endpoint.
4. Avoid returning full KMS key IDs unless authenticated/admin.

Acceptance criteria:

- Secure deployment docs do not expose health/metrics publicly by default.
- Tests cover public/private behavior.

### C8. Migration definitions are embedded in backend `.cc` files and are growing large

Severity: Medium

Evidence:

- SQLite migrations are embedded in `src/storage/sqlite/SqliteBackend.cc`, which is 1281 lines.
- Postgres migrations are embedded in `src/storage/postgres/PostgresBackend.cc`, which is 1080 lines.
- There are also `.sql` files under `src/storage/sqlite/migrations/`, but current knowledge indicates migrations 0010-0015 are embedded in C++.

Why this matters:

Embedding all migration SQL in large C++ files makes review harder and increases merge-conflict risk. It also makes it harder for operators to inspect schema changes.

Instructions to improve:

1. Keep checksums and migration ordering in C++ if desired, but move SQL bodies to files:
   - `src/storage/sqlite/migrations/0015_login_attempt.sql`
   - `src/storage/postgres/migrations/0015_login_attempt.sql`
2. Embed them at build time or read them as resources in tests/dev.
3. Add a test that validates migration files are strictly increasing and checksums match.
4. Document how to add a migration.

Acceptance criteria:

- Adding a migration does not require editing a 1000+ line backend file.
- SQL review diffs are readable.

### C9. Global/static RPC permission registry is convenient but fragile

Severity: Medium

Evidence:

- Registry is global static map in `src/rpc/AuthMiddleware.cc:17-25`.
- Services register RPC permissions in constructors, e.g. `src/server/SampleServiceImpl.cc:418-433`, `src/server/AuthServiceImpl.cc:78-94`.

Why this matters:

Constructor-time registration can create ordering surprises in tests and does not guarantee every proto RPC is registered. The registry is useful for tests, but should be generated or asserted against service descriptors.

Instructions to improve:

1. Add a test that enumerates all methods from generated gRPC service descriptors and compares against `AuthMiddleware::registered_rpcs()`.
2. Explicitly model self-service/auth-public RPCs instead of assigning placeholder permissions like `SessionRevoke` to login in `src/server/AuthServiceImpl.cc:82-93`.
3. Consider replacing constructor registration with a single static permission table.

Acceptance criteria:

- A newly added RPC fails tests unless it has an explicit auth policy.
- Login/public/self-service RPCs have clear policy types, not misleading required permissions.

### C10. Build passes but warnings should be treated as cleanup backlog

Severity: Low/Medium

Observed build warnings:

- Ignored `[[nodiscard]]` return in `tests/unit/rate_limiter_test.cpp` around lines 183, 227, 373, 412.
- Ignored `nlohmann::json::parse()` result in `tests/unit/error_translation_test.cpp:203`.
- Vexing parse warning in `tests/unit/qt_sample_table_model_test.cpp:240`.
- Ignored `Uuid::parse()` result in `src/core/custom_field_validator.h:134` when included from fuzz tests.

Why this matters:

Warnings in tests normalize warning noise. Later, real warnings may be missed.

Instructions to fix:

1. For ignored nodiscard results, assign to `[[maybe_unused]] const bool acquired = ...;` or assert result when meaningful.
2. For parse/UUID validation calls, assign to `[[maybe_unused]] const auto parsed = ...;`.
3. Fix the Qt vexing parse with brace initialization or create a genuinely valid parent index.
4. Consider enabling `-Werror` in CI for project code and eventually tests.

Acceptance criteria:

- `cmake --build --preset dev -j 2` emits no project/test warnings.

## Additional design suggestions

### 1. Centralize repository registration for all backends

Current issue:

Repository registration is repeated in server main, CLI backend factory, and tests. Example server main registration: `src/server/main.cc:162-178`.

Recommendation:

Create:

```cpp
namespace fmgr::storage {
  void register_all_repositories(SqliteBackend&);
  void register_all_repositories(PostgresBackend&);
}
```

Then use it everywhere:

- CLI backend factory;
- server main;
- integration tests;
- backup tooling;
- seed scripts if relevant.

Benefit:

A new repository cannot be accidentally missed in one execution path.

### 2. Split large service/backend files by responsibility

Candidates:

- `src/server/SampleServiceImpl.cc` combines marshalling, PHI encryption/reveal, import, watch streaming, and CRUD/lifecycle RPCs.
- `src/storage/sqlite/SqliteBackend.cc` combines connection handling, migrations, audit helpers, metadata tables, transaction implementation.
- `src/storage/postgres/PostgresBackend.cc` combines migrations, pool, transaction, audit append, migration engine.
- `src/cli/CliApp.cc` combines many command groups and dispatch logic.

Recommendation:

Do not refactor all at once. Refactor when touching a slice:

- `SampleServiceMapping.cc` for proto/core conversion;
- `SamplePhi.cc` for PHI prepare/reveal;
- `SampleImportRpc.cc` for import RPC helpers;
- `SqliteMigrations.cc` / `PostgresMigrations.cc` or external SQL files;
- `AuditAppend.cc` per backend;
- `CliBackupCommands.cc`, `CliSampleCommands.cc`, etc.

Acceptance criteria:

- New code lands in smaller responsibility-specific files.
- Existing large files shrink gradually without drive-by churn.

### 3. Make configuration first-class before production packaging

Current state:

`src/server/main.cc:146-148` says full TOML configuration is deferred. Production options are scattered across env vars.

Recommendation:

Introduce `/etc/freezerd/freezerd.toml` or `FMGR_CONFIG` before packaging.

Minimum config domains:

- database backend;
- gRPC listen/TLS/mTLS;
- REST listen/TLS/proxy assumptions;
- KMS provider;
- backup schedule/retention;
- health/metrics exposure;
- logging level;
- rate limits;
- max message size.

Acceptance criteria:

- Config is validated at startup.
- Invalid production config fails before binding any public port.
- Env overrides are documented and tested.

### 4. Improve docs/status discipline

Examples:

- README says 1080+ tests, but current discovery shows 1560 tests.
- README presents web/Python as next/planned; `src/web` has only placeholder CMake plus a lockfile without manifest.
- PostgreSQL is described as production-recommended, but server main is SQLite-only.

Recommendation:

Add a status-generation or release-check habit:

1. Before updating README badges/status, run:
   - `ctest --preset dev -N`
   - `ctest --preset dev -j 2`
2. Keep “implemented” distinct from:
   - implemented in library/backend;
   - wired into production server;
   - documented for operators;
   - covered by integration tests.
3. Consider a `doc/IMPLEMENTATION_STATUS.md` generated or manually updated per milestone.

Acceptance criteria:

- README does not overstate features that exist in libraries but are not reachable through `freezerd`.

## Suggested implementation plan

### Phase 1 — Stabilize tests

Goal: make `ctest --preset dev -j 2` trustworthy.

Tasks:

1. Add `tests/support/TempSqliteDb.h`.
2. Replace static-counter/fixed DB paths in integration and e2e tests.
3. Centralize DB cleanup.
4. Run:
   ```sh
   cmake --build --preset dev -j 2
   ctest --preset dev -j 2 --output-on-failure
   ctest --preset dev -j 2 --output-on-failure
   ctest --preset dev -j 2 --output-on-failure
   ```
5. Fix any remaining order-dependent tests.

Expected result:

- No `no such table` failures.
- No test-process DB collisions.

### Phase 2 — Consolidate test harness

Goal: reduce duplicated setup and prevent future drift.

Tasks:

1. Add `tests/support/RegisterRepositories.h`.
2. Add `tests/support/IntegrationHarness.h`.
3. Convert one service suite.
4. Convert remaining service suites.
5. Delete duplicated `register_all_repositories()` helpers.

Expected result:

- Adding a new repository requires one test-support edit, not 8+ edits.

### Phase 3 — Production backend/config path

Goal: make `freezerd` match the project’s production story.

Tasks:

1. Extract shared backend factory from CLI.
2. Add Postgres env/config support to server.
3. Add exactly-one backend validation.
4. Add server startup tests for config combinations.
5. Update README and docs.

Expected result:

- `freezerd` can boot with SQLite or Postgres.
- Postgres production deployment is no longer only a library/CLI capability.

### Phase 4 — API pagination correction

Goal: lock down stable API behavior before external clients.

Tasks:

1. Define opaque cursor token format.
2. Implement decode/encode helpers.
3. Replace offset tokens in `ListSamples` and other list RPCs.
4. Add malformed-token tests.
5. Add concurrent insert/delete pagination tests.
6. Update proto comments.

Expected result:

- API behavior matches proto contract.
- Clients can safely treat tokens as opaque.

### Phase 5 — Production hardening polish

Goal: reduce operational surprises.

Tasks:

1. Clarify REST TLS/reverse-proxy model.
2. Add health/metrics exposure config.
3. Clean build warnings.
4. Move migrations toward separate SQL files or at least separate migration source files.
5. Add canonical JSON golden vectors.
6. Decide and document audit canonicalization versioning.

## Remediation status — 2026-07-03

Follow-up implementation addressed the highest-impact engineering failures, but
this review is **not fully closed**. Treat the list below as the current handoff.

### Fixed in the first remediation slice

- Parallel CTest SQLite database collisions: integration/e2e tests now use a
  PID+UUID temp DB helper instead of static counters or fixed `/tmp` paths.
- Repeated integration/e2e fast-auth config and SQLite repository registration
  were centralized in `tests/support/` helpers.
- Backend opening/registration moved to a shared storage backend factory used by
  both CLI and `freezerd`.
- `freezerd` now supports backend selection through `FMGR_SQLITE_PATH` / legacy
  `FMGR_DB_PATH` or `FMGR_POSTGRES_URL`, with fail-fast checks for conflicting
  config and no implicit `:memory:` backend in `FMGR_REQUIRE_TLS=1` mode.
- `SampleService.ListSamples` no longer uses plain integer offset page tokens;
  it now emits/accepts opaque versioned sample cursor tokens.
- Ambiguous `src/web/package-lock.json` placeholder state was removed/ignored;
  the web tree remains a placeholder until a real scaffold is added.
- Known build warnings called out in the review were cleaned.

Verification run after the slice:

- `cmake --build --preset dev -j 2` — PASS.
- Focused changed-behavior CTest via `/tmp/hermes-verify-*` script — PASS
  (153/153 non-skipped tests; Postgres variants skipped without
  `FMGR_TEST_POSTGRES_URL`).
- Full `ctest --preset dev -j 2 --output-on-failure` was also run in the same
  workspace and passed 1562/1562 non-skipped tests, with Postgres tests skipped
  because `FMGR_TEST_POSTGRES_URL` was not configured.
- `git diff --check` — PASS.

### Fixed in the second remediation slice (2026-07-03)

- **REST TLS exposure policy (C6):** `fmgr::server::validate_rest_tls_policy`
  fails fast — under `FMGR_REQUIRE_TLS` a non-loopback REST bind must present
  `FMGR_REST_TLS_CERT`/`_KEY` or be bound to loopback; a half-configured cert/key
  pair is rejected in any mode. Unit-tested.
- **Health/metrics exposure controls (C7):** `probe_kms` no longer emits the KEK
  key id into the `/health` body; a shallow always-public liveness (`/healthz`,
  `/livez`) is split from the detailed readiness report; `FMGR_HEALTH_PUBLIC` /
  `FMGR_METRICS_PUBLIC` gate the detailed endpoints (metrics private by default in
  production), requiring a valid bearer otherwise.
- **Explicit RPC auth policy model (C9):** `rpc::RpcPolicy` (Public /
  SelfService / LabPermission / GlobalPermission) replaces the bare-permission
  registry and the `SessionRevoke` placeholders on auth/session endpoints; a
  descriptor-enumeration test asserts every gRPC method has a policy.
- **Migration maintainability (C8):** migration SQL moved to
  `src/storage/{sqlite,postgres}/migrations/*.sql`, embedded at build time; a
  golden-checksum test proves no checksum changed. See `doc/MIGRATIONS.md`.
- **Audit canonicalization:** `canonical_json` now implements RFC 8785 (JCS) with
  a scheme-version constant, golden vectors, and a single shared content builder;
  existing chains verify unchanged.
- **Docs/status refresh:** README test count, migration ranges, RPC policy, and
  REST TLS / health-metrics behavior updated.

### Still open / not fully fixed

- **PostgreSQL live verification:** Postgres-specific tests were not run because
  `FMGR_TEST_POSTGRES_URL` was unset. The server Postgres selection path and the
  Postgres migration split are code-complete but still need live verification
  against a real `postgres:16` before claiming production-validated.

## Bottom-line verdict

FreezerManager has a strong foundation. The core architecture is not the weak point. The urgent work is to stabilize the engineering system around it: deterministic tests, shared harnesses, production server configuration, and API contract consistency.

If I were preparing this for the next milestone, I would not add major user-facing features first. I would spend the next slice on:

1. fixing CTest DB isolation;
2. centralizing integration harness setup;
3. making `freezerd` support Postgres through shared backend creation;
4. replacing offset pagination before clients depend on it.

Those changes will pay down systemic risk and make later M4/M5 work much safer.
