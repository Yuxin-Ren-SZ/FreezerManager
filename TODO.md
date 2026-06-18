# TODO — Implementation Backlog

## Handoff note — 2026-06-17, M5 slice 2 (production KMS + key rotation)

Hardens the PHI key story from dev-only to production-ready and adds master-KEK
rotation (PRD §8). Builds on slice 1. Trigger is the CLI (the new KEK is staged
in the server's credential store, never sent over an RPC); a `RotateKeys` RPC can
wrap the same engine later.

**Keyring + key sources (`src/kms/`):**
- `IKmsProvider::unwrap_dek` now takes the envelope's `kek_id` so a provider can
  hold several KEKs. `KeyringKms` is the shared engine: a `map<kek_id, key>` with
  one active KEK; `wrap_dek` seals under the active KEK, `unwrap_dek(w, kek_id)`
  selects by id (throws on unknown id). Centralizes the `crypto_secretbox`
  wrap/unwrap + BLAKE2b fingerprint that used to live in `EnvVarKms`.
- `EnvVarKms` is now a `KeyringKms` loader: `FMGR_MASTER_KEK` (active) +
  optional `FMGR_MASTER_KEK_PREVIOUS` (comma-separated base64 retired keys).
- `OsKeyringKms` (production): reads `$CREDENTIALS_DIRECTORY/master_kek` (active)
  and `master_kek.prev.*` (retired) — systemd-creds `LoadCredential=`. Each file
  is raw 32 bytes or base64. Pure file I/O + libsodium, no new dependency.
- `make_default_kms()` (`KmsFactory`): OS keyring if `$CREDENTIALS_DIRECTORY/
  master_kek` exists, else `EnvVarKms`, else null (PHI disabled). `FreezerServer`
  and the CLI both use it.

**Rotation:**
- `crypto::rewrap(envelope, kms)` re-wraps the per-record DEK under the active
  KEK and rewrites `kek_id`; field ciphertext is copied verbatim (DEK unchanged),
  so no plaintext PHI is decrypted to disk. Returns nullopt for an empty envelope
  or one already on the active KEK.
- `cli::rotate_phi_keys(backend, kms, lab?, actor, sink)` walks samples
  (incl. tombstoned — PHI persists) per lab, rewraps, and updates via the Sample
  repo. Returns `{scanned, rewrapped, current, failed}`. A sample that fails to
  re-wrap (e.g. update FK re-validation) is counted in `failed` and logged, not
  aborted — so the old KEK is retired only once `failed == 0`.
- `freezerctl key rotate [--sqlite|--postgres] [--lab <uuid>] --actor <uuid>`.

**Operator runbook (rotate the master KEK):**
1. Move the current KEK to `FMGR_MASTER_KEK_PREVIOUS` (or a `master_kek.prev.*`
   credential file).
2. Install the new KEK as `FMGR_MASTER_KEK` / `master_kek`.
3. Run `freezerctl key rotate --actor <uuid>` until it reports `failed 0` and
   `rewrapped + current == scanned`.
4. Drop the retired KEK once every record is migrated.

**Tests:** `kms_test` (keyring: retired-key unwrap, unknown-id throws, env
previous keys), `os_keyring_test` (base64/raw files, retired keys, missing
dir/file fail-fast), `field_cipher_test` (rewrap rotates + still decrypts, no-op
when current/empty), `cli_test` (rotate rewraps to active KEK + audits + second
rotate no-op; argv `key rotate` via env). SQLite + Postgres-param where relevant.

**Known limitations / follow-ups:**
- Rotation audits as a Sample `update` (after-image shows the new `kek_id`,
  ciphertext only). A distinct `key.rotate` audit *action* would need a custom
  action threaded through the repository update path (the `MutationContext.reason`
  is currently not persisted) — deferred. The `key.rotate` permission and the
  online `RotateKeys` RPC are also deferred.
- Rotation scope is the Sample entity (the only PHI column).
- `VaultKms` (transit engine) not implemented.

Verification: `cmake --build --preset dev` clean; `ctest --preset dev -j1` — new
kms/crypto/os-keyring/cli-rotation tests green; only the 12 pre-existing baseline
failures (SqliteBackend file-detection, CustomFieldResolver, E2E unauth) plus the
known-flaky `FuzzRateLimiter` under parallel load (passes in isolation).
`run-clang-tidy-17` on new/changed sources clean; `clang-format-17` clean;
`tools/check-spdx-headers.sh` clean; `git diff --check` clean.

## Handoff note — 2026-06-17, M5 slice 1 (PHI field-level encryption + KMS + PHI-read audit)

First PHI-safety slice (PRD §8). PHI custom fields stop sitting in plaintext: they
are split out of the sample's plaintext blob, AEAD-encrypted with a per-record DEK
wrapped by a master KEK, and disclosed on read only to `phi.read` holders — every
disclosure audited. Scope: Sample entity only.

**New modules:**
- `src/kms/IKmsProvider.h` — envelope KMS interface: `wrap_dek`/`unwrap_dek`
  (`WrappedDek{nonce,ciphertext}`) + `key_id()`. Domain-free, I/O-free.
- `src/kms/EnvVarKms.{h,cc}` — dev/test provider; KEK from `FMGR_MASTER_KEK`
  (base64, 32 bytes), `crypto_secretbox` DEK wrapping, fail-fast on missing/short
  key. `key_id()` = BLAKE2b fingerprint. **NOT for production** (OsKeyring/Vault
  later). `tests/unit/kms_test.cpp` (9): round-trip, fresh nonce, tamper/wrong-key
  rejected, bad/missing key fail-fast.
- `src/crypto/FieldCipher.{h,cc}` — `encrypt(PhiFields, kms)`/`decrypt(...)`.
  Fresh per-record DEK, per-field `crypto_secretbox`. Envelope JSON
  `{v,kek_id,dek:{n,c},fields:{key:{n,c}}}`; empty map → `"{}"`.
  `tests/unit/field_cipher_test.cpp` (9): round-trip, mixed types, plaintext never
  in envelope, fresh-DEK divergence, tamper/wrong-KEK/malformed rejected.

**Audit seam:**
- `ITransaction::note_phi_read(entity_kind, entity_id, ctx, field_keys)` — new
  virtual, default throws `UnsupportedOperation`. Sqlite/Postgres transactions
  override it to append an immutable `action="phi.read"` row (joins the same hash
  chain) with `after_json={"phi_keys":[...]}` — **key names only, never values**
  (PRD §7.3, §17). Delegates to `note_mutation`.

**Service wiring:**
- `SampleServiceImpl` takes a borrowed `kms::IKmsProvider*` (nullable). Write
  (`Create`/`Update`): `prepare_custom_fields` resolves defs, validates the combined
  blob, splits by `is_phi` — non-PHI stays plaintext, PHI encrypted into
  `phi_fields_enc_json`. PHI write requires `Lab.is_phi_enabled` (else
  `INVALID_ARGUMENT`) + a configured KMS (else `INTERNAL`); does **not** require
  `phi.read`. Read (`Get`/`List`): `reveal_phi` decrypts + merges into
  `custom_fields_json` only when the caller holds `phi.read`, then `note_phi_read`
  per disclosing sample. `validate_sample_custom_fields` removed (folded into
  `prepare_custom_fields`).
- `FreezerServer` builds `EnvVarKms` from env at construction; null (PHI disabled,
  warn-logged) when `FMGR_MASTER_KEK` unset. `kms_` declared before `sample_svc_`.
- `ItemTypeServiceImpl` rejects `is_phi ∧ indexed` on CFD create/update (PRD §4.1).
- `tests/integration/sample_service_integration_test.cpp` (+5): ciphertext at rest,
  value absent from both columns, visible to phi.read holder, hidden from non-holder,
  write without phi.read, PHI-read audit row keys-only. Fixture now sets
  `FMGR_MASTER_KEK`, enables PHI on the labs, grants `phi.read` to SystemAdmin
  (excluded by default per PRD §3), and seeds a non-required `is_phi` CFD.
  `item_type_service_integration_test.cpp` (+1): indexed-PHI rejected.

**Known limitations / follow-ups:**
- PHI on Sample only; other entities have no PHI column yet.
- List discloses one `phi.read` audit row per sample (precise but verbose for large
  lists); per-request batching is a later option.
- `EnvVarKms` only; OsKeyringKms / VaultKms and `key.rotate` rotation deferred.
  `kek_id` is recorded in the envelope for forward rotation support.
- Decrypted PHI is **not** echoed in the Create/Update response (only on Get/List
  for phi.read holders).

Verification: `cmake --build --preset dev` clean; `ctest --preset dev -j1` — new
kms/crypto/sample-PHI/item-type tests green; only the 12 pre-existing baseline
failures remain (SqliteBackend file-detection, CustomFieldResolver, E2E unauth).
`run-clang-tidy-17` on new/changed sources clean; `clang-format-17` clean;
`tools/check-spdx-headers.sh` clean; `git diff --check` clean. Postgres
`note_phi_read` override compiles but is untested without `FMGR_TEST_POSTGRES_URL`.

## Handoff note — 2026-06-15, M3 SSE streaming slice 1 (WatchAuditFeed → SSE)

First server-streaming RPC + reusable SSE bridge, proving the pattern end-to-end.

**Changed/new:**
- `proto/fmgr/v1/audit.proto` — `rpc WatchAuditFeed(WatchAuditFeedRequest) returns
  (stream AuditEvent)` + request message (lab/entity filters + `since` cursor).
- `src/server/AuditServiceImpl.{h,cc}` — poll-tail `ServerWriter` handler: authorize
  once at stream-open (same gating as `ListAuditEvents`), then re-query
  `at >= cursor` every ~1s, dedup same-microsecond ids, write each as proto, exit
  on `ServerContext::IsCancelled()`. Registers the RPC (AuditRead). Helpers
  `build_watch_query` / `emit_new_events` keep cognitive complexity under budget.
- `src/rest/SseBridge.h` (new) — generic `stream_sse<RespT>()`: drives the gRPC
  `ClientReader` on a worker thread, posts each frame to the connection's event
  loop via `queueInLoop` (trantor `AsyncStream::send` is loop-thread-only), 15s
  keepalive comments, maps a non-OK `Finish()` to an `event: error` frame. The
  loop-side keepalive `TryCancel`s a parked Read when the client disconnects.
- `src/rest/RestGateway.cc` — `GET /api/v1/audit/watch` route; `since` resume via
  `Last-Event-ID`/`?since=`; emits `id:`+`data:` SSE frames.
- Tests: `audit_service_integration_test.cpp` (+5: live-tail receives event,
  member/outsider/no-bearer denied, `ListSince` range regression);
  `rest_gateway_integration_test.cpp` (+2: raw-socket SSE positive + no-bearer
  error frame).

**Bug fixed along the way:** the SQLite **and** Postgres audit repositories
rendered *every* predicate as `column = ?`, so `since`/`until` (range predicates)
silently matched nothing — `ListAuditEvents` since/until were broken and untested.
Now render `=`/`>=`/`<=` by operator and throw on unsupported ops.

**Known limitations / follow-ups:**
- Polling (~1s), not LISTEN/NOTIFY — portable across SQLite/Postgres; push is a
  later optimization (`caps().listen_notify` still unused).
- SSE auth failures surface as an `event: error` frame (HTTP 200 already
  committed), not an HTTP 401.
- TSan does **not** cover the bridge: the streaming tests are `grpc_integration`-
  labelled and excluded from asan/tsan (Conan gRPC/absl are uninstrumented). The
  bridge's safety rests on the queueInLoop serialization design + review.
- Next on this bridge: `WatchSampleList` (delta add/update/remove), bulk-import
  progress, periodic mid-stream re-authorization.

Verification: `cmake --build --preset dev` clean; `ctest --preset dev -j1` — only
the 12 pre-existing baseline failures (SqliteBackend file-detection,
CustomFieldResolver, E2E unauth); new streaming/SSE tests green;
`run-clang-tidy-17 -p out/build/dev src/server src/rest src/storage/...` clean;
`clang-format` clean.

## Handoff note — 2026-06-15, M3 REST gateway fan-out (Box/ItemType/Role/Audit/Share)

Completed the REST/JSON gateway fan-out: the Drogon front door now fronts **all
9 gRPC services** (was Auth/Session/Lab/Sample). Pure mechanical reuse of the
existing `forward()` macro and the generic `JsonProtoMapping`/`RestErrorTranslation`
helpers — no new abstractions, no CMake change (all stubs already live in the
single `FreezerManager::proto` target).

**Changed:**
- `src/rest/GatewayStubs.h` — add Box/ItemType/Role/Audit/Share stubs + includes.
- `src/rest/RestGateway.cc` — `FMGR_ROUTE(...)` for the remaining ~45 unary RPCs.
  Paths are kebab-case under `/api/v1/*` (e.g. `/api/v1/freezer/list`,
  `/api/v1/storage-container/create`, `/api/v1/custom-field-def/create`,
  `/api/v1/role/permissions/grant`, `/api/v1/audit/export`, `/api/v1/share/approve`).

**Tests (`tests/integration/rest_gateway_integration_test.cpp`):** per-service
positive (admin holds the perm → 200), negative (member lacks it → 403), and
missing-bearer (→ 401), plus an E2E `item-type/create` write through the gateway.
28/28 `RestGatewayTest` pass. Note: some write handlers validate the request body
*before* the auth gate (→ 400), and `ShareService.ApproveShareRequest` runs a
custom role gate after loading the request — so missing-bearer checks target the
lenient `list` endpoints, and share `share.approve` authz stays covered by
`share_service_integration_test.cpp` at the gRPC layer.

**Deferred:** SSE/WebSocket streaming (needs streaming RPCs added to proto first),
Qt 6 client, React SPA, `freezerctl-py`.

Verification: `cmake --build --preset dev` clean; `ctest --preset dev -R
RestGatewayTest` 28/28 pass; `run-clang-tidy-17 -p out/build/dev src/rest/` clean;
`clang-format` clean. (12 pre-existing env failures unrelated to this change:
SqliteBackend file-detection, CustomFieldResolver, E2E unauth — also red on the
`dev` baseline.)

## Handoff note — 2026-06-13, M1 CLI read nouns (freezer/box/item-type list + inspect)

Added the read-only half of the outstanding M1 CLI nouns: `freezerctl freezer|box|
item-type list` and `... inspect --id <uuid>` (PRD §19 M1). Every read is
lab-scoped and Postgres-RLS-gated, reusing the `sample list` query/format pattern.

**New files:**
- `src/cli/EntityRead.h` — header-only, templated `query_in_lab<T>()` and
  `find_in_lab<T>()`, factored from `SampleQuery.cc`. Both inject the
  `current_lab_ids` RLS session var (no-op on SQLite). `find_in_lab` returns
  nullopt when the row is absent **or** belongs to another lab — defense-in-depth
  against cross-lab disclosure via a guessed id, complementing RLS.
- `src/cli/NounCommands.{h,cc}` — `run_{freezer,box,item_type}_list` (tab table +
  `N x(s)` footer) and `run_{freezer,box,item_type}_inspect` (FIELD<TAB>VALUE
  detail; returns 1 + "not found" when absent/foreign). Output to an injected
  `std::ostream` (no stdout), so unit-testable.

**Changed:**
- `src/cli/CliApp.cc` — `freezer`/`box`/`item-type` subcommands, each with `list`
  (shared `--sqlite`/`--postgres`/`--lab`/`--limit`/`--include-tombstoned`) and
  `inspect` (`--lab` + required `--id`). Dispatch extracted into
  `dispatch_read_noun`/`dispatch_read_nouns` helpers to stay under the run_cli
  cognitive-complexity budget.
- `src/cli/CMakeLists.txt` — adds `NounCommands.cc`.

**Tests (`tests/unit/cli_test.cpp`):** the `CliFixture` now also seeds a
container-type, a root storage container + freezer per lab, a box-type, and a box
in lab A. New SQLite+Postgres-parameterized cases: per-noun list (incl. lab-scope
exclusion of lab B's freezer), box list tombstone include/exclude, freezer inspect
detail, inspect of an unknown id, and inspect of another lab's id (both
not-found). Plus argv-level `freezer list`, `box inspect`, and `item-type inspect`
through `run_cli`.

Verification: `cmake --build --preset dev` clean; `ctest --preset dev -j1` —
918/918 pass (Postgres cli/conformance skip without `FMGR_TEST_POSTGRES_URL`);
`clang-format --dry-run --Werror` + `run-clang-tidy-17` on new/changed sources
clean; `tools/check-spdx-headers.sh` + `git diff --check` clean.

Handoff notes:
- Read-only slice. The `create` nouns (write + audit `MutationContext`) are the
  next slice and need a lab to exist — pair with `lab create` / D1.3 first-run.
- `storage-container` and `container-type` read nouns drop in with the same
  `EntityRead.h` helpers when wanted.
- CSV import beyond Sample (boxes, item types, custom-field defs, users) remains a
  separate follow-up reusing `CsvReader` + the `build_import`/report pattern.

## Handoff note — 2026-06-13, M1 sample CSV import (transactional + dry-run)

Implemented `freezerctl sample import`, closing the CSV-import half of M1 for the
Sample entity (PRD §13). Export already existed; this adds the read path.

**New files:**
- `src/cli/CsvReader.{h,cc}` — RFC 4180 reader (the counterpart to `CsvWriter`).
  Honours double-quoted fields, doubled embedded quotes, embedded commas/CR/LF,
  CRLF or bare-LF separators; skips `#`-prefixed comment lines so a file produced
  by `sample export` (chain-of-custody header block) round-trips unchanged. No
  spurious trailing empty record. `CsvParseError` on an unterminated quote. Kept
  domain-free for fuzzing (PRD §15 lists the CSV importer as a fuzz target).
- `src/cli/SampleImport.{h,cc}` — pure CSV-row → `core::Sample` mapping and
  validation (`build_import`). No I/O, no DB, clock-injected: required fields,
  UUID/enum parse, box/position pairing (mirrors the samples CHECK), well-formed
  `custom_fields_json`, volume/mass value+unit pairing, and intra-file duplicate
  `(box_id, position_label)` detection. Server-managed columns (id, lab_id,
  status, created_*, last_modified_*, phi_fields_enc_json) are ignored if present,
  so a file cannot forge ownership/authorship or smuggle a row into another lab.
  Emits a per-row `ImportReport`.

**Changed:**
- `src/cli/SampleCommands.{h,cc}` — `run_sample_import()`. Structural gate first
  (any row error ⇒ nothing written, exit 1). `--dry-run`: each row inserted in
  its own transaction that is rolled back (never committed), so DB-level checks
  (item-type liveness, box existence, size-class) report per-row without a poison
  cascade. Normal mode: all rows inserted in a single transaction and committed
  all-or-nothing. RLS `current_lab_ids` injected on every transaction.
- `src/cli/CliApp.cc` — `sample import [--dry-run] --lab <uuid> --actor <uuid>
  <file|->` subcommand (reads stdin on `-`). `--actor` is the recorded importer.
- `src/cli/CMakeLists.txt` — adds `CsvReader.cc`, `SampleImport.cc`.

**Tests (`tests/unit/cli_test.cpp`, all in the existing cli suite):**
- CsvReader: simple rows, bare-LF / no-trailing-newline, comment-line skip,
  quoted comma/newline/doubled-quote, no trailing empty record, unterminated-quote
  throw.
- SampleImport (pure): valid mapping with server-controlled fields, ignores
  server-managed columns, missing-name / bad-UUID / box-without-position /
  intra-file-duplicate-position / bad-custom-JSON row errors, missing-required-
  header and empty-document header errors.
- run_sample_import (SQLite + Postgres-parameterized): persists transactionally,
  dry-run writes nothing, rejects unknown item-type at the DB layer. Plus an
  argv-level `sample import` end-to-end test reading from a file.

Verification:
- `cmake --build --preset dev` — clean.
- `ctest --preset dev -j1` — 889/889 passed (Postgres cli/conformance tests skip
  without `FMGR_TEST_POSTGRES_URL`). NOTE: under `-j$(nproc)` the pre-existing
  `grpc_integration` tests collide on fixed ports/paths and report failures; they
  pass in isolation and serially. Unrelated to this slice.
- `clang-format --dry-run --Werror` on all new/changed files — clean.
- `run-clang-tidy-17 -p out/build/dev` on the new/changed `.cc` — clean.
- `tools/check-spdx-headers.sh` — clean. `git diff --check` — clean.

Handoff notes:
- Dry-run cannot detect a `(box_id, position_label)` collision against *already
  committed* rows (the partial unique index fires at commit, which dry-run never
  reaches); intra-file collisions are caught structurally. Real-mode import
  surfaces a committed-row collision as a `UniqueViolation` that aborts the whole
  batch. Documented limitation, acceptable for v1.
- Import currently covers Sample only. PRD §13 also lists boxes, item types,
  custom-field definitions, and (admin) users — each is a follow-up that can reuse
  `CsvReader` and the `build_import`/report pattern.
- Remaining M1 CLI nouns (freezer/box/item-type create/list/inspect) are still
  outstanding; see Section F6 / L9 for the command-tree conventions.

## Handoff note — 2026-06-02, C5.1 PostgreSQL backend core + conformance suite

Implemented C5.1: `PostgresBackend` + `PostgresTransaction` core and Postgres-dialect migrations
0001–0012 with RLS policies and 13 conformance tests (skipped when `FMGR_TEST_POSTGRES_URL` unset).

**New files:**
- `src/storage/postgres/PostgresBackend.{h,cc}` — `IStorageBackend` implementation:
  - Connection pool (`PostgresBackendState`): fixed-size vector of `unique_ptr<pqxx::connection>`,
    condition-variable acquire/release, configurable timeout → `Unavailable` on exhaustion.
  - `PostgresTransaction`: `std::optional<pqxx::work>` for safe lifecycle management; set/reset
    explicitly before releasing pool slot; `set_session_var` uses `set_config($1, $2, true)`.
  - Migration runner: same checksum scheme as SQLite; `std::ranges::sort` by version; idempotent
    (skip already-applied); checksum mismatch throws `MigrationFailure`.
  - Migrations 0001–0012: Postgres-dialect SQL (JSONB, BIGINT, BOOLEAN, DEFERRABLE FK). All
    lab-scoped tables get `ENABLE ROW LEVEL SECURITY; FORCE ROW LEVEL SECURITY; CREATE POLICY`
    using `current_setting('app.current_lab_ids', true)`. Migration 0011 is a no-op (Postgres
    already has the full audit_events schema from migration 0001; SQLite needed a DROP+recreate).
  - Audit chain: advisory lock `pg_advisory_xact_lock(8675309)` serialises concurrent appends;
    `pqxx::params` builder for the INSERT; `std::optional<std::string>{}` for NULL lab_id.
  - `caps()`: `row_level_security=true`, `json_path_equality=true`, `json_path_indexes=true`,
    `listen_notify=true`.
  - Test hooks: `fail_next_audit_append_for_tests()`, `audit_event_count_for_tests()`,
    `downgrade_to_zero_for_tests()` — mirrors SQLite test hook interface.
- `src/storage/postgres/CMakeLists.txt` — links libpqxx + libsodium + FreezerManager::audit.
- `tests/backend_conformance/postgres_backend_conformance_test.cpp` — 13 tests:
  - Full conformance suite (10 tests mirror SQLite: CRUD, query DSL, tombstone, position
    uniqueness, unsupported entity, serializable isolation, concurrent placement, audit atomicity,
    audit-failure prevention, migration downgrade+forward).
  - 3 Postgres-specific: `CapabilitiesReportRowLevelSecurity`, `MigrateToLatestIdempotent`,
    `SetSessionVarVisibleWithinTransaction`.
  - All skip with `GTEST_SKIP()` when `FMGR_TEST_POSTGRES_URL` is unset (local dev without Docker).

**CI addition needed (not yet done):**
Add to `build.yml`:
```yaml
services:
  postgres:
    image: postgres:16
    env: { POSTGRES_DB: fmgr_test, POSTGRES_USER: postgres, POSTGRES_PASSWORD: test }
    ports: ['5432:5432']
    options: --health-cmd pg_isready --health-interval 5s --health-timeout 5s --health-retries 5
env:
  FMGR_TEST_POSTGRES_URL: postgresql://postgres:test@localhost/fmgr_test
```

**API notes for domain repository authors:**
- Include `storage/postgres/PostgresBackend.h` (which includes `<pqxx/pqxx>`).
- Access transaction via `txn.work()` returning `pqxx::work&`.
- Use `txn.work().exec(sql)` for no-param queries.
- Use `txn.work().exec(sql, pqxx::params{arg1, arg2, ...})` for parameterized queries.
- `exec` returns `pqxx::result`; iterate with `for (pqxx::row_ref row : result)`.
- Field access: `row.at("col_name").as<std::string>()` etc.
- Null params: `std::optional<std::string>{}` appended to `pqxx::params`.
- Error mapping: catch `pqxx::sql_error` and check `error.sqlstate()` (returns `std::string_view`).
- Call `txn.note_mutation(entity_kind, entity_id, ctx)` for every mutation (audit chain).

**What remains for C5 completion:**
- C5 domain repositories (IdentityRepositories, RoleRepositories, LayoutRepositories,
  BoxGeometryRepositories, ItemTypeRepositories, SampleRepositories, SessionRepositories,
  ShareRequestRepositories, AuditRepositories) — not yet implemented. Needed for CLI/CSV (M1).
- C5.2: CI `build.yml` Postgres service container.
- C5.3: RLS policy unit test (set `app.current_lab_ids` to wrong lab, assert 0 rows on
  domain tables). Deferred until domain repositories land.
- C5.4: `sync_custom_field_indexes(lab_id)` for JSONB GIN indexes on indexed CustomFieldDefinitions.

Verification:
- `cmake --build --preset dev` — clean.
- `ctest --preset dev -j1` — 428/428 passed (13 Postgres tests skipped, 415 pass).
- `clang-format --dry-run --Werror` on all new/changed C++ files — clean.
- `clang-tidy -p out/build/dev src/storage/postgres/PostgresBackend.cc` — exit 0.
- `tools/check-spdx-headers.sh` — all new files carry AGPL header.

## Handoff note — 2026-06-02, E3 RBAC middleware + D9.3 session expiry + permission caching

Implemented E3 (AuthMiddleware) and E3 addenda (D9.3 session expiry + permission caching):

**E3 (`src/rpc/AuthMiddleware.{h,cc}`, `tests/unit/auth_middleware_test.cpp`):**
- `AuthMiddleware::authorize(bearer, perm, lab_id?)` — 4-step gate: validate token →
  MFA check → permission check → lab-visibility check. Throws the appropriate `AuthError`
  subclass on any failure.
- `AuthMiddleware::inject_rls_vars(txn, ctx)` — sets `app.current_user_id` and
  `app.current_lab_ids` (comma-joined) as Postgres session vars via
  `ITransaction::set_session_var`. No-op for SQLite (default no-op virtual).
- Static `RpcRegistry` (mutex-guarded `unordered_map`) for compile-time RPC-to-permission
  registration. CI test (F2) will assert every gRPC method appears in this registry.
- 15 integration tests covering: auth success, MFA gate, permission gate, lab-visibility gate,
  hard-delete / key-rotate restrictions, RLS injection, registry register/lookup, token revocation.
- `src/rpc/CMakeLists.txt`: `freezermanager_rpc` static library target.
- `tests/unit/CMakeLists.txt`: `freezermanager_rpc_unit_tests` executable.

**D9.3 session expiry (`src/auth/LocalAuthProvider.{h,cc}`):**
- New config fields: `max_session_idle_seconds` (default 12 h), `max_session_abs_seconds`
  (default 7 d), `last_seen_update_interval_seconds` (default 60 s).
- `check_session_expiry(session, now)` throws `TokenExpired` on idle or absolute violation.
- `update_last_seen_if_needed(session, now)` does a best-effort rate-limited DB write (one
  write per ≥ 60 s per session); failures are silently ignored to prevent a concurrent-request
  race from breaking the auth path.

**Permission caching (`src/auth/LocalAuthProvider.{h,cc}`):**
- New config field: `session_ctx_cache_ttl_seconds` (default 300 s / 5 min; 0 = disabled).
- `lookup_or_build_context(session)` checks `ctx_cache_` (mutex-guarded, keyed by
  `session_id.to_string()`) before calling `build_session_context()`. MFA flag always comes
  from the live DB session row, never from the cache.
- `cache_evict(session_id_str)` called in `revoke_session()`.
- `cache_evict_user(uid)` called in `revoke_all_sessions()` — scans all entries for
  matching `user_id` and removes them.

**Tests added:**
- 4 session-expiry tests: idle timeout throws, absolute TTL throws, within limits passes,
  last_seen updates after interval (audit count increases).
- 1 rate-limit test: last_seen NOT updated within interval (audit count unchanged).
- 2 cache-invalidation tests: revoke_session and revoke_all_sessions each clear their
  respective cache entries, so a subsequent validate_token still throws.
- Total: 409/409 tests pass (up from 402, +7 new tests).

Verification completed locally:
- `cmake --build --preset dev` — clean.
- `ctest --preset dev -j1` — 409/409 passed.
- `clang-format --dry-run --Werror` on all new/changed files — clean.
- `clang-tidy -p out/build/dev src/auth/LocalAuthProvider.cc src/rpc/AuthMiddleware.cc` — exit 0.
- `tools/check-spdx-headers.sh` — only pre-existing `.agents/` failures; all project files clean.
- `git diff --check` — no trailing whitespace.

Handoff notes:
- `ITransaction::set_session_var` is a non-pure virtual with a default no-op in
  `IStorageBackend.h`. SQLite transactions use the no-op; PostgresTransaction (C5) must
  override it with `SET LOCAL key = val` so RLS works.
- DB-backed lockout persistence (E2.2) and password reset (E2.1) remain deferred to
  migration 0013 when `IEmailSender` (Section O) lands.
- API token creation RPCs (E4) remain deferred to F2 (gRPC layer).
- C5 (Postgres backend) is the next major deliverable: it must override `set_session_var`,
  mirror all migrations 0001–0012, add RLS policies, and pass the full conformance suite.

This file expands the milestones in [`doc/PRD.md`](./doc/PRD.md) into concrete,
executable tasks. Tasks are sized to be implementable independently by a
single developer or agent in a few hours to a few days. Cross-module
dependencies are called out explicitly under **⚠ Watch** so that earlier
tasks are not "finished" in a way that boxes in later ones.

## Handoff note — 2026-06-01, E1 IAuthProvider interface + E2 LocalAuthProvider

Implemented E1 (IAuthProvider + AuthTypes) and E2 (LocalAuthProvider + TOTP helper):

**E1 (`src/auth/AuthTypes.h`, `src/auth/IAuthProvider.h`):**
- `PasswordCredentials`, `ApiTokenCredentials`, `AuthCredentials` variant.
- `ClientInfo` (optional IP + user-agent).
- `AuthToken` (session_id, plaintext_token, mfa_complete).
- `SessionContext` (user_id, visible_labs, permissions set, mfa_complete).
- Auth error hierarchy: `AuthError` → `InvalidCredentials`, `AccountLocked`, `MfaRequired`,
  `TokenExpired`, `TokenRevoked`, `PermissionDenied`.
- `IAuthProvider` pure-virtual interface: `authenticate`, `validate_token`, `verify_totp`,
  `revoke_session`, `revoke_all_sessions`.
- 21 unit tests in `tests/unit/auth_types_test.cpp`.

**E2 (`src/auth/Totp.{h,cc}`, `src/auth/LocalAuthProvider.{h,cc}`):**
- `base32_decode` + `totp_generate` + `totp_verify` (RFC 6238, HMAC-SHA1 via OpenSSL
  3 EVP_MAC API; ±1-step window). RFC 6238 known-answer test vectors pass.
- `LocalAuthProvider`: Argon2id password hash/verify via `crypto_pwhash_str` /
  `crypto_pwhash_str_verify`; BLAKE2b-256 session token hashing via `crypto_generichash`.
- Token format: session bearer = 64 hex chars; API token bearer = "fmgr_pat_" + 64 hex chars.
- Password stored in `User.auth_bindings` JSON as `[{"provider":"local","hash":"$argon2id$..."}]`.
- MFA: if `User.totp_secret_enc` is set, authenticate() returns `mfa_complete=false`;
  `verify_totp()` sets it to true in the Session row.
- Account lockout: in-memory `unordered_map` keyed by lowercase email, `std::scoped_lock`.
  Locks after `max_failures_before_lockout` (default 5). AccountLocked thrown on the triggering
  failure itself. State resets on server restart (DB-backed lockout deferred to E2.2).
- `build_session_context()` and `build_api_token_context()` resolve permissions by querying
  LabMembership → RolePermission at request time (caching deferred to E3).
- Schema: added `sessions.mfa_complete INTEGER NOT NULL DEFAULT 1` via migration 0012.
- 9 TOTP tests + 20 LocalAuthProvider integration tests.

Verification completed locally:
- `cmake --build --preset dev`
- `ctest --preset dev -j1` — 357/357 tests passed (up from 324, +33 new tests).
- `clang-format --dry-run --Werror` on all new/changed C++ files — clean.
- `clang-tidy -p out/build/dev src/auth/Totp.cc src/auth/LocalAuthProvider.cc` — exit 0.
- `tools/check-spdx-headers.sh` — all new C++/SQL files carry the AGPL header.
- `git diff --check` — no trailing whitespace.

Handoff notes:
- E2.1 (password reset flow) requires email transport (Section O). Deferred.
- E2.2 (DB-backed lockout persistence) is a security improvement for multi-process deployments.
  Add a `login_attempts` table in migration 0013 when Section O lands.
- E3 (RBAC middleware) is the next slice. It will add per-session permission caching and the
  Postgres RLS session variable injection (`app.current_user_id`, `app.current_lab_ids`).
- E4 (API token creation RPCs) belongs in F2 (gRPC layer); E2 handles only token *validation*.
- The `totp_secret_enc` field is stored **in plaintext** in the DB for now. H3 (field-level
  PHI encryption) will wrap it with the KMS key. Until then, treat it as a non-PHI secret.
- C5 (Postgres backend) must mirror migration 0012 (`mfa_complete` column) and preserve the
  `DEFAULT 1` so in-flight sessions survive the migration.

## Handoff note — 2026-05-31, D9 Session entity + ApiToken

Implemented D9 server-side session and API-token domain slice:

- `src/core/ids.h` adds `ApiTokenIdTag` and `ApiTokenId` (SessionId was already present).
- `src/core/session.h` defines `Session` (id, user_id, token_hash, token_prefix,
  created_at, last_seen_at, ip, user_agent, revoked_at) and `ApiToken` (id, user_id,
  lab_id, name, scope_json, token_hash, token_prefix, created_at, expires_at, revoked_at)
  with JSON serialization. Both use a token_hash/token_prefix scheme: the auth layer
  Argon2id-hashes the full random token and stores only the hash; the prefix is plaintext
  for O(log n) lookup. Rate-limiting last_seen_at updates is the auth layer's responsibility;
  the repository stores whatever it is given.
- `src/storage/SessionTraits.h` adds `EntityTraits<Session>` and `EntityTraits<ApiToken>`,
  both using `Field::RevokedAt` as the tombstone field.
- SQLite migration `0010_sessions` creates `sessions` and `api_tokens` tables.
  Key constraint: partial unique index `ON sessions(token_prefix) WHERE revoked_at_micros IS NULL`
  (enforced at commit/flush time, not at stage_insert time). No ON DELETE CASCADE;
  tombstone propagation is application-level.
- `src/storage/sqlite/SessionRepositories.{h,cc}` adds `SessionRepository` and
  `ApiTokenRepository`. Default query filter: `WHERE revoked_at_micros IS NULL`. soft_delete()
  sets `revoked_at_micros = now()`. ApiTokenRepository additionally accepts optional lab_id (null
  = system-level token).
- `src/storage/CMakeLists.txt` adds `SessionRepositories.cc` to the sqlite library target.

Verification completed locally:

- `cmake --build --preset dev`
- `ctest --preset dev -j1` — 259/259 tests passed (up from 229, +30 new session/API-token tests).
- `clang-format --dry-run --Werror` on all new/changed C++ files — clean.
- `clang-tidy -p out/build/dev src/storage/sqlite/SessionRepositories.cc` — exit 0, no errors.
- `tools/check-spdx-headers.sh` — all new C++/SQL files carry the AGPL header.
- `git diff --check` — no trailing whitespace.

Handoff notes:

- D9.1 (schema/types/repos) is complete. D9.2 (RPCs: list_my_sessions, revoke_session,
  revoke_all_sessions) is deferred to F2 (gRPC layer). D9.3 (auto-expire idle sessions)
  should be enforced in the auth middleware (E3), not in the repository.
- The token_prefix partial unique index fires at commit time, not at stage_insert. Tests that
  verify prefix collision are structured to EXPECT_THROW(txn->commit(), UniqueViolation) rather
  than wrapping insert().
- E1 (IAuthProvider interface) is the natural next slice — it can now reference Session and
  ApiToken as concrete types. E2 (LocalAuthProvider) follows.
- C5 (Postgres backend) must mirror migration 0010_sessions with the same version number
  and preserve the no-ON-DELETE-CASCADE design.

## Handoff note — 2026-05-31, D4/D6 checkbox cleanup

Ticked D4 outer checkbox (D4.1 and D4.2 were both complete but outer was left
open) and all D6.* checkboxes (ItemType, CustomFieldDefinition, validator engine,
and is_phi flag all implemented in the D6 commit). D6.4 note: the schema column
and type flag are in place; enforcement (routing phi fields through the encryption
layer) is deferred to H3 (PHI/KMS section). No code changes — bookkeeping only.

D9 (Session entity) is the next domain slice. It is a blocker for E1 (IAuthProvider
interface) because the auth layer needs to store and validate opaque server-side
sessions and API tokens. Recommended implementation order: D9 → E5.1 (audit
schema) → E1 → E2 (LocalAuthProvider) → E3 (RBAC middleware).

## Handoff note — 2026-05-31, D8 ShareRequest + ShareRequestApproval

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

## Handoff note — 2026-05-31, D7 Sample + Project + SampleProject + CheckoutEvent

Implemented D7 against the existing geometry, layout, identity, item-type, and box slices:

- `src/core/sample.h` defines `Sample`, `Project`, `SampleProjectId`, `SampleProject`,
  and `CheckoutEvent` with JSON serialization. Also adds `VolumeUnit`/`MassUnit` JSON
  converters (needed to persist them individually as separate DB columns).
- `src/storage/SampleTraits.h` adds `EntityTraits` specializations for all four entities.
  `Sample` uses `Field::Status` as its tombstone field (status = tombstoned, not
  archived_at_micros). `SampleProject` and `CheckoutEvent` use dummy tombstone fields
  (hard-delete and insert-only, respectively).
- SQLite migration `0008_samples` creates `projects`, `samples`, `sample_projects`, and
  `checkout_events` tables. Key constraints:
  - `CHECK ((box_id IS NULL) = (position_label IS NULL))` on samples.
  - Partial unique index `samples_position_unique` on `(box_id, position_label)` WHERE
    `status IN ('active', 'checked_out')` — the core no-double-booking invariant.
- `src/storage/sqlite/SampleRepositories.{h,cc}` adds four typed SQLite repositories:
  - `SampleRepository`: validates non-empty name, item_type_id liveness, box existence,
    position label existence in BoxType, and size_class compatibility via
    `box_type_position_accepts`. Soft-delete sets `status = tombstoned`.
  - `ProjectRepository`: standard CRUD + soft-delete via `archived_at_micros`.
  - `SampleProjectRepository`: composite-key link table; hard-deleted via `soft_delete()`;
    `update()` throws `UnsupportedOperation`.
  - `CheckoutEventRepository`: append-only audit records; `update()` and `soft_delete()`
    throw `UnsupportedOperation`.

Verification completed locally:

- `cmake --build --preset dev`
- `ctest --preset dev` — 175/175 tests passed (up from 103).
- `FMGR_STORAGE_STRESS=1 ctest --preset dev -R SqliteBackendConformance`
  — 10/10 SQLite conformance tests passed.
- `clang-format --dry-run --Werror` on all new/changed files — clean.
- `clang-tidy -p out/build/dev` on new .cc and test files — clean (exit 0).
- `tools/check-spdx-headers.sh` — all new C++/SQL files carry the AGPL header.
- `git diff --check` — no trailing whitespace.

Handoff notes:

- D7.1 through D7.4 are implemented. D7.5 (move atomicity property test: 50 threads
  moving the same sample concurrently) is not yet a test; it can be added to the
  property test suite (`tests/property/`) when RapidCheck integration lands.
- Sample state machine (active → checked_out → active → depleted → tombstoned) is NOT
  enforced in the repository layer — enforcing it at the RPC layer (F2) is intentional.
  The repository allows writing any valid status; the partial unique index enforces the
  no-double-booking invariant regardless of how status transitions are orchestrated.
- Cross-entity seeding in tests must use separate committed transactions when entities
  have validation cross-references (e.g. BoxType validates ContainerType.size_class in
  the DB, not in the pending staging map).
- C4.3 (JSON-path generated columns for indexed CustomFieldDefinition fields) now has
  its dependency (CustomFieldDefinition + samples.custom_fields_json) in place; it can
  be implemented at any time.
- D8 (ShareRequest) is the natural next domain entity slice.
- C5 (Postgres backend) must mirror migration 0008_samples with the same version number
  and preserve the no-ON-DELETE-CASCADE design on boxes and sample_projects.

## Handoff note — 2026-05-27, D4.2 seed templates + D5 Box entity

Implemented D4.2 and D5 against the existing geometry + layout slices:

- `data/seed/container_types.json` defines four standard ContainerType stubs
  (cryovial_2ml, tube_50ml, tube_15ml, microplate_well) importable by lab admins.
- `data/seed/box_types/` holds four BoxType seed templates: `9x9_cryobox.json`
  (81 positions), `10x10_cryobox.json` (100 positions), `96_well_rack.json`
  (96 positions), and `mixed_eppendorf.json` (13 positions: 3×3 for 50 mL
  tubes + 2×2 for 15 mL tubes).
- `src/core/box.h` adds the `Box` struct with 9 fields and JSON serialization;
  the file already contained ContainerType / BoxType / Position from D4.1.
- `src/storage/BoxGeometryTraits.h` adds `EntityTraits<Box>`.
- SQLite migration `0006_boxes` creates the `boxes` table with FKs to
  `labs`, `box_types`, and `storage_containers` (all deferrable, no ON DELETE
  CASCADE — tombstone propagation is application-level). Unique index on
  `(lab_id, label) WHERE archived_at_micros IS NULL`.
- `src/storage/sqlite/BoxGeometryRepositories.{h,cc}` adds `BoxRepository` and
  `register_box_repositories()`. Validation enforces: non-empty label;
  `box_type_id` must reference a live BoxType in the same lab; `storage_container_id`
  must reference a live StorageContainer in the same lab — both via direct SQL
  queries (same pattern as D4.1 size-class cross-reference checks).

Verification completed locally:

- `cmake --build --preset dev`
- `ctest --preset dev` — 103/103 tests passed (up from 88).
- `FMGR_STORAGE_STRESS=1 ctest --preset dev -R SqliteBackendConformance`
  — 10/10 SQLite conformance tests passed.
- `clang-format --dry-run --Werror` on all new/changed files — clean.
- `clang-tidy -p out/build/dev` on new .cc and test files — clean.
- `tools/check-spdx-headers.sh` — all new C++/SQL files carry the AGPL header.
- `git diff --check` — no trailing whitespace.

Handoff notes:

- D4.2 and D5 checkboxes are ticked.
- The seed JSON files have no `id`, `lab_id`, `created_at`, or `archived_at`
  fields — an importer (future CLI command or RPC) must supply these on ingestion.
  The seed template test (`tests/unit/seed_templates_test.cpp`) validates
  position counts and structure via the `FMGR_SEED_DATA_DIR` compile definition.
- D6 (`ItemType` + `CustomFieldDefinition`) is the natural next slice — it
  unblocks D7 (Sample) and triggers C4.3 (JSON-path indexes on indexed custom fields).
- C5 (Postgres backend) when started must mirror migration `0006_boxes`
  with the same version number; the application-level tombstone propagation
  constraint (no ON DELETE CASCADE on `storage_container_id`) must be preserved.
- D5 Watch: if a StorageContainer is soft-deleted, the application must cascade
  the tombstone to all Boxes in that container. No automated cascade exists in the
  schema by design (preserves audit history of sample locations).

## Handoff note — 2026-05-09, D4 core geometry

Implemented D4 core against the existing identity + SQLite storage slices:

- `src/core/box.h` defines `ContainerType`, `BoxType`, `Position`, and
  `OuterDimensionsMm` value types with `Field` enums and JSON conversions.
- `src/storage/BoxGeometryTraits.h` adds `EntityTraits<ContainerType>` and
  `EntityTraits<BoxType>`, both using `ArchivedAt` as the tombstone field.
- SQLite migration `0005_box_types` creates `container_types`, `box_types`,
  `box_type_positions`, and `box_type_position_accepts`. Positions and
  accepts are child rows rather than embedded JSON so D5 placement checks can
  query geometry directly.
- `src/storage/sqlite/BoxGeometryRepositories.{h,cc}` adds typed SQLite
  repositories. `ContainerType` validates non-empty `name`/`size_class` and
  positive dimensions when present. `BoxType` validates unique position
  labels, non-negative coordinates, non-empty unique accepts lists, and
  accepted `size_class` tokens that resolve to live `ContainerType` rows in
  the same lab.
- `BoxType` updates atomically replace the persisted position/accept rows
  for that box type inside the same transaction.

Verification completed locally:

- `cmake --build --preset dev`
- `ctest --preset dev` — 88/88 tests passed (up from 76).
- `FMGR_STORAGE_STRESS=1 ctest --preset dev -R SqliteBackendConformance`
  — 10/10 SQLite conformance tests passed.
- `clang-format --dry-run --Werror` on all new D4 source/test files.
- `clang-tidy -p out/build/dev` on the new `.cc` and test files — clean
  (only third-party non-user-code warnings, all suppressed).
- `tools/check-spdx-headers.sh`
- `git diff --check`

Handoff notes:

- D4.1 checkbox is ticked.
- D4 remains open only for D4.2 standard BoxType seed JSON templates.
- C5 (Postgres backend) when started must mirror migration `0005_box_types`
  with the same version number and preserve the D4.1 validation contract.
- D5 (`Box`) can now use `box_type_positions` and
  `box_type_position_accepts` to enforce placement compatibility.

## Handoff note — 2026-05-08, D3 Freezer + StorageContainer recursive layout

Implemented D3 against the existing identity + role slices:

- `src/core/ids.h` adds `FreezerId` (StrongId tag).
- `src/core/freezer.h` defines `Freezer`, `StorageContainer`, and
  `CapacityHint` value types with `Field` enums and JSON conversions.
  `CapacityHint.{rows,cols,depth}` are advisory `std::optional<int>`
  per PRD §4.1 — no enforcement at write time.
- `src/storage/FreezerTraits.h` adds `EntityTraits<Freezer>` and
  `EntityTraits<StorageContainer>`, both using `ArchivedAt` as the
  tombstone field.
- SQLite migration `0004_layout` creates `storage_containers` and
  `freezers` with deferred-FK self/cross references so the layout root
  container and its parent freezer can be inserted in a single
  transaction in either order. `freezers (lab_id, name)` is uniquely
  indexed only among non-archived rows; `storage_containers (id,
  parent_id)` carries a `CHECK (id <> parent_id)` self-parent guard.
  The migration is also committed as
  `src/storage/sqlite/migrations/0004_layout.sql` for reference; the
  authoritative copy used by the runtime is the inline R-string in
  `SqliteBackend.cc::default_migrations()`.
- `src/storage/sqlite/LayoutRepositories.{h,cc}` adds typed SQLite
  repositories for both entities. `StorageContainer` writes (insert
  and update) invoke `check_no_cycle()`, which walks the proposed
  ancestor chain through both staged in-transaction state and
  persisted rows and rejects cycles with `ConstraintViolation`.
  Soft-delete bypasses the cycle check (parent_id unchanged).
- `register_layout_repositories()` registers `StorageContainer` first
  and `Freezer` second; either ordering works at commit time because
  of the deferred FKs, but registering the container repo first makes
  the parent-before-child reading order intuitive in tests.

Verification completed locally:

- `cmake --build --preset dev`
- `ctest --preset dev` — 76/76 tests passed (up from 63).
- `FMGR_STORAGE_STRESS=1 ctest --preset dev -R SqliteBackendConformance`
  — 10/10 SQLite conformance tests passed.
- `clang-format --dry-run --Werror` on all new/changed files (clean
  after one auto-format pass).
- `clang-tidy -p out/build/dev` on the new `.cc` and test files —
  clean (only third-party non-user-code warnings, all suppressed).
- `tools/check-spdx-headers.sh`
- `git diff --check`

Handoff notes:

- D3 checkbox is ticked.
- D4 (`ContainerType` + `BoxType` + `Position`) is the natural next
  slice — it unblocks D5 and the no-double-occupancy invariant tests
  in C3.3. The standard-library BoxType templates (D4.2) are
  importable seed JSON and can land in the same slice or a follow-up.
- C5 (Postgres backend) when started must mirror migration `0004_layout`
  with the same version number; the SQLite-only `CHECK (id <> parent_id)`
  is fine to keep, but the cycle check will need to be a Postgres
  trigger or RECURSIVE CTE since libpqxx callers should not pay an
  extra round-trip per write.
- Capacity-hint *enforcement* is intentionally deferred. The PRD
  treats hints as advisory; D5 (Box) will be the first place that
  could optionally consult them.
- `freezerctl` CLI commands (D-section says "create/list/inspect")
  remain deferred until K5/CLI scaffolding lands.

## Handoff note — 2026-05-08, D2 roles, permissions, role-scoped memberships

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

## Handoff note — 2026-05-08, C4 SQLite reference backend

Implemented the Section C4 SQLite reference backend in `src/storage/sqlite/`
with SQLite-backed unit and conformance coverage. Delivered a
`FreezerManager::storage_sqlite` target, `SqliteBackend`/`SqliteTransaction`,
connection setup for foreign keys, WAL on file-backed databases, 5 s busy
timeout, JSON1 verification, atomic migration metadata, portable SQLite error
mapping, same-transaction audit append hooks, and repository factory plumbing
for future production entities. The SQLite conformance driver remains
test-only and owns its temporary `SqliteConformanceSample` schema; real D1-D8
entities should register their own repositories without depending on this test
schema. C4.3 generated JSON-path columns/indexes remains intentionally
deferred until D6 lands `CustomFieldDefinition`.

Verification completed locally:

- `cmake --build --preset dev`
- `ctest --preset dev` — 38/38 tests passed.
- `FMGR_STORAGE_STRESS=1 ctest --preset dev -R SqliteBackendConformance` —
  10/10 SQLite conformance tests passed.
- `clang-format --dry-run --Werror src/storage/sqlite/SqliteBackend.h src/storage/sqlite/SqliteBackend.cc tests/backend_conformance/sqlite_backend_conformance_test.cpp tests/unit/sqlite_backend_test.cpp`
- `clang-tidy -p out/build/dev src/storage/sqlite/SqliteBackend.cc tests/backend_conformance/sqlite_backend_conformance_test.cpp tests/unit/sqlite_backend_test.cpp`
- `tools/check-spdx-headers.sh`
- `git diff --check`

Handoff notes:

- C4.1, C4.2, and C4.4 are complete for the current backend abstraction.
- C4.3 should be implemented during/after D6, when indexable
  `CustomFieldDefinition` records exist.
- SQLite now passes the reusable C3 behavioral suite, including stress mode
  for concurrent active-position uniqueness.
- The next implementation slice can start D1 (`Lab`, `User`,
  `LabMembership`) or C6 migration harness if migration rigor is prioritized
  before domain entities.

## Handoff note — 2026-05-07, C3 backend conformance test suite

Implemented the Section C3 backend conformance harness in
`tests/backend_conformance/` with a test-only in-memory backend driver.

Delivered:

- `freezermanager_backend_conformance_tests` GoogleTest executable.
- Test-only `ConformanceSample` entity and `EntityTraits` specialization to
  exercise the storage API before production Section D entities exist.
- In-memory `IStorageBackend`, transaction, and repository implementation used
  only by the conformance suite.
- Conformance coverage for CRUD, query DSL filtering/sorting/pagination,
  soft-delete visibility, portable errors, serializable conflicts, concurrent
  box-position uniqueness, audit atomicity, and migration up/down hooks.
- Stress mode for the placement invariant via `FMGR_STORAGE_STRESS=1`.

Verification completed locally:

- `cmake --build --preset dev`
- `ctest --preset dev` — 25/25 tests passed.
- `FMGR_STORAGE_STRESS=1 ctest --preset dev -R BackendConformance` — 10/10
  conformance tests passed.
- `clang-format --dry-run --Werror tests/backend_conformance/storage_backend_conformance_test.cpp`
- `clang-tidy -p out/build/dev tests/backend_conformance/storage_backend_conformance_test.cpp`
- `tools/check-spdx-headers.sh`
- `git diff --check`

Handoff notes:

- The suite is backend-neutral and contains no SQL or dialect-specific setup.
- C3 is complete as the reusable conformance harness; entity-by-entity
  expansion should happen during D1-D8 as real production entities land.
- Future SQLite/Postgres backends should plug into this suite through a
  backend-specific conformance driver, then pass it before backend work is
  considered complete.
- The next implementation slice should start at C4: SQLite reference backend,
  unless project hygiene tasks such as B5.5/B7/B8 are prioritized first.

## Handoff note — 2026-05-07, C2 storage abstraction interface

Implemented the Section C2 storage abstraction slice in `src/storage/` with
focused unit coverage in `tests/unit/storage_interface_test.cpp`.

Delivered:

- `IStorageBackend`, `ITransaction`, and typed `IRepository<T>` interfaces.
- `SchemaVersion`, `IsolationLevel`, `Capabilities`, and `MutationContext`
  support types.
- Portable `BackendError` hierarchy with actionable error codes such as
  `UniqueViolation`, `ForeignKeyViolation`, `SerializationFailure`,
  `Unavailable`, and `UnsupportedOperation`.
- Header-only typed query DSL supporting equality, range, IN-list, JSON-path
  equality, pagination, sort, and soft-delete-aware default visibility with
  explicit tombstone opt-in.
- CMake wiring for the storage interface target and storage unit test
  executable.

Verification completed locally:

- `cmake --build --preset dev`
- `ctest --preset dev` — 15/15 tests passed.
- `clang-format --dry-run --Werror src/storage/IStorageBackend.h tests/unit/storage_interface_test.cpp`
- `clang-tidy -p out/build/dev tests/unit/storage_interface_test.cpp`
- `tools/check-spdx-headers.sh`
- `git diff --check`

Handoff notes:

- The storage interfaces are intentionally backend-neutral and contain no SQL
  strings or dialect-specific types.
- Real domain entities are still deferred to Section D; C2 uses
  `EntityTraits<T>` so later entity slices can declare fields without changing
  backend APIs.
- `IRepository<T>` is a templated virtual interface by design; narrow
  `clang-tidy` suppressions document this on the abstract methods.
- The next implementation slice should start at C3: backend conformance tests
  for any future storage backend before SQLite/Postgres implementation work.

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

## Handoff note — D1 identity domain persistence

Implemented D1.1 and D1.2 for `Lab`, `User`, and `LabMembership`:
core identity types, storage traits, SQLite `0002_identity` migration,
production SQLite repositories, case-insensitive user email uniqueness,
foreign-key-backed memberships, soft-delete visibility, and focused unit
coverage. `Lab` soft-delete uses `archived_at_micros`; `User` soft-delete
sets `disabled`; `LabMembership` soft-delete sets `revoked_at_micros`.

D1.3 remains deferred: the initial `SystemAdmin` first-run wizard should land
after D2 provides role/permission tables, or with K5 once CLI bootstrap,
auth, KMS, and TLS setup exist.

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

- [ ] **C4. SQLite reference backend** (`src/storage/sqlite/`).
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

- [ ] **C5. PostgreSQL reference backend** (`src/storage/postgres/`).
  - [ ] **C5.1.** `PostgresBackend` using libpqxx; connection pool sized by
        config. Use Postgres ≥ 16.
  - [ ] **C5.2.** Migrations under `src/storage/postgres/migrations/` with
        the same numbering scheme as SQLite. Migration runner refuses to
        proceed if SQLite and Postgres migration counts diverge.
  - [ ] **C5.3.** Row-Level Security policies on every domain table keyed
        on `app.current_user_id` and `app.current_lab_ids` settings set
        per-connection by the auth layer (see D3).
  - [ ] **C5.4.** JSONB columns for `custom_fields_json`; GIN indexes on
        fields marked indexable.
  - [ ] **C5.5.** Pass full conformance suite from C3.
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

- [ ] **D9. `Session` entity & device tracking.** PRD §7.1 requires
      server-side opaque sessions but no schema task currently exists.
  - [x] **D9.1.** Schema: `(id, user_id, token_hash, created_at,
        last_seen_at, ip_inet, user_agent, revoked_at)`. Token stored
        as Argon2id hash; only the prefix is plaintext for lookup.
        Also includes `ApiToken` (id, user_id, lab_id, name, scope_json,
        token_hash, token_prefix, expires_at, revoked_at).
  - [ ] **D9.2.** RPCs: `list_my_sessions`, `revoke_session(id)`,
        `revoke_all_sessions` ("log me out everywhere"). All audited.
        Deferred to F2 (gRPC layer).
  - [ ] **D9.3.** Auto-expire idle sessions (configurable; default 12 h
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

- [ ] **E3. RBAC middleware** (`src/rpc/auth_middleware.cc`).
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

- [ ] **E5. Audit log** (`src/audit/`).
  - [ ] **E5.1.** `audit_event` schema with `prev_hash`, `this_hash`.
        Insert is the only allowed write; no UPDATE, no DELETE, enforced
        with a DB trigger.
  - [ ] **E5.2.** Canonical-JSON serializer (RFC 8785 / JCS) for
        `before_json`/`after_json` so hashes are reproducible.
  - [ ] **E5.3.** Hash-chain verifier CLI: `freezerctl audit verify`
        walks the chain and reports the first divergence.
  - [ ] **E5.4.** Nightly checkpoint job: HMAC-SHA-256 the latest hash
        with a key sourced from `IKmsProvider` (Section H), persist the
        checkpoint to a separate `audit_checkpoint` table.
  - [ ] **E5.5.** PHI-read audit kind: a distinct event when a user reads
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
  - [ ] Fan out the remaining 5 services (Box, ItemType, Role, Audit, Share) —
        mechanical copies of the route pattern.
  - [ ] Streaming RPCs bridged to SSE / WebSocket (live sample-list, audit feed,
        bulk-import progress).
  - **⚠ Watch:** the REST gateway is what the React SPA and Python
    client speak. Any breaking change to a `.proto` must increment the
    `v1` package label.

- [ ] **F4. TLS configuration**. TLS 1.3 only; HSTS; modern ciphers.
      Self-signed cert for dev, documented refusal-to-start without a
      cert in production mode (`FMGR_ENV=production`).

- [ ] **F5. Health/metrics endpoints**. `/health` (liveness + readiness),
      `/metrics` (Prometheus). Both unauthenticated; `/metrics` SHOULD
      be bound to localhost or behind reverse-proxy ACL by default.

- [ ] **F6. Qt 6 desktop client** (`src/qt/`). gRPC client.
  - [ ] **F6.1.** Login screen + TOTP prompt + session keychain storage.
  - [ ] **F6.2.** Sample browser (full-text + structured + custom-field
        filter), virtualized table for 100k+ rows.
  - [ ] **F6.3.** Box view: drag-and-drop placement; rejection from
        server surfaces as a clear "size mismatch" toast.
  - [ ] **F6.4.** Bulk check-in/check-out with barcode-scanner focus
        mode (the focused field accepts HID keyboard input and
        auto-submits on Enter or after a configurable inactivity gap).
  - [ ] **F6.5.** CSV import wizard (dry-run first; show validation
        report; confirm; import).
  - [ ] **F6.6.** CSV export from any list view.

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

- [ ] **H1. `IKmsProvider` interface** (`src/kms/IKmsProvider.h`):
      `wrap_dek(dek) → wrapped`, `unwrap_dek(wrapped) → dek`.

- [ ] **H2. KMS implementations**:
  - [ ] **H2.1.** `EnvVarKms` — for tests/dev only. Refuses to load
        if `FMGR_ENV=production`.
  - [ ] **H2.2.** `OsKeyringKms` — systemd-creds backed; default for
        production single-server deployments.
  - [ ] **H2.3.** `VaultKms` — HashiCorp Vault transit engine;
        configurable mount path and key name.

- [ ] **H3. Field-level PHI encryption.** Per-record DEK, generated at
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
  - [ ] **H5.2.** SQLite path: `sqlite3_backup` hot copy + nightly
        rotation. Same encryption.
  - [ ] **H5.3.** `freezerctl backup run | list | restore` CLI.
  - [ ] **H5.4.** Weekly restore-drill job: pick a random recent
        backup, restore into a temp DB, run integrity checks, audit
        the result. Failures page (or email) the system admin.
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

- [ ] **L1. Structured logging** (spdlog → JSON sink to stdout).
      Required fields on every record: `ts`, `level`, `request_id`,
      `actor_user_id` (nullable), `lab_id` (nullable), `event`. PHI
      goes through `redact()` (H4).

- [ ] **L2. Request-id propagation**. Generate at the RPC entry; carry
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
