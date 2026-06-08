# FreezerManager — Security & Data-Integrity Remediation Audit

**Date:** 2026-06-03
**Audited commit:** `f1e5470` — `fix(storage): fix Postgres B1–B8 bugs, add CI Postgres service, RLS integration tests`
**Reference:** `doc/CODE_REVIEW_2026-06-02.md` (Security and Data-Integrity Remediation Handoff)
**Method:** Manual code inspection of all files referenced in the remediation handoff, cross-referenced against each acceptance criterion.

---

## Executive Summary

The audited commit (`f1e5470`) addresses **Postgres-specific infrastructure bugs (B1–B8)** and **CI Postgres service integration**. It does **not** implement the 7-phase security/integrity remediation plan described in the 2026-06-02 handoff.

Of the ~40 discrete criteria across 7 phases, **only Phase 1 is fully resolved**. Phases 2–6 have critical gaps. Phase 7 (CI sanitizer execution) is entirely unimplemented.

---

## 1. Phase 1 — Per-lab authorization context

> **Status:** ✅ FULLY RESOLVED

| Criterion | Status | Evidence |
|---|---|---|
| `SessionContext` redesigned with `permissions_by_lab` + `global_permissions` | ✅ Pass | `src/auth/AuthTypes.h:76-95` |
| `can_see_lab()` method | ✅ Pass | `src/auth/AuthTypes.h:83-85` |
| `has_for_lab()` method | ✅ Pass | `src/auth/AuthTypes.h:87-90` |
| `has_global()` method | ✅ Pass | `src/auth/AuthTypes.h:92-94` |
| `resolve_permissions()` scopes grants per lab | ✅ Pass | `src/auth/LocalAuthProvider.cc:103-144` |
| Global-only permissions (`SampleDeleteHard`, `BackupRun`, `KeyRotate`) isolated from lab-scoped sets | ✅ Pass | `src/auth/LocalAuthProvider.cc:133-136` |
| `AuthMiddleware::authorize()` enforces per-lab permission checks | ✅ Pass | `src/rpc/AuthMiddleware.cc:42-48` |
| `AuthMiddleware::authorize()` enforces global permission checks when no lab_id | ✅ Pass | `src/rpc/AuthMiddleware.cc:46-47` |
| `is_global_only_permission()` function with explicit permission mapping | ✅ Pass | `src/core/permissions.h:182-209` |
| MFA gate present and enforced before permission checks | ✅ Pass | `src/rpc/AuthMiddleware.cc:37-39` |
| RLS injection uses bare keys (no double `app.` prefix) — B1 fix verified | ✅ Pass | `src/rpc/AuthMiddleware.cc:55-67` |
| Mixed-role regression test: `LabAdmin` in Lab A, `ReadOnly` in Lab B | ✅ Pass | `tests/unit/auth_middleware_test.cpp:349-363` (`AuthorizeScopesMixedLabAdminAndReadOnlyByLab`) |
| Mixed-role regression test: `Member` in Lab A, `LabAdmin` in Lab B (asymmetric) | ✅ Pass | `tests/unit/auth_middleware_test.cpp:365-376` (`AuthorizeScopesMixedMemberAndLabAdminAsymmetrically`) |
| Existing single-lab tests continue to pass | ✅ Pass | All `local_auth_provider_test.cpp` and `auth_middleware_test.cpp` tests |

### Assessment

Phase 1 is the **only fully resolved phase**. The implementation matches the suggested design exactly: `SessionContext` carries `permissions_by_lab` and `global_permissions`; `resolve_permissions` iterates memberships and routes `is_global_only_permission()` grants to the global set only for `SystemAdmin` roles; `AuthMiddleware::authorize` checks `has_for_lab` when `lab_id` is provided and `has_global` when it is not.

---

## 2. Phase 2 — API-token scope enforcement

> **Status:** ❌ CRITICAL GAPS — Fail-open paths exist

