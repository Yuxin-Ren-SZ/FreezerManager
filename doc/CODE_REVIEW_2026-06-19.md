# Consolidated Review: M5 — PHI Encryption + Backup/Restore/Scheduler

**Reviewed range:** `0579443..6247a90` (merge of 3 M5 sub-branches into `dev`)
**Date:** 2026-06-19
**Scope:** All M5 features — PHI field encryption, production KMS, key rotation, encrypted backup, restore, restore-drill, retention, in-server scheduler.
**Files:** 61 changed, +4979 / −38
**Cross-reference:** `doc/CODE_REVIEW_2026-06-12.md`, `doc/security-audit-2026-06-13.md`, prior `review-m5-phi-encryption.md` (2026-06-17)

---

## Executive Summary

The M5 work is architecturally sound. The cryptography design — envelope encryption with per-record DEKs, AEAD everywhere (`crypto_secretbox` / `crypto_secretstream_xchacha20poly1305`), `sodium_memzero` on all DEK paths, key separation (master ≠ backup), and DEK-only re-wrap for rotation (no plaintext PHI exposure) — is correct and well-tested.

Of 27 total tracked findings from three prior reviews, 16 are now resolved, 7 remain unfixed (carried forward below), and 4 new findings were discovered in the backup/scheduler/restore code. Two 🔴 findings warrant attention before production deployment: (1) unbounded `getline` in `FileCipher::decrypt_file` — a remote DoS vector; (2) `std::random_device` still used for entity-ID generation in four service implementation files, carrying forward audit C-1 as partially unfixed.

---

## 🔴 Critical / Bug

### F-1 (NEW) — `src/crypto/FileCipher.cc:182`: Unbounded manifest read — remote DoS

`std::getline(input, manifest_line)` reads the first line of a backup file with no size limit. An attacker who can place a crafted file in the backup directory (or supply one to a restore/verify path) can cause memory exhaustion with a multi-GB first "line" before the chunk parser ever runs.

**Fix:** Cap the manifest line at 4096 bytes. Use `std::ifstream::get()` or `std::getline` with a max-chars guard; throw `CipherError` if exceeded.

### F-2 (C-1 from security-audit-2026-06-13, PARTIALLY UNFIXED) — `std::random_device` still used for entity-ID generation in four services

`core::generate_uuid_v4()` in `src/core/uuid.h` was correctly migrated to `randombytes_buf()` (+ L-1 fixed as a side effect). However, four service `.cc` files still carry their own copy-pasted `generate_uuid_v4()` using `std::random_device` (and their own `snprintf`):

| File | Line | IDs generated |
|------|------|---------------|
| `src/server/SampleServiceImpl.cc` | 67–87 | `SampleId`, `CheckoutEventId` |
| `src/server/ItemTypeServiceImpl.cc` | 45–65 | `ItemTypeId`, `CustomFieldDefinitionId` |
| `src/server/BoxServiceImpl.cc` | 49–69 | `StorageContainerId`, `FreezerId`, `ContainerTypeId`, `BoxTypeId`, `BoxId` |
| `src/server/LabServiceImpl.cc` | 47–67 | `LabId`, `UserId` |

These are called for every `Create*` RPC. On platforms where `std::random_device` degrades to a deterministic PRNG, entity IDs become predictable — enabling enumeration and forgery. The copy-paste itself is tracked as F-7 from `CODE_REVIEW_2026-06-12`.

**Fix:** Delete the four local `generate_uuid_v4()` definitions. Replace all call sites with `core::generate_uuid_v4()`. This also partially addresses F-7 (duplicated helpers).

---

## 🟡 Risk / Medium

### F-3 (NEW) — `src/crypto/FileCipher.cc:53-54,111`: TOCTOU between hash and encrypt passes

`encrypt_file` opens `in_path` twice: once at L109 for SHA-256 hashing, again at L111 for streaming encryption. Between these two opens, the temp file (a hot-copied SQLite database) could theoretically be replaced by a concurrent process holding the same path. The manifest would record the hash of the *first* file but encrypt the *second* — the content-hash check on decrypt would then fail, but this turns a valid backup into an unrecoverable one.

In practice the temp file is under `out_path + ".hotcopy.tmp"` and no other process should touch it. **Fix:** Add a comment noting the assumption; consider hashing from the already-open stream in a single pass, or re-hashing the encrypted file's plaintext after writing.

### F-4 (NEW) — `src/backup/BackupCommands.cc:148-162`: Serializable transaction for audit-only append on live backend

