# #228 — audit predecessor integrity before locked MFA transactions

Issue: [#228](https://github.com/Yuxin-Ren-SZ/FreezerManager/issues/228).
PR: [#234](https://github.com/Yuxin-Ren-SZ/FreezerManager/pull/234).
Worker: worker-3 (Codex), slot 3, `fix/228-audit-predecessor`.
Baseline: main `2c02b29`; migration lock reserves **16** in both backends.

## Result and scope

Migration `0016_audit_predecessor_integrity` verifies the complete existing audit
chain before adding `audit_events_prev_hash_unique`, including the genesis
predecessor. SQLite uses the migrator's existing `BEGIN IMMEDIATE`; PostgreSQL
takes `ACCESS EXCLUSIVE` on `audit_events` in its migration transaction. The
verifier reuses the backend's existing row mapping on that same connection and
transaction. A fork, orphan, invalid row or hash mismatch fails as
`MigrationFailure`, rolls back schema/version changes, and leaves the existing
chain untouched. The explicit verification flag participates in checksums when
enabled; every legacy false-flag checksum is unchanged.

Every PostgreSQL audit insert targets the named predecessor constraint with
`ON CONFLICT ... DO NOTHING RETURNING id`. A zero-row result is a fixed-text
`SerializationFailure`; domain changes and every audit event in the losing batch
roll back together. The advisory lock remains, but is no longer mistaken for
snapshot refresh. Native SQLSTATE 40001/40P01 keep their transient status; other
uniqueness failures keep `UniqueViolation`.

SQLite also targets only predecessor conflicts. Because `BEGIN IMMEDIATE`
already excludes competing writers, a no-insert result is a fixed-text base
`BackendError` invariant failure, translated as INTERNAL rather than
ALREADY_EXISTS. Unrelated ID/hash uniqueness retains its existing refusal class.
No collision is skipped while later events or domain writes commit.

The custom conformance schemas explicitly supply the required predecessor
constraint/index and support downgrade/reapply. The PostgreSQL fixture selects
the integrity migration by version/name rather than whichever migration is last.
No integrity index is created early by `ensure_metadata_tables`.

This slice adds no auth transaction mode, session writer, MFA state/proof table,
replay policy, provider integration, recovery RPC or dependency. #211 consumes
this prerequisite; #179 and its deployment gate remain open.

## Regressions and review revisions

`tests/unit/audit_repository_test.cpp` covers migration acceptance/preservation,
genesis/non-genesis forks, orphan/hash-mismatch refusal, unchanged rows/version
after repeated startup refusal, checksummed verification requirements, and
predecessor uniqueness/domain rollback on both backends.

Sixteen deterministic PostgreSQL snapshot cases keep two live transactions open:
the older SERIALIZABLE/REPEATABLE READ snapshot and staged domain batch precede
the newer READ COMMITTED commit. They cover empty/existing chains, single/batch
events, both commit orders and fresh whole-operation retry. There is no
sleep-based ordering proof. Fixture-local triggers separately force the named
zero-row outcome, native 40001/40P01 and unrelated hash uniqueness; each asserts
full domain/audit rollback. They add no production failpoint. SQLite triggers
pin its distinct invariant outcome and preserve unrelated ID/hash statuses.
`tests/unit/error_translation_test.cpp` pins the base-error INTERNAL mapping.

Lead and worker-1 review requested the targeted SQLite invariant classification
and explicit migration-16 fixture selection; both landed in `101d0c1` and received
source re-review. Linux clang-tidy then found an unnecessary migration copy and
SQLite commit complexity 28/25. The final revision uses a const reference and a
named single-audit-insert invariant helper without changing behavior or relaxing
checks.

## Validation evidence

All commands use the slot-private environment from
`AGENT_SLOT=3 source scripts/agent/env.sh`, serial CTest and build parallelism 3.

- Red commit `2373da6`: `cmake --build --preset dev --target
  freezermanager_storage_repository_tests` passed. The direct gtest filter
  `'*AuditIntegrity*:*PredecessorUniqueness*:*AuditSnapshotTest*'` failed **7 SQLite
  cases**, with **23 PostgreSQL skips** because the local URL is unset.
- [Linux red run 40772242231](https://github.com/Yuxin-Ren-SZ/FreezerManager/actions/runs/40772242231)
  exercised PostgreSQL: gcc/clang dev and UBSan each selected **1,889**, with
  **22 expected failures and 1 existing PostgreSQL skip**. The failures were seven
  SQLite integrity cases, their seven PostgreSQL counterparts, and eight stale
  snapshot/new-writer-first cases. The stale commit returned success, retained
  its losing Lab rows and produced a chain the verifier rejected.
- SQLite invariant review regression, filter `'*AuditRepositoryTest.Sqlite*'`:
  **1 failed / 2 passed / 3 opposite-backend skips** before the targeted change;
  **3 passed / 3 skips / 0 failed** afterward.
- Functional `101d0c1`: focused CTest filter
  `'AuditRepositoryTest|AuditSnapshotTest|SqliteAuditEvent|SqliteBackendConformanceTest|PostgresBackendConformanceTest|ErrorTranslation.AuditPredecessor'`
  selected **99: 43 passed / 56 skips / 0 failed** (1.90 s).
- `cmake --build --preset dev` completed all 129 remaining build steps. Initial
  sandboxed `ctest --preset dev --output-on-failure` selected **1,908: 1,151
  passed / 279 skips / 478 failed** (127.32 s); gRPC integration fixtures could
  not bind loopback ports. This failed attempt is not counted as validation.
- The permitted unsandboxed rerun of that exact full CTest command selected
  **1,908: 1,629 passed / 279 skips / 0 failed** (185.99 s). The skips are
  275 PostgreSQL cases without a local URL and four PostgreSQL-only trigger
  contracts intentionally skipped for the SQLite fixture parameter.
- After the behavior-preserving tidy cleanup, the targeted build of repository,
  SQLite conformance and PostgreSQL conformance executables passed. CTest filter
  `'AuditRepositoryTest|AuditSnapshotTest|SqliteBackendConformanceTest|PostgresBackendConformanceTest'`
  selected **85: 29 passed / 56 skips / 0 failed** (1.08 s).
- Changed-file `clang-format --dry-run --Werror`, `git diff --check` and
  `tools/check-spdx-headers.sh` passed. Local changed-TU `clang-tidy -p
  out/build/dev ...` was attempted but exits on the documented LLVM17/macOS SDK
  builtin incompatibilities; it is not lint clearance.
- Local `scripts/agent/conan-install.sh asan`, `scripts/agent/conan-install.sh
  ubsan`, `scripts/agent/conan-install.sh tsan`, and matching `cmake --preset
  asan`, `cmake --preset ubsan`, `cmake --preset tsan` commands passed. Local
  sanitizer binaries/suites were **not** run.
  [Functional Linux run 40802942907](https://github.com/Yuxin-Ren-SZ/FreezerManager/actions/runs/40802942907)
  at exact head `101d0c1883088930956527be5da9fb14346bf120` passed gcc dev/release,
  clang dev, ASan, UBSan and TSan, with live PostgreSQL; web, format/SPDX and CLA
  also passed. Its actual clang-tidy job failed only on the two findings
  described above. ASan/TSan retain CI's `grpc_integration|e2e` exclusions; these
  green jobs do not claim excluded transport coverage.

At this handoff commit the final helper/reference/doc head's matching CI is
pending. The lead records final-head checks and review on #228/#234; no merge
gate, live-PostgreSQL requirement or actual clang-tidy requirement is waived.