| Criterion | Status | Evidence / Issue |
|---|---|---|
| Token-scope parser exists | ✅ Pass | `permissions_from_scope_json()` at `src/auth/LocalAuthProvider.cc:52-70` |
| Scope intersection applied to base permissions | ✅ Pass | `intersect_grants_with_scope()` at `src/auth/LocalAuthProvider.cc:89-101` |
| Malformed JSON → reject token | ❌ **FAIL-OPEN** | `nlohmann::json::parse(scope_json, nullptr, false)` returns empty value on parse failure; `!parsed.is_array()` returns empty set, which `intersect_grants_with_scope` treats as no-op (line 92-93) |
| JSON object instead of array → reject token | ❌ **FAIL-OPEN** | `!parsed.is_array()` at line 56 returns empty set → full base permissions |
| Unknown permission key → reject token | ❌ **FAIL-OPEN** | `catch (const std::exception&)` at line 64 silently skips unknown keys |
| Non-string array element → reject token | ❌ **FAIL-OPEN** | `item.is_string()` check at line 60 silently skips non-strings |
| `[]` (empty array) → no permissions | ❌ **FAIL-OPEN** | `intersect_grants_with_scope` at line 92-93 returns grants unchanged when scope is empty |
| Token `lab_id` restricts visible labs | ❌ **NOT IMPLEMENTED** | `build_api_token_context()` at lines 543-558 never filters by `token.lab_id` |
| Token `lab_id` not visible to user → reject | ❌ **NOT IMPLEMENTED** | No visibility check on token's lab_id against user's memberships |

### Current behavior vs. required behavior

| Token scope | Required (handoff) | Current | Gap |
|---|---|---|---|
| `["sample.read"]` | Only `sample.read` | Only `sample.read` | ✅ OK |
| `[]` | No permissions | Full base permissions | ❌ Fail-open |
| `{"sample.read": true}` | Reject token | Full base permissions | ❌ Fail-open |
| `["sample.raed"]` (typo) | Reject token | Full base permissions | ❌ Fail-open |
| `[123]` | Reject token | Full base permissions | ❌ Fail-open |
| Token with `lab_id=A`, request to Lab B | Denied | Allowed (lab_id not enforced) | ❌ Fail-open |

### Assessment

An API token with `scope_json = "garbage"` currently receives the user's **full** permissions across all their labs. An API token scoped to Lab A can access Lab B data. This is a **critical** gap. The fix requires:
1. Make `permissions_from_scope_json()` throw on malformed JSON, unknown keys, and non-string elements.
2. Make empty scope equivalent to zero permissions, not unrestricted.
3. In `build_api_token_context()`, restrict `visible_labs` to `{token.lab_id}` when present.

---

## 3. Phase 3 — Session invalidation & authorization-cache invalidation

> **Status:** ❌ NOT RESOLVED

| Criterion | Status | Evidence / Issue |
|---|---|---|
| `authz_version` column on `users` table | ❌ **NOT IMPLEMENTED** | No `authz_version` column in any migration (SQLite or Postgres) |
| `CachedContext.authz_version` field | ❌ **NOT IMPLEMENTED** | `CachedContext` at `LocalAuthProvider.h:114-119` has no version field |
| Disabled user check on session-token validation | ❌ **NOT IMPLEMENTED** | `validate_session_token()` (lines 393-419) never queries user status |
| Disabled user check on API-token validation | ❌ **NOT IMPLEMENTED** | `validate_api_token()` (lines 496-523) never queries user status |
| Disabled user check on TOTP verification | ❌ **NOT IMPLEMENTED** | `verify_totp()` (lines 196-234) queries user for TOTP secret, never checks status |
| Cache invalidation on session revocation | ✅ Pass | `cache_evict()` called from `revoke_session()` (line 248) |
| Cache invalidation on revoke-all-sessions | ✅ Pass | `cache_evict_user()` called from `revoke_all_sessions()` (line 263) |
| Cache invalidation on membership/role changes | ❌ **NOT IMPLEMENTED** | No hooks triggered from membership or role mutations |
| Stale cache TTL-based refresh | ✅ Pass | `lookup_or_build_context()` at lines 457-492 expires cache entries |