`append_backup_event` uses `IsolationLevel::Serializable` to append a `backup.create` / `backup.prune` / `backup.drill` event to the *live production backend*. Under heavy concurrent write load, this serializable transaction can conflict and throw, causing the backup audit record to be lost (the backup file itself is unaffected). Audit-only appends do not need serializable isolation.

**Fix:** Use `ReadCommitted` (or a dedicated "append only" isolation level if the backend supports it). Catch and log audit-append failures without aborting the backup operation.

### F-5 (NEW) — `src/backup/RetentionPolicy.cc:27`: `gmtime_r` return value unchecked

`::gmtime_r(&seconds, &tm_utc)` can return `nullptr` for timestamps that cannot be represented as calendar time (negative epoch, year > 9999 on some platforms). `tm_utc` would remain uninitialized, and subsequent reads are UB. In practice, `created_micros` is always non-negative and within range for a backup system, but defense-in-depth argues for a check.

**Fix:** Check the return value; if null, treat the file as un-parsable (return the same path as an out-of-range timestamp would → retained, not pruned).

### F-6 (from review-m5-phi-encryption.md, UNFIXED) — `src/kms/KmsFactory.cc:21`: Double `getenv("CREDENTIALS_DIRECTORY")`

`make_kms()` reads `CREDENTIALS_DIRECTORY` at L24 and confirms the credential file exists, then delegates to `OsKeyringKms::from_systemd_credentials()` which reads `CREDENTIALS_DIRECTORY` again. Redundant; call `from_credentials_dir(creds_dir)` directly with the already-resolved path.

### F-7 (from review-m5-phi-encryption.md, UNFIXED) — `src/cli/KeyCommands.cc:48-88`: Long-lived Serializable transaction in key rotation

`rotate_lab` opens a `Serializable` transaction, loads all samples for the lab (including tombstoned), and re-wraps each one inside the transaction. For labs with millions of samples, this is a very long-lived transaction that will conflict with live writes. Batching (e.g., 100 samples per transaction) is the intended scaling path.

### F-8 (M-1 from security-audit-2026-06-13, UNFIXED) — `src/cli/CsvReader`: No input size limits

CSV parser reads the entire stream with no cap on rows, fields, or field lengths. A maliciously crafted CSV can exhaust memory. Add configurable limits (`max_field_length`, `max_fields_per_row`, `max_rows`).

### F-9 (M-2 from security-audit-2026-06-13, UNFIXED) — `src/core/custom_field_validator`: No field-count limit

No cap on the number of custom field definitions per entity. A lab admin could define thousands of fields, degrading every sample create/update. Add `max_custom_fields_per_entity` (suggest 200) and enforce at definition time + in the validator.

### F-10 (M-3 from security-audit-2026-06-13, UNFIXED) — SQLite database path unsanitized

`BackendOptions::sqlite_path` from CLI flags is passed directly to `sqlite3_open_v2()` with no `..` traversal check or canonicalization against a `data_dir`. Resolve with `std::filesystem::weakly_canonical` and verify it's under the data directory.

### F-11 (M-5 from security-audit-2026-06-13, UNFIXED) — `token_prefix_len` defaults to 16

Default remains 16 hex chars (64 bits). Reducing to 8 (4 bytes) creates modest prefix collisions (~1 at ~65k sessions), making offline hash-cracking harder after a DB leak, and shrinks the index. The hash comparison is the authoritative check.

### F-12 (L-2 from security-audit-2026-06-13, UNFIXED) — Postgres connection string stores password as plaintext

`PostgresBackendOptions::connection_string` includes the password as a plain `std::string`, which may appear in core dumps, debug logs, or process listings. Accept password via a separate env var (`FMGR_PG_PASSWORD`) or file path; never store it in the options struct.

### F-13 (L-5 from security-audit-2026-06-13, UNFIXED) — Missing `_GLIBCXX_ASSERTIONS` in sanitizer presets

ASAN/UBSAN presets enable sanitizers but don't enable `-D_GLIBCXX_ASSERTIONS`, which adds bounds-checking in `std::vector::operator[]` and other STL hardening. Add `add_compile_definitions(_GLIBCXX_ASSERTIONS)` to the ASAN preset.

---

## 🔵 Nit / Low

### N-1 (NEW) — `src/server/SampleServiceImpl.cc:226`: JSON parse error → INTERNAL instead of INVALID_ARGUMENT

