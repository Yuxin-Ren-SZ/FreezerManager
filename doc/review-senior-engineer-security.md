# Code Security Review: Senior Software Engineer Perspective

**Reviewer persona:** Senior software engineer with 15+ years in security-sensitive
systems (auth, crypto, storage, network protocols).  I read every line of
`src/auth/`, `src/kms/`, `src/crypto/`, `src/rpc/AuthMiddleware.cc`,
`src/server/SampleServiceImpl.cc`, `src/server/FreezerServer.cc`,
`src/storage/detail/QuerySqlBuilder.h`, `src/audit/`, and `src/cli/CsvReader.h`.
This review is based on the actual code at `feat/qt-csv-import-wizard`
(c17979c), not the PRD.

---

## Overall Assessment

This is not hobbyist auth code.  The author understands constant-time
comparison, Argon2id parameter selection, envelope encryption, per-record DEK
rotation, hash-chain audit with fork detection, and the difference between a
random UUID from `std::random_device` and one from `libsodium randombytes_buf`.
The code has been through at least one security audit pass — I can see the
scars: `security-audit-2026-06-12` and `review F-8` annotations in comments
marking issues that were found and fixed.

There are real concerns, and I'll detail them below.  But the baseline is
unusually strong for a pre-alpha project.  I would not say that about most
codebases I review.

---

## Auth Layer: `src/auth/LocalAuthProvider.cc` (872 lines)

### Strengths

**Argon2id parameter selection (`LocalAuthProvider.h:51-52`).** 64 MiB memory,
3 iterations.  This is the OWASP recommendation for password hashing.  The
config struct exposes `pwhash_memlimit` and `pwhash_opslimit` so tests can
reduce them (the comment at line 49 explicitly says "In tests, use memlimit =
crypto_pwhash_MEMLIMIT_MIN (8192) and opslimit = 1 to avoid multi-second
delays").  This is the correct pattern: hard defaults, tunable for tests, not
accidentally slow in production.

**Password verification (`LocalAuthProvider.cc:357`).** Uses
`crypto_pwhash_str_verify()` for constant-time comparison.  Not `==` on
strings, not `strcmp`.  Correct.

**Session token generation (`LocalAuthProvider.cc:665`).** 32 random bytes via
`randombytes_buf` → 64 hex chars → 256 bits of entropy.  This is not a JWT
with a weak signing key.  The token is stored as a BLAKE2b-256 hash; the
plaintext is shown exactly once and never persisted.  The comment at line 39 in
the header file explicitly says "Return plaintext token in AuthToken (shown
once, never stored)."

**Token hash comparison (`LocalAuthProvider.cc:685`).** Uses `sodium_memcmp`
for constant-time comparison of the computed hash against the stored hash.
This is the correct primitive — not `==` on hex strings, not `strncmp`.

**Account lockout (`LocalAuthProvider.cc:750-813`).** Five failures →
one-hour lockout.  The lockout map has a hard cap (`max_lockout_entries`,
default 10000) and an eviction strategy: expired locks and idle unlocked
entries are evicted automatically; if still over the cap, unlocked entries are
evicted first.  The comment at line 764 explicitly calls out "an attacker
spraying random emails cannot grow the map without bound" and references
`security-audit-2026-06-12 #2`.  This tells me the author has thought about
credential-spray DoS.

**Email normalization (`LocalAuthProvider.cc:319`).** Lowercases the email
before lookup AND before lockout tracking.  This prevents a case-sensitivity
bypass where `User@lab.edu` and `user@lab.edu` are treated as different
lockout buckets.

**API token scope enforcement (`LocalAuthProvider.cc:54-78`).**
`permissions_from_scope_json()` parses the scope with fail-closed semantics:
malformed JSON → empty set (zero permissions), unknown permission keys →
silently skipped (but `validate_scope_json()` at creation time rejects them
up front — see the comment at line 72).  The `["*"]` sentinel for unrestricted
is documented.  The intersection logic (`intersect_grants_with_scope`,
line 126) correctly restricts token permissions to a subset of the user's role
permissions, then further restricts to a single lab if `token.lab_id` is set.

**Permission cache with authz_version invalidation (`LocalAuthProvider.cc:527-562`).**
The cache check requires BOTH that the TTL hasn't expired AND that the cached
`authz_version` matches the user's current `authz_version`.  This means a
permission downgrade takes effect on the next request, not at the next cache
expiry — exactly the correct design.  The comment at line 534 is explicit:
"a permission change bumps the user's authz_version and forces a rebuild."