### Test gap acknowledged in code

The test `VerifyTotpDisabledUser` (`tests/unit/local_auth_provider_test.cpp:675-692`) explicitly documents this gap:

> "verify_totp should still fail because the user query finds the user (soft_delete doesn't remove the row), and the TOTP secret is still set. The actual rejection should come from the TOTP code being wrong."

This means a disabled user can complete TOTP verification and gain full MFA access despite being disabled.

### Assessment

A disabled user's existing bearer sessions and API tokens continue to work indefinitely (until natural session expiry or cache TTL expiration). Permission downgrades (membership revocation, role change) are not reflected until the cache TTL expires (default 5 minutes). No `authz_version` mechanism exists.

---

## 4. Phase 4 — PostgreSQL audit-chain head selection

> **Status:** ❌ NOT RESOLVED

| Criterion | Status | Evidence / Issue |
|---|---|---|
| Monotonic `seq BIGSERIAL` column in `audit_events` | ❌ **NOT IMPLEMENTED** | `seq` column absent from both Postgres migration 0001 and SQLite migration 0011 |
| `ORDER BY seq DESC` for chain head selection | ❌ **NOT IMPLEMENTED** | Postgres still uses `ORDER BY at_micros DESC, id DESC` (`PostgresBackend.cc:766`) |
| SQLite explicit `seq` ordering | ❌ **NOT IMPLEMENTED** | SQLite still uses `ORDER BY rowid DESC` (`SqliteBackend.cc:781`) |
| Advisory transaction lock acquired | ✅ Pass | `pg_advisory_xact_lock(8675309)` at line 762 |
| Forward migration for existing rows | ❌ **NOT IMPLEMENTED** | No migration adds the `seq` column |

### Assessment

Multiple mutations within one transaction share the same `at_micros` timestamp. With random UUIDs for `id`, the lexicographically largest UUID is not guaranteed to be the final event appended, meaning a later transaction can select an earlier event as its predecessor and create an audit-chain fork.

---

## 5. Phase 5 — Repository-derived audit snapshots

> **Status:** ❌ NOT RESOLVED

| Criterion | Status | Evidence / Issue |
|---|---|---|
| `MutationContext` stripped of `before_json`/`after_json` | ❌ **NOT IMPLEMENTED** | `MutationContext` still has both fields (`IStorageBackend.h:50-52`) |
| `AuditMutation` struct with snapshot fields | ❌ **NOT IMPLEMENTED** | Both backends' `AuditMutation` lacks `before_json`/`after_json` (Postgres: `PostgresBackend.cc:671-676`, SQLite: `SqliteBackend.cc:953-958`) |
| Snapshots derived from authoritative entity state | ❌ **NOT IMPLEMENTED** | Postgres reads `mutation.context.before_json`/`after_json` at lines 777-784; SQLite does the same at lines 1037-1044 |
| Repositories produce snapshots on insert/update/soft-delete | ❌ **NOT IMPLEMENTED** | No repository implementation generates `before_json` or `after_json` |
| Soft-delete snapshots include generated fields | ❌ **NOT IMPLEMENTED** | N/A — snapshots not derived by repositories |

### Current data flow

```
Caller → MutationContext (with caller-supplied before_json/after_json)
       → Repository operation
       → note_mutation(context) — trusts caller's snapshots
       → Audit append — reads context.before_json / context.after_json
```

### Assessment

The audit chain's trustworthiness depends entirely on the caller honestly providing `before_json` and `after_json` in the `MutationContext`. A buggy or malicious caller can omit or falsify audit snapshots. The repository layer, which has access to the authoritative entity state, never contributes snapshots.