`prepare_custom_fields` calls `nlohmann::json::parse(incoming_json)` inside the RPC handler's try block. A `json::parse_error` is caught by `catch (...)` → `current_exception_to_grpc_status()`, which maps unknown exceptions to `INTERNAL`. Wrap in a `try/catch` that translates `json::parse_error` → `ConstraintViolation` → `INVALID_ARGUMENT`. Same pattern applies to any gRPC handler that parses client-supplied JSON.

### N-2 (from review-m5-phi-encryption.md, UNFIXED) — `ensure_sodium()` duplicated 4×

Now duplicated across `FieldCipher.cc`, `FileCipher.cc`, `KeyringKms.cc`, and `EnvVarKms.cc` (one more than the prior review noted). Extract to a shared utility header.

### N-3 (from review-m5-phi-encryption.md, UNFIXED) — `rewrap` error message in key rotation unclear

When `KeyringKms::unwrap_dek` throws because the old KEK is missing from the keyring, the log says "cannot re-wrap" but doesn't suggest checking retired key files (`master_kek.prev.*`). The operator may not realize the old KEK was prematurely removed.

### N-4 (F-6 from CODE_REVIEW_2026-06-12, UNFIXED) — Unbounded audit reads

`VerifyAuditChain` and `ExportAuditLog` load every audit row into memory. Acceptable for current scale; tracked as a TODO for a streaming path. Unchanged in M5.

### N-5 (F-7 from CODE_REVIEW_2026-06-12, PARTIALLY UNFIXED) — Duplicated service-local helpers

`generate_uuid_v4`, `now_timestamp`, `make_ctx` are copy-pasted across 4+ service translation units. The `generate_uuid_v4` copies are now a security concern (see F-2). Candidate for `server/ServiceUtil.h`. F-2's fix (delete local copies, use `core::generate_uuid_v4()`) addresses the UUID part of this.

---

## ✅ Confirmed Resolved (from prior reviews)

| ID | Source | Finding | Resolution |
|----|--------|---------|------------|
| C-2 | security-audit | Constant-time token hash comparison | `sodium_memcmp` in `verify_token_hash()` |
| H-1 | security-audit | No rate limiting | `RateLimiter` in `AuthServiceImpl::Login` |
| H-2 | security-audit | No TLS enforcement guard | `require_tls` + `FMGR_REQUIRE_TLS` env var |
| H-3 | security-audit | `std::cerr` unbuffered | Replaced with `spdlog` |
| M-4 | security-audit | SQLite WAL mode not enabled | `PRAGMA journal_mode = WAL` in constructor |
| L-1 | security-audit | Fragile snprintf in UUID | `core::generate_uuid_v4()` uses `randombytes_buf` |
| L-3 | security-audit | KMS entirely stubbed | Full KMS subsystem implemented (M5) |
| L-4 | security-audit | Missing AuditCommands implementation | `AuditCommands.cc` implemented |
| — | CODE_REVIEW | F-1 (privilege escalation) | Fixed |
| — | CODE_REVIEW | F-2 (share separation-of-duties) | Fixed |
| — | CODE_REVIEW | F-3 (include_decided mislabel) | Fixed |
| — | CODE_REVIEW | F-4 (page-token parse) | Fixed |
| — | CODE_REVIEW | F-5 (self-share guard) | Fixed |
| — | review-m5 | PHI never exposed during rewrap | Verified — `rewrap` copies ciphertext verbatim |
| — | review-m5 | DEK zeroing on all paths | Verified — `sodium_memzero` after encrypt/decrypt/rewrap |
| — | review-m5 | Indexed-PHI rejection at create+update | Verified — `reject_indexed_phi` on both paths |

---

## Summary Table

| ID | Severity | Status | Source |
|----|----------|--------|--------|
| F-1 | 🔴 bug | **NEW** — Unbounded `getline` in `decrypt_file` | This review |
| F-2 | 🔴 bug | **PARTIAL** — 4 services still use `std::random_device` | C-1 from security-audit |
| F-3 | 🟡 risk | **NEW** — TOCTOU between hash/encrypt passes | This review |
| F-4 | 🟡 risk | **NEW** — Serializable tx for audit-only append | This review |
| F-5 | 🟡 risk | **NEW** — `gmtime_r` return unchecked | This review |
| F-6 | 🟡 risk | Unfixed — double `getenv` in KmsFactory | review-m5 |
| F-7 | 🟡 risk | Unfixed — long Serializable tx in rotation | review-m5 |
| F-8 | 🟡 risk | Unfixed — CSV no size limits | M-1 |
| F-9 | 🟡 risk | Unfixed — no custom-field count cap | M-2 |
| F-10 | 🟡 risk | Unfixed — SQLite path unsanitized | M-3 |
| F-11 | 🟡 risk | Unfixed — token_prefix_len default 16 | M-5 |
| F-12 | 🟡 risk | Unfixed — PG password in connection string | L-2 |
| F-13 | 🟡 risk | Unfixed — missing `_GLIBCXX_ASSERTIONS` | L-5 |
| N-1 | 🔵 nit | **NEW** — JSON parse error → INTERNAL | This review |
| N-2 | 🔵 nit | Unfixed — `ensure_sodium` duplicated 4× | review-m5 |
| N-3 | 🔵 nit | Unfixed — rotation error msg unclear | review-m5 |
| N-4 | 🔵 nit | Unfixed — unbounded audit reads | F-6 |
| N-5 | 🔵 nit | **PARTIAL** — duplicated helpers | F-7 |

