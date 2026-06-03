# Code Review Report — FreezerManager v0.1 (2026-06-02)

**Date:** 2026-06-02
**Supersedes:** [`CODE_REVIEW_2026-06-01.md`](./CODE_REVIEW_2026-06-01.md)
**Review scope:** Whole codebase, with deep focus on work landed since the last review —
RBAC middleware (E3), session expiry + permission cache (D9.3), and the new
PostgreSQL backend core + conformance suite (C5.1).
**Methodology:** Manual read of new/changed sources, build + full test run, doc reconciliation.

---

## Part A — Status & Documentation Reconciliation

### Current implementation state

| Slice | Scope | State |
|---|---|---|
| M0 | Repo skeleton, CI, CMake + Conan, SPDX/CLA | ✅ Done |
| C1/C2 | Core value types, `IStorageBackend` + typed query DSL | ✅ Done |
| D1–D9 | Lab/User/Role/Freezer/Box/Sample/ShareRequest/Session entities + SQLite backend | ✅ Done |
| E1/E2 | `IAuthProvider` + `LocalAuthProvider` (Argon2id + TOTP + lockout) | ✅ Done |
| E3 + D9.3 | `AuthMiddleware` RBAC gate, session expiry, permission cache | ✅ Done |
| E5.1/E5.2 | Hash-chained audit log + canonical JSON | ✅ Done |
| C5.1 | `PostgresBackend` core + connection pool + Postgres conformance suite | ⚙️ Core landed, **never run against a real Postgres** (see B1–B3) |
| C5.x | Postgres **domain repositories** (Identity/Role/Layout/Box/Item/Sample/Session/Share/Audit) | 🔲 Not started — the main remaining gap before CLI/CSV (M1) |

### Real metrics (verified 2026-06-02)

- **435** `TEST`/`TEST_F`/`TEST_P` macros across **33** test source files (+ **6** benchmark files).
- `ctest --preset dev`: **435 passed, 0 failed, 13 skipped** (Postgres conformance tests
  `GTEST_SKIP` when `FMGR_TEST_POSTGRES_URL` is unset — the local default).
- ~**14.9 K** lines across `src/` (`.h`/`.cc`).

### Documentation drift found (fixed in this pass)

| Doc | Stale claim | Reality |
|---|---|---|
| `README.md` L34–41 | 8 domain entities marked 🔲 planned | All ✅ implemented + SQLite-tested |
| `README.md` L100, L103 | conformance suite + authz tests 🔲 | Both exist (`*_conformance_test.cpp`, `auth_middleware_test.cpp`) |
| `README.md` L113–115 | roadmap test counts 259 / 357 / 409 | **435** total now |
| `TEST_COVERAGE.md` L3 | "357 tests across 23 files" | **435 tests across 33 files** |
| `TEST_COVERAGE.md` §9 | "Postgres backend not implemented", "RBAC middleware E3 not implemented" | Both now exist + tested |

### Prior-review findings — resolution status

| ID | Finding | Status | Fixed by |
|---|---|---|---|
| F1 | `optional_to_json` duplicated 7× | ✅ Resolved | `fc71cca` — consolidated into `src/core/json_helpers.h` |
| F2 | `Volume`/`Mass` cross-unit throws, no converter | ✅ Resolved | `a15bf9e` — added `to_unit()` |
| F3 | Date/Datetime custom-field validation accepts any string | ✅ Resolved | `a15bf9e` — format validation added |
| F4 | Permission catalog lookup O(n) linear scan | ✅ Resolved | `fc71cca` — `to_key()` O(1) index, `parse_permission()` binary search (`permissions.h:120–170`) |
| F5 | Test helpers duplicated across test files | ✅ Resolved | `fc71cca` — shared `tests/test_helpers.*` |
| F6 | Audit SHA-256 vs token BLAKE2b — intentional? | ⏳ Open | Documentation question; no rationale comment added yet |
| F7 | `type_index` repository-map key needs a clarifying comment | ⏳ Open | Minor; not yet addressed |

---

## Part B — Fresh Findings (deep pass on new code)

Severity legend: 🔴 bug · 🟡 risk · 🔵 nit · ❓ question.

> **Theme:** every Postgres finding below is undetected by CI because all 13 Postgres
> conformance tests `GTEST_SKIP` without `FMGR_TEST_POSTGRES_URL`, and there is no
> AuthMiddleware↔Postgres RLS integration test. The Postgres path has effectively
> **never executed end-to-end**. B1–B3 are blockers for trusting it.

### B1. 🔴 RLS session variable is double-prefixed → tenant isolation silently disabled

**Files:** `src/rpc/AuthMiddleware.cc:57,66` ↔ `src/storage/postgres/PostgresBackend.cc:692–701`

`PostgresTransaction::set_session_var` prepends `"app."` to the key it receives:

```cpp
impl_->work->exec("SELECT set_config($1, $2, true)",
                  pqxx::params{"app." + std::string(key), std::string(value)});
```

