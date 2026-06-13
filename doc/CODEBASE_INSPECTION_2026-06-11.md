# Codebase Inspection — 2026-06-11

## Overview

- **125 source files** (`.cc`/`.h`), **42 test files**, **10 proto files**
- **~9,000 lines** of production code (excl. tests/generated)
- **~601 tests**, all passing (16 PG conformance tests skip without `FMGR_TEST_POSTGRES_URL`)
- Current commit: `c9480dc` — ItemTypeService gRPC handlers just landed

## Test Coverage (gcovr)

| | Lines | Functions | Branches |
|---|---|---|---|
| **Overall** | **86.9%** (7,757 / 8,925) | **98.0%** (1,940 / 1,979) | **44.2%** (8,451 / 19,133) |
| Excl. server/CLI/detail | 90.0% | — | — |

### Low spots

| File | Lines | Notes |
|---|---|---|
| `storage/detail/AuditColumns.h` | 20% | Template column mappers; not directly instantiated |
| `storage/detail/LayoutColumns.h` | 22% | Same — indirect coverage via repos |
| `storage/detail/IdentityColumns.h` | 33% | Same |
| `storage/postgres/AuditRepositories.cc` | 66% | Many error-handling branches untriggered without live PG |
| `storage/sqlite/AuditRepositories.cc` | 77% | Audit-only paths (hash chain verification) |
| `storage/postgres/PostgresRepoSupport.h` | 69% | Pool contention + error mapping paths |

**Branch coverage is 44.2%** — expected for C++: many `catch`/error branches only fire in concurrency/integration tests (which aren't compiled under `--coverage`).

## Implementation Status vs PRD

### Done (M0–M2)
- ✅ All 15 domain entities (Lab, User, Role, Freezer, Box, Sample, etc.)
- ✅ `IStorageBackend` + typed query DSL
- ✅ SQLite backend (WAL, migrations 0001–0013)
- ✅ PostgreSQL backend (RLS, connection pool, full domain repos)
- ✅ `LocalAuthProvider` (Argon2id, TOTP, session lifecycle, account lockout)
- ✅ `AuthMiddleware` (4-step RBAC gate, per-lab permissions, RLS injection)
- ✅ API-token scope enforcement (fail-closed)
- ✅ Append-only audit chain + hash-chain verifier
- ✅ Repository-derived audit snapshots (caller cannot forge)
- ✅ `authz_version`-based cache invalidation
- ✅ `freezerctl` skeleton + sample CSV export

### In progress
- 🔲 gRPC services: 3 of 5 landed (Auth/Session, Lab, Box, ItemType); SampleService next

### Deferred
- 🔲 Remaining RPCs: SampleService, RoleService, AuditService, ShareService
- 🔲 Qt desktop client, React SPA, Python API
- 🔲 PHI field-level encryption, KMS, backups
- 🔲 OIDC/LDAP/mTLS auth providers
- 🔲 Packaging (`.deb`, `.rpm`, Docker)

## Known Gaps

| Gap | Severity | Detail |
|---|---|---|
| **Sanitizer CI execution** | High | `build.yml` gates test step to `dev` preset only; asan/ubsan/tsan build but never run tests |
| **PG audit before-images** | Low | SQLite captures full before/after on all entities; PG misses before on Box/Layout/ItemType/Role/Session/Share. Forge channel already closed — audit richness only. |
| **clang-tidy debt** | Low | ~278 pre-existing warnings; non-blocking (main branch unprotected). Mostly mechanical (`readability-identifier-length`, uppercase-literal-suffix). |
| **`greater_or_equal` / `less_or_equal` predicates** | Low | Implemented but no production code uses them; skipped from tests intentionally |
| **Property/fuzz test suites** | Low | Directories exist but empty; RapidCheck/libFuzzer not yet integrated |
| **E2E smoke tests** | Low | No server exists yet; `tests/e2e/` placeholder |

## Next Actions (priority order)

1. **SampleService gRPC handlers** — last blocker before "usable server" milestone
2. **Run sanitizers in CI** — drop `preset == dev` gate, add `--output-on-failure` to ctest
3. **Remaining services** — RoleService → AuditService → ShareService
4. **PG audit before-image richness** — mechanical: add `find_by_id` before write in ~10 methods
5. **clang-tidy cleanup** — sweep when time permits; mostly mechanical