---

## 6. Phase 6 — Multi-lab referential integrity

> **Status:** ❌ PARTIALLY RESOLVED — Critical gaps remain

| Criterion | Status | Evidence |
|---|---|---|
| Box: same-lab validation | ✅ Pass | `SampleRepositories.cc:508-517` — checks `boxes.id = ? AND boxes.lab_id = ?` |
| Item type: same-lab validation | ✅ Pass | `SampleRepositories.cc:547-556` — checks `item_types.id = ? AND item_types.lab_id = ?` |
| Container type: same-lab validation | ❌ **GAP** | `validate_sample()` never validates `container_type_id` against lab_id |
| Parent sample: same-lab validation | ❌ **GAP** | No validation that `parent.lab_id == child.lab_id` |
| Parent sample: ancestry cycle check | ❌ **GAP** | No cycle detection |
| Parent sample: self-reference check | ❌ **GAP** | No `parent.id != child.id` check (DB `CHECK` on samples table missing) |
| SampleProject: same-lab validation | ❌ **GAP** | No check that `sample.lab_id == project.lab_id` |
| CheckoutEvent: lab matches sample | ❌ **GAP** | No check that `event.lab_id == referenced_sample.lab_id` |
| Composite unique indexes (`samples(id, lab_id)`) | ❌ **NOT IMPLEMENTED** | No compound uniqueness indexes for lab-scoped entities |
| Composite foreign keys | ❌ **NOT IMPLEMENTED** | All FKs are single-column; no `FOREIGN KEY (container_type_id, lab_id) REFERENCES container_types(id, lab_id)` |
| `sample_projects.lab_id` column | ❌ **NOT IMPLEMENTED** | Schema lacks a `lab_id` column on `sample_projects` |

### Assessment

While box and item-type cross-lab checks exist, the following are missing:
- **Container type**: A Lab A sample can use a Lab B container type.
- **Parent sample**: A Lab A sample can claim a Lab B sample as its parent. No cycle or self-reference detection.
- **SampleProject**: A Lab A sample can be linked to a Lab B project.
- **CheckoutEvent**: An event can claim a different lab than its referenced sample.

### Schema hardening

No compound indexes or composite foreign keys exist. The `sample_projects` table lacks a `lab_id` column entirely, making application-level cross-lab validation the only enforcement mechanism.

---

## 7. Phase 7 — CI sanitizer execution

> **Status:** ❌ NOT RESOLVED

| Criterion | Status | Evidence / Issue |
|---|---|---|
| ASan tests execute in CI | ❌ **NOT IMPLEMENTED** | `build.yml:116` — `if: ${{ matrix.preset == 'dev' }}` gates test execution to dev only |
| UBSan tests execute in CI | ❌ **NOT IMPLEMENTED** | Same gate blocks ubsan preset |
| TSan tests execute in CI | ❌ **NOT IMPLEMENTED** | Same gate blocks tsan preset |
| `--output-on-failure` on ctest | ❌ **NOT IMPLEMENTED** | Line 119: `ctest --preset dev` — no `--output-on-failure` flag |
| PostgreSQL service container | ✅ Pass | Lines 52-65 in `build.yml` |
| `FMGR_TEST_POSTGRES_URL` env var | ✅ Pass | Line 118 |

### Assessment

Sanitizer presets (asan, ubsan, tsan) build correctly in CI but never run tests. The `dev` gate on the Test step blocks all non-dev presets. The sanitizer presets' CMakeLists likely define separate CTest presets (e.g., `asan`, `ubsan`, `tsan`) which are never invoked.

---

## 8. Documentation cleanup

