# FreezerManager Security & Robustness Audit

**Date**: 2026-06-12
**Revision**: `f30a944` (merge `feat/rpc-role-service`)
**Tests**: 840 pass, 0 fail (Postgres skipped)
**Coverage**: Lines 86.9% / Functions 98.0% / Branches 44.2%

---

## Findings

### 🔴 High

| # | File | Line(s) | Problem | Fix |
|---|------|---------|---------|-----|
| 1 | `GrpcErrorTranslation.h` | 73-74 | `current_exception_to_grpc_status()` passes raw `e.what()` to gRPC status on unknown exceptions. DB error messages leak table/column names to clients. | Return generic `"internal server error"` in the `std::exception` catch arm; log real error server-side. |
| 2 | `LocalAuthProvider.cc` | 719-729 | `lockout_map_` entries never evicted on expiry. Attacker submitting random emails grows map unboundedly → OOM under sustained attack. | Sweep expired entries (`locked_until` in past) on insert, or cap map size with FIFO eviction. |
| 3 | Service handlers (`SampleServiceImpl`, `ShareServiceImpl`, `RoleServiceImpl`) | — | No retry on `SerializationFailure` (Postgres 40001). Returned as INTERNAL instead of ABORTED. Concurrent operations fail unnecessarily. | Catch `SerializationFailure` specifically → map to `grpc::StatusCode::ABORTED`, or retry with exponential backoff (max 3). |

### 🟡 Medium

| # | File | Line(s) | Problem | Fix |
|---|------|---------|---------|-----|
| 4 | `src/kms/` | — | PHI encryption key management stubbed. `phi_fields_enc_json` column exists but no KMS implementation to generate/store/rotate keys. | Implement KMS before any real PHI data enters the system. |
| 5 | `LocalAuthProvider.cc` | 67-72 | `permissions_from_scope_json()` silently skips unknown permission keys. Typo in API token scope → zero permissions with no error. | Validate scope keys against `parse_permission()` at creation time; reject unknowns. |
| 6 | `ShareServiceImpl.cc` | 206 | `scope_json` from request stored directly — no structural validation (is it valid JSON? an array?). | Validate `scope_json` shape and keys at creation. |
| 7 | `LocalAuthProvider.h` / `.cc` | 172-179 | No minimum password length. Empty or 1-char passwords accepted (Argon2id mitigates offline brute-force, but policy should enforce minimums). | Add configurable `min_password_length` (default ≥8) to `LocalAuthProviderConfig`. |

### 🔵 Informational

| # | Area | Note |
|---|------|------|
| 8 | Branch coverage | 44.2% — driven by untested validation branches in `*Columns.h` files and error-switch arms. Add focused unit tests for `validate_*` functions. |
| 9 | Duplicate UUID gen | Three identical `generate_uuid_v4()` implementations (ShareServiceImpl, RoleServiceImpl, SqliteBackend). Two use `std::random_device` (platform-dependent), one uses `libsodium`. Extract to `src/core/uuid.h`. |
| 10 | `RoleServiceImpl.cc:104-106` | `ROLE_KIND_UNSPECIFIED` silently maps to `Member` (least-privilege). Prefer throwing `INVALID_ARGUMENT`. |
| 11 | `IdentityColumns.h:83-86` | `looks_like_email` only checks `@` not at boundaries. Allows `"@@"`. Consider RFC 5321 subset check at API layer. |
| 12 | Postgres tests | All skipped in CI (no Postgres server). The Postgres backend has zero automated coverage. |

---

## Confirmed Safe

- **SQL injection**: All queries parameterized; column names from enum-switch string-literals.
- **Audit chain**: SHA-256 hash chain with append-only triggers, `this_hash UNIQUE`, pure verifier.
- **Auth cache invalidation**: `authz_version` epoch forces rebuild on permission change — no stale-cache window.
- **MFA gate**: `AuthMiddleware::authorize()` checks `mfa_complete` before access; bypass not possible.
- **Audit snapshots**: Repository-derived from authoritative state; callers cannot forge.
- **Token storage**: BLAKE2b-256 hashes only; plaintext token shown once at creation.
- **Cross-lab shares**: Three-signature scheme with per-role permission checks; no signature forgery possible.

---

## Verdict

Architecture is sound for pre-alpha. No critical data-loss or auth-bypass vulnerabilities. The three high-severity items (error-message leak, lockout-map growth, missing retry logic) are implementation hardening — straightforward to fix. Medium items are expected gaps for this stage (KMS stub, validation rigor). Recommend addressing 🔴 items before first production deployment.

---

## Resolution (2026-06-12)

