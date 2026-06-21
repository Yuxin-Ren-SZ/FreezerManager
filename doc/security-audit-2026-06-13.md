# FreezerManager Security Audit

**Date**: 2026-06-13
**Revision**: `HEAD` (post hardening pass of 2026-06-12)
**Scope**: Full codebase — 140+ source files across `src/`, `tests/`, `tools/`, `.github/`
**Previous audit**: 2026-06-12 review (now retired) — all 12 original findings resolved or deferred

---

## Executive Summary

The codebase is in good shape for pre-alpha. The previous audit's high-severity items (error-message leak, lockout-map growth, missing retry logic) have been verified as resolved. This fresh audit surfaces 15 new findings, most of them hardening items that should be addressed before production deployment. The architecture — parameterized SQL queries, SHA-256 hash-chained audit log, BLAKE2b token hashing, `authz_version` permission-cache invalidation, and the three-signature share-approval scheme — remains sound.

---

## Findings

### 🔴 Critical

#### C-1: `generate_uuid_v4()` uses `std::random_device` — potentially deterministic on some platforms

| Field | Detail |
|---|---|
| **File** | `src/core/uuid.h:118-137` |
| **Risk** | On MinGW, older Linux kernels (<4.8 with broken RDRAND), and certain embedded targets, `std::random_device` falls back to a deterministic PRNG. An attacker who can predict the next UUID can forge request IDs, enumerate entity IDs, or craft collisions. |
| **Impact** | Predictable entity IDs undermine the unguessability premise of UUID-based resource identifiers. `request_id` forging could corrupt audit log correlation. |
| **Exploitability** | Platform-dependent. A server deployed on a platform with weak `std::random_device` leaks entity-creation ordering and potentially allows ID enumeration. |
| **Fix** | Replace `std::random_device` with `libsodium`'s `randombytes_buf()`. Libsodium is already a declared dependency in `conanfile.txt` (used elsewhere for token generation). This gives OS-provided randomness (getrandom/getentropy on Linux, `RtlGenRandom` on Windows) with no fallback-to-weak-PRNG path. |

**Recommended code change** in `src/core/uuid.h`:

```cpp
// Replace:
#include <random>    // remove

// Add:
#include <sodium.h>  // add

[[nodiscard]] inline std::string generate_uuid_v4() {
    std::array<std::uint8_t, 16> bytes{};
    randombytes_buf(bytes.data(), bytes.size());
    bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0FU) | 0x40U);
    bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3FU) | 0x80U);
    std::array<char, 37> buf{};
    std::snprintf(buf.data(), buf.size(),
                  "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                  bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5],
                  bytes[6], bytes[7], bytes[8], bytes[9], bytes[10], bytes[11],
                  bytes[12], bytes[13], bytes[14], bytes[15]);
    return {buf.data()};
}
```

> Also remove the now-unused `#include <random>`.

#### C-2: No constant-time comparison for BLAKE2b token hash verification

| Field | Detail |
|---|---|
| **File** | `src/auth/LocalAuthProvider.h:172-173` (declaration); implementation in `LocalAuthProvider.cc` |
| **Risk** | `verify_token_hash()` compares the BLAKE2b-256 hash of the submitted bearer token against the stored hash. If this comparison uses `operator==` on `std::string`, it exits early on the first differing byte. An attacker who can measure response-time differences with microsecond precision may iteratively brute-force the hash by observing timing. |
| **Impact** | Timing side channel on session token and API token validation paths. An attacker who captures a `token_prefix` (from logs, memory dumps, or network traces) and times thousands of validation attempts could walk the hash byte-by-byte. |
| **Exploitability** | Low in practice (requires network-level timing fidelity), but this is a well-understood cryptographic hygiene requirement. |
| **Fix** | Use `sodium_memcmp()` (already available via `libsodium`) or `crypto_verify_32()` for the 32-byte hash comparison: |

