# #211 — lock session writers before security decisions

Issue: [#211](https://github.com/Yuxin-Ren-SZ/FreezerManager/issues/211).
PR: [#229](https://github.com/Yuxin-Ren-SZ/FreezerManager/pull/229).
Worker: worker-1 (Codex), slot 1, `fix/211-session-writers`.
Baseline: main `5d4c77602f4ce1dde8e1f36f31b51112cb321e4d`, including audit
prerequisite #228 and pure policy #227. No schema/proto/dependency change.

## Result and scope

The old activity refresh could write an earlier whole Session after another
request committed revocation or MFA completion. It could restore revoked
authority, clear MFA completion, or move activity backward. All production
Session updates/deletes now acquire the existing owning user lock and reread
current state before deciding and committing their audited mutation. A candidate
owner lookup before locking is used only to select the lock; it grants no
authority. Session repositories require that lock and reject ownership changes.

`begin_auth_transaction` fails closed by default. Concrete implementations sort
and deduplicate requested user UUIDs, acquire locks before repository factories
or repository reads, and record immutable per-request Locked/Missing statuses.
An empty request, an unrequested ID or a completed transaction is refused.

SQLite begins IMMEDIATE before user reads, reuses that database transaction at
commit, and performs real rollback on explicit rollback, destruction,
construction/factory failure and failed commit. A failed rollback closes the
private connection if it remains in a transaction. There is no app writer mutex,
fallback transaction or retry loop. Extended BUSY/LOCKED codes map to
Unavailable in both backend and Session repository paths.

PostgreSQL explicitly sets READ COMMITTED before security reads, independent of
the database default, locks existing user rows with FOR UPDATE in UUID order,
and sets a local five-second lock timeout. SQLSTATE 55P03 maps to Unavailable;
native 40001/40P01 retain SerializationFailure. Abort precedes pool release on
failure. Ordinary explicitly requested READ COMMITTED also honors that request.

Refresh uses the locked current row and current time, preserves security fields,
refuses revoked/missing rows, and recomputes monotonic/rate-limited activity.
Another writer's recent refresh produces no redundant write or audit. Storage
failure during an optional activity touch stays nonfatal. Background
`revalidate_token` remains read-only and non-renewing. Password-session creation
rechecks active status, unchanged password-checked credential bindings and
current TOTP enrollment under lock; it clears the password failure budget only
after the audited fresh-ID insertion commits.

Completed native accessors fail closed. PostgreSQL session-variable access uses
the guarded accessor; SQLite Session/ApiToken staged reads check the active
handle before returning cached provisional entities. Thus commit, rollback and
failed commit cannot expose rolled-back cache contents or unavailable native
work. Existing migration16 audit predecessor/append/atomicity safeguards remain
in place. This slice adds no persisted MFA state, replay/backoff/proof policy,
provider-policy integration, recovery RPC or deployment authorization. #179
and the later phases remain open.

## Regression and review evidence

`tests/paused_activity_backend.h` uses bounded promises to pause before the
write transaction acquires resources, after the earlier authority read; no
sleep-based ordering proof is used. Provider tests and both-backend repository
parameters exercise both write orderings, concurrent refresh eligibility,
current password binding/status/enrollment decisions, failure-budget retention,
owning-user/ownership guards, immutable Missing/finished statuses, bounded
contention and rollback/destructor/failed-audit release. Every persisted Session
change retains the common audit commit boundary.

R3 review found PostgreSQL work access after its optional had been reset. The
correction guards native access and the direct setter. The expanded failed-commit
case then genuinely exposed SQLite's provisional-cache bypass despite the
native guard; the shared staged-read guard corrected it. Tests directly assert
Session read/update/delete refusal after commit, rollback and failed commit,
retain persisted-row/audit assertions, and reacquire the lock while the failed
object remains alive. Both independent reviewers accepted exact source head
`ed470e8a25b113fc9f6d9eb078919b1eda3ce0b4`; the lead accepted source subject to
validation and handoff.

## Validation evidence

Commands use `AGENT_SLOT=1 source scripts/agent/env.sh`, worktree-private TMPDIR,
serial CTest and build parallelism 3. Local PostgreSQL URL is unset; its skips
are distinct from live PostgreSQL CI's intentional backend-contract skips.

- Original red tests (`b6f453c`, rebased `c7c24b5`): build of
  `freezermanager_auth_unit_tests` passed; `ctest --preset dev -R
  '^LocalAuthProviderTest.DelayedActivity' --output-on-failure` gave **3 failed /
  1 passed** before production fixes. Stale revocation/MFA/activity behavior was
  reproduced; injected audit rollback passed.
- Before concrete backend support, eight SQLite auth contract cases failed
  UnsupportedOperation and eight PostgreSQL parameters skipped. The default
  fail-closed interface selection `'^StorageInterface.UnsupportedAuth'` passed
  **2/2**.
- `ctest --preset dev -R
  '^SessionAuthErrorMapping.PostgreSqlRepositoryLockTimeoutIsUnavailable$'
  --output-on-failure` gave **1 failure** before the shared repository 55P03
  mapping fix.
- R3 completed/rolled-back repository reads gave **2 genuine baseline failures**
  before native accessor guards. A later narrow run with those guards present
  gave **1 failed / 7 passed / 3 PostgreSQL skips**, exposing failed-commit
  provisional cache reads. An extra temporary guard-removal mutation check is
  not counted as baseline/TDD evidence.
- `cmake --build --preset dev --target freezermanager_auth_unit_tests
  freezermanager_storage_repository_tests freezermanager_storage_unit_tests`
  passed. Focused CTest filter
  `'^(LocalAuthBackend\.|LocalAuthProviderTest\.|StorageInterface.UnsupportedAuth|SessionAuthErrorMapping\.|Backends/SessionRepositoryTest\.)'`
  selected **179: 134 passed / 45 PostgreSQL skips / 0 failed**.
- The final lifecycle/interface/error-mapping selection selected **11: 8 passed /
  3 PostgreSQL skips / 0 failed**, including direct terminal read/update/delete,
  setter and failed-commit cache assertions.
- [Historical Linux run 40926898291](https://github.com/Yuxin-Ren-SZ/FreezerManager/actions/runs/40926898291)
  at `ed470e8` was **red**. Both dev compilers and UBSan each selected **1,986:
  1,977 passed / 8 skipped / 1 failed**; ASan selected **1,491: 1,482 passed /
  8 skipped / 1 failed**; TSan selected **1,467: 1,458 passed / 8 skipped /
  1 failed**. Every runtime failure was the stale
  `TransactionResilienceTest.WriteAfterCommitIsRejected` expectation of silent
  discard after commit. Live PostgreSQL and the new #211 races otherwise passed.
  Its eight skips were seven intentional opposite-backend trigger parameters and
  one existing PostgreSQL concurrent-sample case, not unavailable PostgreSQL.
  Release/format/web/CLA passed. Actual tidy failed only on unchecked optional
  access in `PostgresTransaction::work()` despite the compound guard.
- Local `ctest --preset dev -R
  '^TransactionResilienceTest.WriteAfterCommitIsRejected$' --output-on-failure`
  reproduced **1/1 failed**. The correction expects ConstraintViolation and
  retains Lab1-present/Lab2-absent plus unchanged audit assertions. The optional
  guard now uses a stable reference and a direct engagement check, retaining
  the completed-state check without suppression. Lead source review accepted
  this two-file correction.
- `cmake --build --preset dev` completed the original **193/193** targets and
  the correction's incremental **26/26** targets. CTest filter
  `'^(TransactionResilienceTest\.|LocalAuthBackend\.|LocalAuthProviderTest\.|StorageInterface.UnsupportedAuth|SessionAuthErrorMapping\.|Backends/SessionRepositoryTest\.)'`
  then selected **199: 154 passed / 45 PostgreSQL skips / 0 failed** (9.02s).
- Repository-wide `clang-format --dry-run --Werror`, SPDX check and
  `git diff --check` passed before the functional checkpoint. The final two-file
  correction's format/diff checks also passed. The lead repeated the whole-repo
  format gate, `tools/check-spdx-headers.sh` and diff check after corrections;
  all exited 0.
- Lead-run native-loopback `ctest --preset dev --output-on-failure` selected
  **1,986: 1,684 passed / 302 skipped / 0 failed** (188.88s). The skips are
  **298 PostgreSQL URL-unset cases**, including 16 PostgreSQL audit snapshot
  cases, and **4 intentional opposite-backend SQLite audit trigger cases**.
  The earlier interrupted approval request created no log and
  returned no process ID; it is not counted as a test run. No duplicate run was
  started.
- Advisory `clang-tidy -p out/build/dev
  src/storage/postgres/PostgresBackend.cc` failed in the documented LLVM17/macOS
  SDK frontend incompatibilities: **48,874 warnings and 20 errors**, including
  missing clzg/ctzg/interference-size builtins. Its broken-AST diagnostics do
  not establish semantic cleanliness; actual Linux tidy remains required.
- `scripts/agent/conan-install.sh asan` (build=never) and `cmake --preset asan`
  passed. The lead-run focused ASan build completed **101/101** steps:

  ```sh
  cmake --build --preset asan --target \
    freezermanager_auth_unit_tests freezermanager_storage_unit_tests \
    freezermanager_sqlite_storage_unit_tests freezermanager_storage_repository_tests \
    freezermanager_transaction_resilience_tests freezermanager_backend_conformance_tests \
    freezermanager_sqlite_backend_conformance_tests freezermanager_postgres_backend_conformance_tests
  ```

- Lead-run `ctest --preset asan -LE 'grpc_integration|e2e' -R
  '^(LocalAuthBackend\.|LocalAuthProviderTest\.|StorageInterface\.|SessionAuthErrorMapping\.|TransactionResilienceTest\.|Backends/SessionRepositoryTest\.|Backends/AuditRepositoryTest\.|BackendConformanceTest\.|SqliteBackend\.|SqliteAuditEvent\.|SqliteBackendConformanceTest\.|PostgresBackendConformanceTest\.)'
  --output-on-failure` selected **331: 246 passed / 85 skipped / 0 failed**
  (15.44s). Its skips are **81 PostgreSQL URL-unset cases** and the same **4
  opposite-backend SQLite audit contracts**. This is focused ASan coverage, with
  the named transport labels excluded; no full local ASan suite or local UBSan
  or TSan pass is claimed.

At this handoff the final source/docs checkpoint's matching-head
compiler/live-PostgreSQL/full-sanitizer/actual-tidy checks remain pending.
The lead records final gates and exact-head source verification on #211/#229;
no pending or red check is waived.