| # | Severity | Status | Notes |
|---|----------|--------|-------|
| 1 | 🔴 | ✅ Fixed | `current_exception_to_grpc_status()` now returns generic `"internal server error"` and logs the real error to stderr; `e.what()` no longer reaches the client. |
| 2 | 🔴 | ✅ Fixed | `LockoutState` gains `last_activity`; `evict_stale_lockouts()` drops expired/idle entries on each failure and enforces `max_lockout_entries` (default 10000), preferring unlocked entries. |
| 3 | 🔴 | ✅ Fixed | `SerializationFailure → ABORTED`, `Unavailable → UNAVAILABLE`, `ForeignKeyViolation → FAILED_PRECONDITION` added to the translation funnel. Client may now retry on ABORTED. (Server-side auto-retry deferred — ABORTED is the canonical signal.) |
| 4 | 🟡 | ⏳ Deferred | KMS is a feature, not a fix; gated on "before any real PHI". Out of scope for this hardening pass. |
| 5 | 🟡 | ✅ Fixed | `validate_scope_json()` rejects malformed/non-array scopes and unknown permission keys at `create_api_token` time. |
| 6 | 🟡 | ✅ Fixed | `CreateShareRequest` rejects non-object / malformed `scope_json` with INVALID_ARGUMENT. |
| 7 | 🟡 | ✅ Fixed | `LocalAuthProviderConfig::min_password_length` (default 8) enforced in `hash_password`. |
| 8 | 🔵 | ⏳ Deferred | Branch-coverage work — separate task. |
| 9 | 🔵 | ✅ Fixed | Service `generate_uuid_v4` copies removed; single `core::generate_uuid_v4()` in `core/uuid.h`. SqliteBackend's libsodium generator left as-is (stronger, storage-layer). |
| 10 | 🔵 | ✅ Fixed | `ROLE_KIND_UNSPECIFIED` now → INVALID_ARGUMENT instead of silently mapping to Member. |
| 11 | 🔵 | ✅ Fixed | `looks_like_email` tightened: exactly one `@`, non-empty local part, dotted non-empty domain; rejects `@@`, `a@b`, multi-`@`. |
| 12 | 🔵 | ⏳ Deferred | Postgres CI coverage — infra task. |

**Tests added:** `HashPasswordRejectsShortPassword`, `CreateApiTokenRejectsUnknownScopeKey` (auth unit); `CreateRoleWithUnspecifiedKindIsRejected`, `GrantGlobalOnlyPermissionFailsPrecondition`, `CreateRoleWithSystemAdminKindIsRejected` (role integration); `CreateRejectsSameSourceAndTarget`, `CreateRejectsNonObjectScopeJson`, `OneSignerCannotSignTwoRoles`, `ListHidesDecidedUnlessRequested`, `ListRejectsMalformedPageToken` (share integration). Test fixtures bumped from `hunter2` → `hunter22` (8-char floor).

---

## Proactive Fixes (Discovered by author during hardening)

These were found and addressed independently while working through the original 12 findings.

| # | File | Severity | Problem | Fix |
|---|------|----------|---------|-----|
| F-1 | `RoleServiceImpl.cc` | 🔴 | A lab-owned custom role could be created with `SystemAdmin` kind, then granted a global-only permission (e.g. `lab.provision`). `resolve_permissions` promotes such grants to `global_permissions` for any SystemAdmin-kind role — a deployment-wide escalation path. | `from_proto_custom_role_kind()` rejects `ROLE_KIND_SYSTEM_ADMIN` for custom roles. `GrantPermission` rejects global-only permissions for lab-scoped roles with FAILED_PRECONDITION. |
| F-2 | `ShareServiceImpl.cc` | 🔴 | One principal holding `ShareApprove` on both labs could sign all three approver roles and single-handedly approve a cross-lab transfer (separation-of-duties violation). | `user_already_signed()` checks for prior signatures by the same user before `gate_approver_role()`. A second attempt returns FAILED_PRECONDITION. |
| F-3 | `ShareServiceImpl.cc` | 🟡 | `ListShareRequests` defaulted to returning all non-revoked requests (including approved/rejected). | Default query now filters to `status = 'pending'`; `include_decided` flag surfaces terminal states. |
| F-4 | `AuditServiceImpl.cc`, `ShareServiceImpl.cc` | 🟡 | `std::stoull(page_token)` threw `std::invalid_argument` on malformed tokens → caught as INTERNAL. A crafted client could trigger this. | `parse_page_offset()` helper catches the exception and throws `ConstraintViolation` → INVALID_ARGUMENT. |
| F-5 | `ShareServiceImpl.cc` | 🔵 | Nothing prevented a lab from sending a share request to itself. | `CreateShareRequest` rejects `source_lab_id == target_lab_id` with INVALID_ARGUMENT. |

**Re-audit stats**: 850 tests pass (0 fail), +10 tests vs. original audit. Clean build — no warnings (the two `-Wunused-result` warnings from the `hash_password` throw-tests were resolved with `(void)` casts). No regressions.