**MFA flag from DB, not cache (`LocalAuthProvider.cc:538`).** The cached
context stores permissions but `mfa_complete` always comes from the live
session row.  This means `verify_totp()` takes effect on the very next
request, not after cache expiry.  The header comment at line 135 confirms:
"MFA flag always comes from the DB session row, never from this cache."

### Concerns

**C-1: Lockout state is in-memory only (`LocalAuthProvider.cc:752`).**
`lockout_map_` is a `std::unordered_map` protected by a `std::mutex`.  It
resets on server restart.  A targeted attacker who can crash the server (via
resource exhaustion, unhandled exception, or OOM) can reset the lockout
counters.  The PRD acknowledges this ("in-memory, resets on restart") but does
not treat it as a gap.  For a single-server deployment with a small user base,
this is acceptable.  For a production deployment serving 50+ labs, an attacker
with a known email address could brute-force passwords by interleaving
credential attempts with server-crash attempts.

**Recommendation:** Document this as a known limitation.  For production,
consider persisting lockout state in the database (a `failed_login_attempts`
table with TTL-based cleanup) or using an external rate limiter (Redis,
fail2ban on the gRPC port).

**C-2: No built-in session token rotation.**  A session token is valid for up
to 7 days (absolute timeout).  If a token is exfiltrated (e.g., copied from a
`~/.netrc` file or an environment variable), there is no mechanism to detect
replay from a different IP or user-agent.  The `ClientInfo` struct captures IP
and user-agent at authentication time but does not appear to be checked during
`validate_token()`.

**Recommendation:** For v1, add optional IP-binding to sessions (configurable,
off by default for small labs behind NAT).  For v2, consider token rotation on
each `validate_token()` call (issue a new token, revoke the old one, return the
new token in a response header).

**C-3: TOTP secret stored in the User row (`LocalAuthProvider.cc:262`).**
The field is named `totp_secret_enc` ("enc"), but the current code treats it as
a plaintext base32 string — the secret is passed directly to `totp_verify()`.
The "enc" suffix suggests encryption is planned but not yet implemented.  A
database compromise that reads the User table would expose all TOTP seeds in
plaintext, defeating the second factor.

**Recommendation:** Encrypt `totp_secret_enc` under the master KEK before
storing it.  The FieldCipher infrastructure already exists and can be reused.

**C-4: `SoftDeleteSample` bypasses the middleware pattern (`SampleServiceImpl.cc:541`).**
Instead of calling `middleware_.authorize(bearer, Permission::SampleDeleteSoft, lab_id)`,
it calls `auth_.validate_token(bearer)` directly, then manually checks MFA and
permission.  The permission check (`line 552`) happens *after* loading the
sample, because the lab_id is only known at that point.  This is a legitimate
pattern when the target entity's lab isn't known in advance, but it means the
permission check is not registered in the static RPC registry and won't be
caught by the CI test that asserts every RPC is registered.

**Recommendation:** Register `SoftDeleteSample` with a wildcard permission
(e.g., `sample.delete_soft:*`) in the RPC registry, or add a two-phase
authorize pattern to the middleware: `authorize_entity(token, required_perm)`
that defers the lab-scope check to a callback.

---

## TOTP Implementation: `src/auth/Totp.cc` (168 lines)

### Strengths

**RFC compliance.** `hmac_sha1()` uses OpenSSL 3 `EVP_MAC` API (the comment at
line 23 explicitly avoids the deprecated `HMAC()` function).  Counter is 8
bytes big-endian (RFC 4226 §5.2).  Dynamic truncation follows RFC 4226 §5.3
exactly — offset from the last nibble of the HMAC, mask the high bit, extract 4
bytes.  The `totp_verify()` window is ±1 step (`config.window_steps`), which
matches the RFC 6238 recommendation.

**Counter underflow guard (`Totp.cc:158`).** `if (counter < 0) continue;`
prevents a wraparound bug when `base_counter + delta < 0` for negative deltas
near the epoch.

### Concerns