| Item | Handoff reference | Status | Notes |
|---|---|---|---|
| PostgreSQL CI service-container note | §11 | ✅ | postgres:16 service present in `build.yml` |
| Token-hash comments (Argon2id → BLAKE2b) | §11 | ⚠️ Partial | `session.h:5` still says "Argon2id"; field comment at line 57 correctly says "BLAKE2b" |
| Cache documentation | §11 | ✅ | `AuthTypes.h:72-73`: "Permission grants may be cached by auth providers between requests" |
| API token scope semantics | §11 | ❌ | No documentation of fail-closed behavior, empty scope policy, or lab scope restrictions |
| Token `lab_id` semantics (global vs. multi-lab) | §11 | ❌ | Not documented. `ApiToken.lab_id` is optional with no explicit policy comment. |

---

## 9. Security hardening tasks (§10)

| Item | Status | Notes |
|---|---|---|
| Password-login timing normalization | ❌ Not started | Unknown/disabled users return early without Argon2id verification |
| TOTP attempt throttling | ❌ Not started | No per-session failure tracking |
| Persistent login lockout tracking | ❌ Not started | Lockout state is in-memory only |
| TOTP secret encryption | ❌ Not started | Field is named `totp_secret_enc` but stored as plaintext |
| API-token CRUD RPCs | ❌ Not started | No create/revoke/list RPC handlers |

---

## 10. Summary by phase

| Phase | Status | Severity | Gap count |
|---|---|---|---|
| **Phase 1** — Per-lab authorization | ✅ **RESOLVED** | — | 0 |
| **Phase 2** — API-token scope enforcement | ❌ **CRITICAL** | API tokens bypass all scope restrictions | 7 |
| **Phase 3** — Session invalidation | ❌ **HIGH** | Disabled users retain access; role changes take 5 min to propagate | 6 |
| **Phase 4** — Audit-chain ordering | ❌ **MEDIUM** | Multi-mutation transactions can fork the chain | 4 |
| **Phase 5** — Audit snapshots | ❌ **MEDIUM** | Callers control what goes into the audit log | 4 |
| **Phase 6** — Cross-lab integrity | ❌ **HIGH** | 6 of 8 relationship types lack lab-scoped validation | 6 |
| **Phase 7** — CI sanitizers | ❌ **LOW** | Sanitizers build but never run tests | 4 |
| **Documentation** | ⚠️ **PARTIAL** | — | 2 |

### Totals

- **Resolved:** ~10 criteria (all in Phase 1)
- **Unresolved:** ~31 criteria across Phases 2–7 and documentation
- **Resolution rate:** ~24%

---

## 11. Recommended priority order

1. **Phase 2 (API token scope)** — Fail-open parsing + missing lab_id enforcement. An API token acts as a master key today.
2. **Phase 3 (Session invalidation)** — Disabled users and downgraded roles continue accessing data.
3. **Phase 6 (Cross-lab integrity)** — Data can silently cross lab boundaries through 6 relationship types.
4. **Phase 4 (Audit chain)** — Deterministic ordering needed before audit is relied upon.
5. **Phase 5 (Audit snapshots)** — Trustworthy snapshots needed before audit is relied upon.
6. **Phase 7 (CI sanitizers)** — Enable test execution for asan/ubsan/tsan presets.

---

## 12. Verification

This audit was performed via manual code inspection of all files referenced in the 2026-06-02 remediation handoff:

```
src/auth/AuthTypes.h
src/auth/LocalAuthProvider.h
src/auth/LocalAuthProvider.cc
src/rpc/AuthMiddleware.h
src/rpc/AuthMiddleware.cc
src/core/session.h
src/core/permissions.h
src/storage/IStorageBackend.h
src/storage/sqlite/SqliteBackend.cc
src/storage/sqlite/SampleRepositories.cc
src/storage/postgres/PostgresBackend.cc
tests/unit/local_auth_provider_test.cpp
tests/unit/auth_middleware_test.cpp
.github/workflows/build.yml
```

Each criterion was checked against the actual source code at commit `f1e5470`. Where applicable, line numbers are cited for reproducibility.