**17 items carried forward, 4 new from this review, 13 from prior reviews.**

---

## Verdict

**Architecture**: Sound. Envelope encryption, key separation, audit gating, and rotation design are correct.

**Implementation**: Good for pre-alpha. The cryptography is correctly applied and well-tested. The backup subsystem is thorough — GFS retention, restore drill, scheduler, temp-file cleanup — all paths look correct.

**Before production**: Fix F-1 (unbounded getline — remote DoS) and F-2 (finish the `std::random_device` → `randombytes_buf` migration for all service ID generation). The 🟡 items are hardening for beta/pre-production.

**Cryptography**: No concerns. AEAD everywhere, fresh DEKs per operation, `sodium_memzero` on all DEK paths, key separation tested via `BackupKmsIsDistinctFromMasterAndCannotCrossUnwrap`. 👍

---

## Resolution (2026-06-19)

All actionable findings addressed in this pass. Build clean; full suite green except
the documented pre-existing baseline (SqliteBackend file-detection, CustomFieldResolver,
E2E unauth). clang-format / clang-tidy / SPDX / `git diff --check` clean on the diff.

| ID | Resolution |
|----|------------|
| F-1 | `decrypt_file` reads the manifest header via a bounded `read_manifest_line` capped at 4096 bytes; throws `CipherError` when exceeded. Test: `FileCipherTest.OversizedManifestHeaderRejected`. |
| F-2 / N-5 | Deleted the four service-local `generate_uuid_v4` copies; all now `using core::generate_uuid_v4` (libsodium CSPRNG). |
| F-3 | `encrypt_file` documents the single-owner temp-file assumption between the hash and encrypt passes. |
| F-4 | `append_backup_event` uses `ReadCommitted` and is best-effort: a failed append is logged to the caller's stream, never aborts the backup. |
| F-5 | `bucket_keys` checks `gmtime_r`; an unrepresentable timestamp retains the file instead of reading an uninitialised `tm`. |
| F-6 | `make_kms` calls `from_credentials_dir(creds_path, basename)` with the already-resolved directory (no second `getenv`). |
| F-7 | `rotate_lab` enumerates IDs in a short read txn, then re-wraps in bounded 100-row Serializable batches, re-reading each row in its batch. |
| F-8 | `parse_csv` takes `CsvLimits` (max field length / fields-per-row / rows); throws `CsvParseError` on breach. Tests in `CsvReaderTest`. |
| F-9 | `k_max_custom_fields_per_entity = 200` enforced in `validate_custom_fields` and at definition time in `CreateCustomFieldDefinition`. |
| F-10 | `open_backend` rejects a `sqlite_path` escaping `BackendOptions::data_dir` (via `weakly_canonical` + `lexically_relative`). Test: `BackendFactoryTest.RejectsSqlitePathEscapingDataDir`. |
| F-11 | `token_prefix_len` default reduced 16 → 8 hex chars. |
| F-12 | `open_backend` bridges `FMGR_PG_PASSWORD` to libpq's `PGPASSWORD`; `BackendOptions` documents not embedding the password in the URL. |
| F-13 | `_GLIBCXX_ASSERTIONS` added to the ASAN and UBSAN builds. |
| N-1 | `current_exception_to_grpc_status` maps `json::parse_error` → `INVALID_ARGUMENT` with a generic message (no PHI echo). Test in `ErrorTranslation`. |
| N-2 | Shared `core::sodium_ready()` (`core/sodium_init.h`); the four module wrappers delegate to it. |
| N-3 | Key-rotation re-wrap failure message points the operator at the retired `master_kek.prev.*` files. |
| N-4 | Unchanged — acknowledged TODO (streaming audit reads), out of scope. |