The Postgres conformance test honors that contract — it passes the **bare** key
(`postgres_backend_conformance_test.cpp:719`: `set_session_var("current_lab_ids", …)`,
then reads `current_setting('app.current_lab_ids')`). But `AuthMiddleware` passes the key
**already prefixed**:

```cpp
txn.set_session_var("app.current_user_id", ctx.user_id.to_string());
txn.set_session_var("app.current_lab_ids", lab_ids);
```

Result on Postgres: the variable actually set is `app.app.current_lab_ids`. Every RLS
policy reads `current_setting('app.current_lab_ids', true)`, which stays **unset**.

**Impact:** combined with B2 (fail-open policy), RLS provides **zero** lab isolation — a
session scoped to lab A can read every lab's rows. This is the system's defense-in-depth
backstop for multi-tenant data separation; it is currently a no-op.

**Fix:** pick one prefixing site. Either have `AuthMiddleware` pass bare keys
(`"current_user_id"`, `"current_lab_ids"`) — matching the conformance test and the
`PostgresBackend` contract — or drop the `"app."` prepend in `set_session_var` and pass
fully-qualified keys everywhere. Then add an integration test that exercises the
`AuthMiddleware::inject_rls_vars` → Postgres → `current_setting` path (the current mock in
`auth_middleware_test.cpp:97` captures the raw key and never reproduces the prefix, so it
masks the defect).

### B2. 🔴 RLS policies fail **open** when the lab-ids variable is unset

**File:** `src/storage/postgres/PostgresBackend.cc:284–286` (pattern repeated for every
lab-scoped table: containers, freezers, container_types, box_types, boxes, item_types, cfd,
projects, samples, checkout_events).

```sql
CREATE POLICY fmgr_samples_lab ON samples USING (
  lab_id = ANY(string_to_array(COALESCE(current_setting('app.current_lab_ids',true),''),','))
  OR COALESCE(current_setting('app.current_lab_ids',true),'') = '');
```

The trailing `OR … = ''` clause means: **if the variable is empty/unset, the row passes**.
Any connection that forgets to set the variable (or sets the wrong name, per B1) sees
**all rows across all labs**. RLS should fail *closed* — an unset context should expose
nothing, not everything.

**Fix:** drop the `OR … = ''` escape hatch so an unset context yields zero rows. If a
genuine "superuser / migration" bypass is needed, model it explicitly (e.g. a dedicated
`app.bypass_rls` boolean checked only for trusted maintenance roles, or rely on the table
owner with `BYPASSRLS`), not as the default for an empty string.

### B3. 🔴 Migration 0003 uses `ADD CONSTRAINT IF NOT EXISTS` — invalid PostgreSQL syntax

**File:** `src/storage/postgres/PostgresBackend.cc:164–166`

```sql
ALTER TABLE lab_memberships
  ADD CONSTRAINT IF NOT EXISTS lab_memberships_role_id_fk
  FOREIGN KEY (role_id) REFERENCES roles(id) DEFERRABLE INITIALLY DEFERRED;
```

PostgreSQL (through 16) does **not** support `IF NOT EXISTS` on `ADD CONSTRAINT`. On a real
server `migrate_to_latest()` throws a syntax error at migration 3, so the schema never
builds. That this was not caught confirms the suite has not run against a live Postgres.

**Fix:** guard the constraint the same way the triggers in migration 0001 are guarded —
wrap in a `DO $$ … IF NOT EXISTS (SELECT 1 FROM pg_constraint WHERE conname = …) THEN
ALTER TABLE … ADD CONSTRAINT … END IF; $$;` block, or define the FK inline in the
`lab_memberships` `CREATE TABLE` once `roles` exists (reorder migrations) — but the
`DO`-block guard is the smaller change and keeps migrations idempotent.

### B4. 🟡 Postgres audit events drop `lab_id` and before/after payloads

**File:** `src/storage/postgres/PostgresBackend.cc:746–785`

Every appended audit row hardcodes `lab_id = NULL` and `before_json`/`after_json = "{}"`,
regardless of the mutation. `MutationContext` carries no lab id or row diff, so the audit
chain records *that* something changed but not *which lab* or *what* changed. For a
PHI-aware, compliance-oriented system this is a material gap.

**Fix:** extend `MutationContext` (or `note_mutation`) to carry `lab_id` and the
before/after snapshots, and bind them instead of the hardcoded NULL/`{}`. Tracking this
against the audit requirements in `doc/PRD.md` is worthwhile before domain repos land, so
the repositories populate it from day one.

### B5. 🟡 Pooled connections are never health-checked or replaced

**File:** `src/storage/postgres/PostgresBackend.cc:577–620, 660–683`

The pool creates `pool_size` `pqxx::connection`s up front and hands them out forever. If a
connection drops (DB restart, idle timeout, network blip), it stays in the pool and every
transaction that draws it throws. There is no `is_open()` check on acquire and no
reconnect.

**Fix:** on `acquire`, validate the slot (`conn.is_open()`); if dead, reconstruct the
`pqxx::connection` before use. Consider mapping connection-class SQLSTATEs (`08xxx`) to
`Unavailable` and retrying once on a fresh connection.