**C-5: No constant-time comparison on TOTP code (`Totp.cc:161`).**
`hotp(key, counter, config.digits) == code` is a string comparison.  For a
6-digit TOTP code, the timing difference is negligible in practice (the search
space is only 10^6, so the attacker can brute-force faster than they can
measure timing), but it's worth noting for completeness.  A
`sodium_memcmp`-based comparison would be more robust against future changes
that increase the code length.

---

## KMS & Cryptography: `src/kms/KeyringKms.cc`, `src/crypto/FieldCipher.cc`

### Strengths

**Envelope encryption with keyring (`KeyringKms.h:4-10`).**  Active KEK +
retired KEKs.  This is the correct design for key rotation: records encrypted
under an old KEK still decrypt during the rotation window.  The `unwrap_dek()`
method selects the KEK by `kek_id` from the envelope, so the rotation is
online and non-disruptive.  The comment at line 9: "records wrapped under a
now-retired KEK still decrypt while a rotation is in flight" — correct.

**DEK wrapping (`KeyringKms.cc:60-87`).** Uses `crypto_secretbox_easy`
(XSalsa20-Poly1305) with a fresh random nonce per wrap.  `unwrap_dek()` throws
`KmsError` on authentication failure — correct, no silent decryption of
tampered DEKs.

**Per-record DEK with zeroization (`FieldCipher.cc:88-98`).**  Fresh
`randombytes_buf` for each DEK, `sodium_memzero` after wrapping.  The DEK
lifetime is bounded to the `encrypt()` call.  Each PHI field is individually
sealed under the DEK with its own nonce — correct, no nonce reuse across
fields.