```cpp
[[nodiscard]] inline bool verify_token_hash(std::string_view token,
                                            std::string_view stored_hash) const {
    const auto computed = hash_token(token);
    if (computed.size() != stored_hash.size()) return false;
    return sodium_memcmp(computed.data(), stored_hash.data(), computed.size()) == 0;
}
```

---

### 🔴 High

#### H-1: No network-layer rate limiting — credential spray across distinct emails beats per-email lockout

| Field | Detail |
|---|---|
| **File** | `src/server/AuthServiceImpl.h`, `src/auth/LocalAuthProvider.h` |
| **Risk** | The per-email lockout (`max_failures_before_lockout=5`) and the lockout map cap (`max_lockout_entries=10000`) prevent memory exhaustion from a spray, but do nothing to throttle the sheer volume of login attempts. An attacker can submit one attempt each for 10,000 distinct emails per lockout-eviction cycle, probing for valid accounts indefinitely. |
| **Impact** | Account enumeration (response timing differs between "unknown user" and "wrong password"), sustained load on Argon2id hashing, and log noise. |
| **Fix** | Add a connection-level or IP-level rate limiter at the gRPC interceptor layer. A simple token-bucket or sliding-window limiter that rejects excess `Login` RPCs per (IP, time_window) with `RESOURCE_EXHAUSTED` is sufficient. The `spdlog` library (already in `conanfile.txt`) can log rate-limit events. |

#### H-2: No production TLS enforcement guard

| Field | Detail |
|---|---|
| **File** | `src/server/FreezerServer.h:28-29` |
| **Risk** | `FreezerServerOptions::tls_cert_path` and `tls_key_path` are `std::string` fields defaulting to empty. An empty string means "start without TLS (dev mode only)" per the comment. There is no runtime flag, environment variable check, or assertion that prevents a production build from starting with empty TLS paths. |
| **Impact** | A misconfiguration in production (missing CLI flags, wrong config file) silently starts a plaintext gRPC server. Bearer tokens and PHI travel in the clear. |
| **Fix** | Add a `bool require_tls{false}` field to `FreezerServerOptions`. In production presets (`release`, `release-deterministic`), set it to `true`. During `build()`, if `require_tls && tls_cert_path.empty()`, throw `std::invalid_argument`. Also consider reading these paths from `FMGR_TLS_CERT` / `FMGR_TLS_KEY` environment variables as a fallback. |

#### H-3: `std::cerr` is unbuffered — error-logging path under gRPC error handler is a DoS vector

| Field | Detail |
|---|---|
| **File** | `src/server/GrpcErrorTranslation.h:97,100` |
| **Risk** | `current_exception_to_grpc_status()` writes the real `e.what()` to `std::cerr` before returning a generic error to the client. `std::cerr` is unbuffered by default — each `<<` triggers a `write()` syscall. If an attacker triggers a high rate of internal errors (e.g. malformed protobufs, constraint violations), the server thread pool serializes on stderr writes. |
| **Impact** | Denial of service via error amplification. Each request can trigger an unbuffered write; at high concurrency, the syscall serialization degrades all RPC throughput. |
| **Fix** | Replace `std::cerr` with `spdlog` (already a declared dependency, unused so far). Use the async logger for the gRPC error handler so log writes never block the RPC thread pool: |

```cpp
// In GrpcErrorTranslation.h:
#include <spdlog/spdlog.h>

// Replace std::cerr lines with:
spdlog::error("grpc: unhandled internal error: {}", e.what());
spdlog::error("grpc: unhandled non-std exception");
```

Consider a `spdlog::async_logger` with a bounded queue to prevent memory buildup under sustained attack.

---

### 🟡 Medium

#### M-1: CSV parser has no input size limit — memory-exhaustion DoS