### B6. 🔵 `throw_pqxx_error` funnels all unmapped errors to `ConstraintViolation`

**File:** `src/storage/postgres/PostgresBackend.cc:33–40`

The default branch maps any unrecognized SQLSTATE — including connection failures
(`08xxx`), syntax errors (`42xxx`), and admin shutdown (`57xxx`) — to `ConstraintViolation`.
Callers can't distinguish "your data violated a rule" from "the database is unreachable."

**Fix:** map `08xxx`/`57P01` → `Unavailable`, leave a genuine fallback for truly unknown
states, and consider a distinct code for syntax/internal errors so they surface as bugs
rather than user constraint violations.

### B7. 🔵 Migration checksums use `std::hash<std::string>`

**File:** `src/storage/postgres/PostgresBackend.cc:567–571` (SQLite backend uses the same
scheme).

`std::hash` is neither stable across standard-library implementations nor across compiler
upgrades, and is not collision-resistant. A toolchain bump can change the hash of an
unchanged migration and trip the `MigrationFailure` "checksum changed" guard — a confusing
false alarm — while a collision could let a *changed* migration slip through.

**Fix:** hash migration text with a fixed algorithm already linked in (e.g. libsodium
`crypto_generichash`/BLAKE2b, or the existing SHA-256 audit helper) and store the hex
digest. Low urgency, but cheap to fix before more migrations accumulate.

### B8. 🔵 `current_version()` swallows every `pqxx::sql_error` as "version 0"

**File:** `src/storage/postgres/PostgresBackend.cc:886–901` (and
`audit_event_count_for_tests()` at 928–943).

The catch-all assumes "table doesn't exist yet → version 0," but it also hides real
failures (permission denied, connection lost), reporting a fresh/empty schema when the
truth is unknown. That could drive `migrate_to_latest` to re-run migrations against a DB it
couldn't actually read.

**Fix:** narrow the catch to the undefined-table SQLSTATE (`42P01`) and let other errors
propagate (as `Unavailable`/`MigrationFailure`).

### B9. 🔵 Most non-lab tables have no RLS (likely intentional — document it)

**File:** `src/storage/postgres/PostgresBackend.cc` migrations 0002/0003/0009/0010

`labs`, `users`, `roles`, `permissions`, `sessions`, `api_tokens`, `share_requests`, and the
join tables carry no RLS policy. For cross-lab tables (`share_requests`) and global ones
(`permissions`) that is defensible, but `sessions`/`api_tokens` hold token hashes for all
users and rely entirely on app-layer scoping.

**Fix (doc, not code):** add a short comment block listing which tables are deliberately
RLS-exempt and why, so a future reader doesn't read the omission as an oversight. Revisit
`sessions`/`api_tokens` when the auth RPCs are designed.

---

## Architecture Observations

| Component | State | Notes |
|---|---|---|
| Core domain types | ✅ Solid | Strong IDs, unit-safe quantities, exhaustive enum/JSON round-trips |
| `IStorageBackend` + Query DSL | ✅ Clean | Backend-agnostic; conformance suite parameterized across backends |
| SQLite backend | ✅ Mature | 260+ tests; migrations, concurrency, soft-delete all covered |
| Auth (`LocalAuthProvider`, TOTP) | ✅ Good | Argon2id + RFC 6238; cache + lockout thread-safe |
| `AuthMiddleware` RBAC gate | ✅ Good design, ⚠️ see B1 | 4-step gate ordering is correct; RLS injection has the prefix bug |
| Audit chain | ✅ Production-grade on SQLite | Postgres path needs B4 (lab_id/diff) before parity |
| **Postgres backend** | ⚙️ **Unverified** | Core written but B1–B3 mean it has not run end-to-end; needs CI DB service (C5.2) |
| Postgres domain repos | 🔲 Not started | The gating work for M1 |

---

## Summary

| Severity | Count | Items |
|---|---|---|
| 🔴 bug | 3 | B1 RLS double-prefix · B2 RLS fail-open · B3 invalid migration SQL |
| 🟡 risk | 2 | B4 audit drops lab_id/diff · B5 no connection health-check |
| 🔵 nit | 4 | B6 error mapping · B7 checksum hash · B8 swallowed errors · B9 RLS-exempt tables undocumented |
| ❓ open (prior) | 2 | F6 hash divergence · F7 type_index comment |

**Bottom line.** The SQLite-backed core remains production-quality and the prior review's
findings are all resolved. The new risk is concentrated in the **PostgreSQL backend**: it
compiles and its unit-shaped tests pass, but because the live-DB tests are skipped by
default, three real defects (B1–B3) sit undetected — and two of them (B1+B2) jointly nullify
multi-tenant row isolation. **Recommended next steps, in order:** (1) fix B3 so migrations
run; (2) stand up the CI Postgres service (TODO C5.2) so the conformance suite actually
executes; (3) fix B1+B2 and add an AuthMiddleware↔RLS integration test; (4) then proceed to
Postgres domain repositories.
