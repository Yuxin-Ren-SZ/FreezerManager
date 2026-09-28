# Handoff note — 2026-06-01, E1 IAuthProvider interface + E2 LocalAuthProvider

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