**Key rotation without plaintext exposure (`FieldCipher.cc:147-183`).**
`rewrap()` unwraps the DEK under the old KEK, re-wraps under the active KEK,
and updates `kek_id` in the envelope.  The field ciphertext is copied verbatim
(line 179: "Fields are sealed under the (unchanged) DEK — copy them
verbatim").  This means `rewrap()` never exposes plaintext PHI — it only
rotates the key wrapping.  Critically, `sodium_memzero(dek)` is called at line
177 after the re-wrap.  Correct.

**FileCipher with chunked streaming (`src/crypto/FileCipher.h`).**
Uses `crypto_secretstream` (chunked encryption with MAC per chunk + FINAL tag
on the last chunk).  Content hash (`content_sha256`) is verified on decrypt.
This prevents truncation attacks (an attacker who removes the last N chunks
cannot produce a valid FINAL tag).

### Concerns

**C-6: KeyringKms holds raw key bytes in std::vector (`KeyringKms.h:43`).**
`std::map<std::string, std::vector<std::uint8_t>> keks_` stores raw 32-byte
KEK material in heap memory with no guard pages, no mlock, no secure
allocation.  A core dump or a `/proc/<pid>/mem` read by a privileged attacker
would expose all KEKs.  libsodium provides `sodium_mlock()` and
`sodium_mprotect_noaccess()` for this purpose.

**Recommendation:** Wrap KEK bytes in a `SecureBuffer` class that calls
`sodium_mlock()` on allocation and `sodium_memzero()` + `sodium_munlock()` on
destruction.  Use `mprotect(PROT_NONE)` when the keyring is not actively in
use (between crypto operations).

**C-7: Canonical JSON is not RFC 8785 compliant (`CanonicalJson.cc:13-17`).**
`canonical_json()` uses `nlohmann::json::dump(-1, ' ', false, strict)`.  The
compact form with space separator is deterministic for the current version of
nlohmann, but RFC 8785 (JCS — JSON Canonicalization Scheme) specifies a much
stricter algorithm: no whitespace, keys sorted by UTF-16 code unit order, no
trailing commas, string escaping rules.  The current implementation relies on
`std::map` for key ordering (which is lexicographic, not UTF-16 code unit) and
nlohmann's dump() behavior.  If nlohmann changes its default float formatting
or string escaping in a future version, the canonical form changes, and the
audit chain breaks irrecoverably — every `this_hash` becomes invalid.

**Recommendation:** Before 1.0, either (a) pin the exact canonicalization
algorithm with a comprehensive test suite that covers edge cases (float
precision, Unicode escaping, null vs absent keys) and add a CI test that fails
if nlohmann changes output, or (b) implement RFC 8785 JCS.  Option (b) is
preferred because it's a published standard with test vectors.

---

## Query DSL & SQL Injection Surface: `src/storage/detail/QuerySqlBuilder.h`

### Strengths

**Parameterized queries only.**  Every value entering a SQL clause goes through
`dialect.placeholder()` (producing `?` or `$N`) and is appended to a
`std::vector<nlohmann::json> params` array.  At no point is a user-controlled
value interpolated into a SQL string.  The column names come from
`column_name(predicate.field)`, which is a compile-time mapping from
`Entity::Field` enums to hardcoded string literals — not user input.

**JSON path extraction is parameterized.**  Both the SQLite dialect
(`json_extract(col, ?) = ?`, line 77) and the PostgreSQL dialect
(`jsonb_extract_path_text(col, $N, $N) = $N`, line 110) bind path segments as
parameters.  No string concatenation of JSON path components.

**Dialect abstraction prevents leakage.**  The `SqlDialect` interface forces
every SQL fragment to go through `placeholder()`.  It's impossible to write a
new predicate type that injects raw SQL without modifying the dialect
interface, which would be visible in code review.

### Concerns

**C-8: Sort direction is string-interpolated (`QuerySqlBuilder.h:216`).**
`sql += sort.direction == SortDirection::Ascending ? " ASC" : " DESC";`.  This
is safe in practice because `sort.direction` is an enum, not user input.  But
it's the only place in the entire query builder where a non-literal string
touches the SQL.  A future refactor that makes the sort direction
user-configurable (e.g., a REST query parameter `?sort=name&order=asc`) could
introduce an injection vector here if the enum is replaced with a string.

**Recommendation:** Add a `static_assert` or a comment with a stern warning
that this is the only non-parameterized SQL fragment and must never accept
user-controlled strings.

---

## Server Layer: `src/server/SampleServiceImpl.cc`, `src/server/FreezerServer.cc`

### Strengths

**Authorization before parsing (`SampleServiceImpl.cc:649`).**  Every RPC
handler that takes a `lab_id` from the request calls
`middleware_.authorize(bearer, required_perm, lab_id)` *before* touching
request fields.  The comment at line 646 is explicit: "a missing/invalid token
must surface as UNAUTHENTICATED, not as a downstream INTERNAL from parsing an
(unvalidated) empty lab_id."

**CSV import: server-supplied identity (`SampleServiceImpl.cc:693`).**
`ImportContext.lab_id` and `ImportContext.actor` are set from the server's
authorized session context, never from the CSV content.  This means an
attacker cannot smuggle rows into another lab by crafting a CSV with a
different `lab_id` column.

**Dry-run isolation (`SampleServiceImpl.cc:720-727`).**  Each dry-run row is
inserted in its own `Serializable` transaction that is never committed — it
rolls back on scope exit.  The comment at line 726 is explicit: "Intentionally
not committed."

**RLS injection before repo use.**  Every handler that opens a transaction
immediately calls `AuthMiddleware::inject_rls_vars(*txn, sctx)` before any
repo operation.

**UUID generation from CSPRNG (`SampleServiceImpl.cc:68-71`).**
`using core::generate_uuid_v4` — the comment at line 69 explicitly warns that
`std::random_device` may degrade to a deterministic engine and cites
`security audit C-1 / review F-2`.  The implementation uses
`libsodium randombytes_buf`.  This is correct.

**TLS production guard (`FreezerServer.cc:50-57`).**  If `require_tls` is set
but no cert/key paths are configured, the server throws at startup before
binding any port.  The comment at line 52: "a misconfiguration aborts startup
loudly instead of serving tokens/PHI in the clear."  This is the correct
fail-closed pattern.

**Separate backup KEK (`FreezerServer.cc:90-95`).**  The backup scheduler
uses `make_backup_kms()`, not `make_default_kms()`.  If no backup KEK is
configured, backups are disabled with a warning — the server still starts.
Correct: backups are a feature, not a hard dependency.

### Concerns

**C-9: TLS is not implemented (`FreezerServer.cc:68`).**
`throw std::runtime_error("TLS not yet implemented; run with empty
tls_cert_path for dev mode");` — this is an explicit `throw`, not a TODO.
Every token and every sample record travels in plaintext over the network.
For a single-machine deployment (Qt client on localhost), this is acceptable.
For any deployment where the gRPC port is reachable from the network, this is
a hard blocker.

The PRD lists TLS as deferred to M5.  This review confirms it's not just
deferred — it's actively blocked with a runtime error.

**Recommendation:** This is the single highest-priority security gap.  It
should be implemented before any deployment where the server listens on a
non-loopback interface.

**C-10: No gRPC message size limit.**  The `ImportSamples` RPC accepts a
`csv_content` string field with no size limit.  A 1 GB CSV could exhaust
server memory.  The `CsvReader` has `CsvLimits` (1 MiB per field, 1024 fields
per row, 1M rows), but the gRPC layer has no inbound message size cap.

**Recommendation:** Set `grpc::ResourceQuota` or `SetMaxMessageSize()` on the
`ServerBuilder`.  A reasonable default is 10 MiB (the default gRPC limit is
4 MiB, which may be too tight for large CSV imports).  Make it configurable
via `FreezerServerOptions`.

**C-11: `current_exception_to_grpc_status()` may leak internal details.**
This function (in `GrpcErrorTranslation.h`) maps C++ exceptions to gRPC status
codes.  A `std::runtime_error("table 'samples' does not exist")` from a
database driver would be surfaced to the client as `INTERNAL` with the raw
message.  An attacker probing error messages could map the database schema.

**Recommendation:** In production mode, `INTERNAL` errors should return a
generic "internal server error" message and log the real error server-side.
Development mode can return detailed messages.

**C-12: No request ID propagation for tracing.**
`make_ctx(sctx, "import_samples")` sets `request_id = ""`.  There's no
correlation ID from the incoming gRPC metadata to the audit event.  If a
security incident requires tracing "which RPC call produced this audit row,"
the linkage is lost.

**Recommendation:** Extract or generate a request ID from gRPC metadata
(`x-request-id` header) and propagate it to `MutationContext::request_id`.

---

## CSV Parser: `src/cli/CsvReader.h`

### Strengths

**Resource limits (`CsvReader.h:31-35`).**  `CsvLimits` struct with
`max_field_length` (1 MiB), `max_fields_per_row` (1024), `max_rows` (1M).
The comment at line 28 cites `review F-8 / security-audit M-1`.  A
`CsvParseError` is thrown when limits are exceeded — the parser cannot be
forced into unbounded memory allocation.

**Comment-line skipping (`CsvReader.h:5-7`).**  Lines starting with `#` are
skipped.  This is documented as enabling round-trip compatibility with the
export format (which includes a header comment block).  It also prevents
comment injection attacks where a CSV cell value containing a newline followed
by `#` could be misinterpreted — but only if the quoting rules are violated
first.

---

## Summary of Findings

| ID | Severity | Area | Finding |
|----|----------|------|---------|
| C-9 | **Critical** | Server | TLS not implemented — all traffic in plaintext |
| C-1 | High | Auth | Lockout state in-memory, resets on restart |
| C-7 | High | Audit | Canonical JSON not RFC 8785 — nlohmann version dependency risks chain breakage |
| C-3 | Medium | Auth | TOTP secret stored as plaintext despite `_enc` naming |
| C-10 | Medium | Server | No gRPC message size limit — CSV import can exhaust memory |
| C-11 | Medium | Server | Exception-to-gRPC mapping may leak internal errors |
| C-12 | Low | Server | No request ID propagation to audit events |
| C-2 | Low | Auth | Session tokens not bound to client IP/user-agent |
| C-4 | Low | Auth | `SoftDeleteSample` bypasses RPC permission registry |
| C-6 | Low | KMS | Raw key bytes in heap without mlock/memzero guards |
| C-8 | Low | Storage | Sort direction is the only non-parameterized SQL fragment |
| C-5 | Info | Auth | TOTP code comparison is not constant-time |

---

## Verdict

The auth and crypto code shows real security engineering discipline: constant-time
comparison, parameterized queries, envelope encryption with per-record DEKs,
keyring-based rotation without plaintext exposure, hash-chain audit with fork
detection, and an explicit security audit trail in the code comments.

The concerns are real but fixable.  The critical gap is TLS (C-9) — this must
be resolved before any non-localhost deployment.  The high-severity items (C-1,
C-7) should be addressed before the first production tag.

For a pre-alpha project, this is an unusually strong security baseline.  I
would rather inherit this codebase and fix these 12 items than inherit a
commercial LIMS that claims "SOC 2 compliant" but has never had its auth code
read by a third party.