| Field | Detail |
|---|---|
| **File** | `src/cli/CsvReader.h:30`, implementation in `CsvReader.cc` |
| **Risk** | `parse_csv(std::istream& in)` reads the entire stream into memory with no size cap on rows, fields, or individual field lengths. A maliciously crafted CSV (e.g. a single line with a 4 GB quoted field) exhausts server memory. |
| **Impact** | DoS on the CLI import path (`freezerctl sample import`). The gRPC `SampleService` CSV endpoints (if they reuse this parser) would be remotely exploitable. |
| **Fix** | Add configurable limits: `max_field_length` (default 64 KB), `max_fields_per_row` (default 200), `max_rows` (default 1,000,000). Enforce them during parse; throw `CsvParseError` with a descriptive message when exceeded. |

#### M-2: Custom field validator has no field-count limit

| Field | Detail |
|---|---|
| **File** | `src/core/custom_field_validator.h:225-233` |
| **Risk** | `validate_custom_fields()` iterates all definitions with no cap. A lab admin could define thousands of custom fields on an ItemType, causing every sample create/update to spend inordinate time in validation (and every audit-event serialize/deserialize to carry massive JSON). |
| **Impact** | Degraded RPC performance; large audit-event JSON bloat; potential CPU-exhaustion DoS. |
| **Fix** | Add a `max_custom_fields_per_entity` constant (suggest 200). Reject ItemType creation/update when the resolved field count exceeds this. Also validate in `validate_custom_fields()` as a defense-in-depth check. |

#### M-3: SQLite database path is unsanitized — path traversal risk

| Field | Detail |
|---|---|
| **File** | `src/cli/BackendFactory.h:21` (`sqlite_path`), `src/storage/sqlite/SqliteBackend.h:28` (`database_path`) |
| **Risk** | `BackendOptions::sqlite_path` is taken directly from CLI flags and passed as `SqliteBackendOptions::database_path` to SQLite's `sqlite3_open_v2()`. A path containing `../` traverses outside the expected data directory. While SQLite won't execute shell commands via `sqlite3_open`, a crafted path like `/etc/passwd` or `/proc/self/environ` could expose host information in error messages. |
| **Impact** | Information disclosure via error messages, unintended file creation outside the data directory, potential symlink attacks. |
| **Fix** | Resolve the path against a configurable `data_dir` with `std::filesystem::canonical()`, then verify the result starts with the data directory. Reject any path containing `..` components before resolution. |

```cpp
// In BackendFactory or open_backend:
std::filesystem::path resolved = data_dir / sqlite_path;
resolved = std::filesystem::weakly_canonical(resolved);
// Verify `resolved` is under `data_dir` — reject otherwise.
```

#### M-4: SQLite backend doesn't enable WAL mode by default

| Field | Detail |
|---|---|
| **File** | `src/storage/sqlite/SqliteBackend.h:27-31` |
| **Risk** | The SQLite backend doesn't configure journal mode. The default is DELETE mode, which serializes all writes and causes concurrent reads to see "database is locked" errors. This increases serialization failures and degrades performance under concurrent access. |
| **Impact** | Unnecessary transaction conflicts; reduced concurrent reader throughput; potential for data loss if the process crashes mid-write with DELETE journal mode. |
| **Fix** | Execute `PRAGMA journal_mode=WAL;` immediately after opening the database in the backend constructor. Also set `PRAGMA synchronous=NORMAL;` (safe in WAL mode) and `PRAGMA foreign_keys=ON;`. |

#### M-5: Token prefix length default (16 hex chars) narrows lookup index

| Field | Detail |
|---|---|
| **File** | `src/auth/LocalAuthProvider.h:59` |
| **Risk** | `token_prefix_len` defaults to 16 hex characters (8 bytes / 64 bits). The prefix is stored plainly in the DB and is the indexed lookup key. While 64 bits is too large to brute-force remotely, an attacker who can query the sessions table (e.g. via a compromised read-only DB credential or SQL injection) can enumerate all valid prefixes with a single `SELECT DISTINCT token_prefix FROM sessions` query. The prefix itself is not enough to authenticate (the full 128 hex chars + hash are needed), so this is a defense-in-depth concern. |
| **Impact** | If the sessions table is ever leaked, the prefix length determines how efficiently an attacker can partition the hash-cracking workload. With 16-char prefixes, every session maps to a unique prefix, making offline cracking trivially parallelizable. A shorter 8-char prefix (4 bytes) would cause collisions, forcing an attacker to try multiple hashes per prefix. |
| **Fix** | Reduce `token_prefix_len` default to 8 (4 bytes). This creates modest prefix collisions (expect ~1 collision at ~65k sessions), which is fine — the hash comparison is the authoritative check. Fewer unique prefixes in the DB also makes the index smaller and faster. |

---

### 🔵 Low / Informational

#### L-1: Fragile `snprintf` in UUID generation

| Field | Detail |
|---|---|
| **File** | `src/core/uuid.h:132-135` |
| **Risk** | `std::snprintf(buf.data(), buf.size(), ...)` — the buffer is exactly 37 chars. The format string produces 36 chars + 1 null = 37. If the format string ever changes (e.g. a separator added), this would silently truncate. |
| **Fix** | Assert at compile time or use `std::array<char, 37>` combined with `static_assert` on the format length. Better: use a string stream or `fmt::format` (already in `conanfile.txt`). This is fixed as a side effect of C-1 if the recommended libsodium change is adopted. |

#### L-2: Postgres connection string stored as plain text in configuration

| Field | Detail |
|---|---|
| **File** | `src/storage/postgres/PostgresBackend.h:28` |
| **Risk** | `PostgresBackendOptions::connection_string` is a plain `std::string` that includes the database password. This string may appear in core dumps, debug logs, or process listings. |
| **Fix** | Accept the password via a separate environment variable (`FMGR_PG_PASSWORD`) or a file path. Read it only at connection time; never store it in the options struct. Libpqxx supports `PGPASSWORD` natively. |

#### L-3: PHI encryption key management is entirely stubbed (`src/kms/`)

| Field | Detail |
|---|---|
| **File** | `src/kms/CMakeLists.txt` (empty directory) |
| **Risk** | The `phi_fields_enc_json` column exists on the Sample entity, but no KMS subsystem generates, stores, rotates, or unwraps the per-field AEAD encryption keys. Any PHI written to this column today would be stored as effectively plaintext JSON. |
| **Fix** | Already noted as deferred from the previous audit. Gate PHI toggle (`LabEnablePhi`) behind a runtime check that a KMS backend is configured; reject PHI writes otherwise. |

#### L-4: `AuditCommands.h` header exists but no CLI audit command implementation

| File | `src/cli/AuditCommands.h` | placeholder header — no implementation |
|---|---|---|
| **Fix** | Deferred. Audit commands (chain verification, export) are important for compliance; schedule for release milestone. |

#### L-5: Build configuration for sanitizers doesn't include `-D_GLIBCXX_ASSERTIONS`

| File | `CMakeLists.txt:78-91` |
|---|---|
| **Risk** | The asan/ubsan presets enable sanitizers at compile+link time but don't enable `-D_GLIBCXX_ASSERTIONS`, which enables bounds-checking in `std::vector::operator[]` and other STL hardening. |
| **Fix** | Add `add_compile_definitions(_GLIBCXX_ASSERTIONS)` to the ASAN preset. |

---

## Confirmed Safe (Re-verified)

The following aspects from the previous audit and general review were re-verified as sound:

| Area | Verification |
|---|---|
| **SQL injection** | All queries use parameterized placeholders (`?` / `$N`) through `QuerySqlBuilder`. Column names come from compile-time enum-to-string switches — never from user input. |
| **Audit hash chain** | `CanonicalJson` + `compute_audit_hash` produce deterministic SHA-256 chain. `AuditChainVerifier` is pure and DB-free. Backend trigger layer rejects UPDATE/DELETE on audit rows. `this_hash UNIQUE` prevents chain forks. |
| **Token storage** | Only BLAKE2b-256 hashes stored in DB; plaintext token shown once at `authenticate()` response. `token_prefix` is purely an index aid, not a secret (explicitly documented). |
| **Auth cache invalidation** | `authz_version` epoch on `User` bumped in-transaction whenever permissions change. `CachedContext` validated against fresh `authz_version` — stale entries evicted immediately. |
| **MFA gate** | `AuthMiddleware::authorize()` checks `mfa_complete` before granting access. No bypass path exists. |
| **Audit snapshot integrity** | `AuditSnapshot` (before/after entity JSON) is derived by repositories from their own authoritative state. Callers cannot forge audit content — they supply only `MutationContext` (who, why). |
| **Cross-lab share approval** | Three-signature scheme: SourceAdmin, TargetAdmin, SystemAdmin each sign separately. `user_already_signed()` prevents double-signing by the same user. `ShareServiceImpl` rejects self-sharing (source == target). |
| **Error-message leak (prior C-1)** | Fixed: `current_exception_to_grpc_status()` returns generic `"internal server error"` for unknown exceptions. |
| **Lockout-map growth (prior C-2)** | Fixed: `max_lockout_entries` cap + `evict_stale_lockouts()` sweep. |
| **Serialization failure (prior C-3)** | Fixed: `SerializationFailure → ABORTED` in translation funnel. |
| **Password length (prior M-7)** | Fixed: `min_password_length` default 8 enforced in `hash_password()`. |
| **Scope JSON validation (prior M-5)** | Fixed: `validate_scope_json()` rejects malformed/non-array scopes and unknown permission keys. |
| **Share scope validation (prior M-6)** | Fixed: `CreateShareRequest` rejects non-object scope_json. |
| **Role escalation (prior F-1)** | Fixed: `SystemAdmin` kind rejected for custom roles; global-only permission rejects for lab roles. |
| **Share separation-of-duties (prior F-2)** | Fixed: one user cannot sign multiple approver roles. |
| **Page token parsing (prior F-4)** | Fixed: malformed page tokens return INVALID_ARGUMENT. |

---

## Dependency Security

| Dependency | Version | Notes |
|---|---|---|
| `libsodium` | 1.0.21 | Current. Provides Argon2id, BLAKE2b, randombytes, constant-time comparison. |
| `openssl` | 3.6.2 | Current LTS. Used by gRPC for TLS. |
| `grpc` | 1.78.1 | Recent. |
| `spdlog` | 1.17.0 | **Unused** — declared in `conanfile.txt` but not yet integrated. Recommended for structured logging (see H-3). |
| `nlohmann_json` | 3.12.0 | Current. Key-sorted `std::map` (deterministic serialization verified). |
| `sqlite3` | 3.51.3 | Current. |
| `libpqxx` | 8.0.1 | Current. |

---

## Verdict

**Architecture**: Sound. The RBAC + MFA + audit-chain design provides defense-in-depth appropriate for a biospecimen management system that may handle PHI.

**Implementation**: Good for pre-alpha. The previous audit's 12 findings are all resolved or deferred. This audit surfaces 15 new findings:

- **2 Critical**: Replace `std::random_device` with `libsodium` for UUID generation; add constant-time hash comparison for token verification.
- **3 High**: Add network-layer rate limiting, TLS enforcement guard in production presets, replace `std::cerr` with `spdlog` for error logging.
- **5 Medium**: CSV size limits, custom-field count limits, SQLite path sanitation, WAL mode default, token prefix length reduction.
- **5 Low/Info**: Fragile snprintf, plaintext PG connection string, KMS stub, missing audit CLI commands, missing `_GLIBCXX_ASSERTIONS` in sanitizer presets.

**Recommendation**: Fix the two critical items before any deployment. Address the three high items before the first production deployment. Medium items are hardening for the beta milestone. Low items are polish.
