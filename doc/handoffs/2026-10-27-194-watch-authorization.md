# #194 — non-renewing watch authorization revalidation

Worker: worker-3 (Codex). PR: [#206](https://github.com/Yuxin-Ren-SZ/FreezerManager/pull/206).
Implements [#148 part 1](https://github.com/Yuxin-Ren-SZ/FreezerManager/issues/148),
following [ADR0001](../adr/0001-watch-authorization-revalidation.md). Parts 2/3,
MFA replay/backoff #179, and strict-UBSan fixture #202 remain separate.

## Result

`IAuthProvider::revalidate_token` is an explicit pure virtual obligation.
`LocalAuthProvider` shares normal validation through private named activity
modes. Both paths retain authoritative hash/revocation/expiry/active-user checks,
live MFA, authz-version cache invalidation and current PAT permission/lab
intersections. Background checks do not update timestamps, open activity write
transactions, or create sessions for PATs. Ordinary validation still records
activity at its existing interval.

`AuthMiddleware::reauthorize` and `reauthenticate` share private gate bodies with
ordinary admission. Registry agreement, MFA and permissions remain enforced;
background checks use the new provider operation and spend no limiter tokens.
Existing admission accounting order remains unchanged. There is no public
skip-accounting flag.

Both sample and audit watches preserve their admission gate once, then obtain a
fresh context before every polling transaction. Sample requires scoped
SampleRead. Audit repeats scoped AuditRead or unscoped global LabProvision
(SystemAdmin) checks after its TokenAndMfa gate. Every poll injects fresh RLS
variables. Failed checks leave through existing exception translation before a
new polling batch or payload write. Cursor ordering, cancellation and PHI
exclusion are unchanged.

The normal cadence remains ten 100 ms cancellation-aware slices between polls.
This is not a hard one-second delivery/revocation guarantee: an in-flight batch,
slow database/scheduler, blocked synchronous write and queued gRPC/SSE frames can
outlive invalidation. Revocation after a successful check can race that batch.
No queued-frame purge, backpressure redesign or strict linearization is claimed.

The SSE production bridge needed no change. It emits the existing RESTerror JSON
frame and ends the HTTP response body. For HTTP/1.1 chunking, body EOF requires a
zero chunk and complete trailer terminator; a keepalive TCP socket can remain
open. Drogon refuses async streams for `Connection: close`, so peer TCP EOF is
not the response-body completion contract. The lead accepted this clarification
[in #194](https://github.com/Yuxin-Ren-SZ/FreezerManager/issues/194#issuecomment-6452607712),
and ADR0001 now records it. Timeout, reset and client close do not pass the test.
Client #182/#203 already handles UNAUTHENTICATED by ending the session and
PERMISSION_DENIED by ending the affected feed, retaining the existing MFA flow.

## Tests and evidence

Tests-first commit `d50ebff` added established-stream revocation regressions
before changing production handlers. On old handlers:

```sh
ctest --preset dev -R 'Watch.*StopsAfterOpeningSessionIsRevoked' --output-on-failure
```

Result: 0 passed / 2 failed. Both received an opening marker, committed session
revocation, then reached DEADLINE_EXCEEDED instead of UNAUTHENTICATED.

New coverage lives in:

- [provider tests](../../tests/unit/local_auth_provider_test.cpp): exact unchanged
  stored activity timestamps/audit count under concurrent revalidation; idle and
  absolute expiry, boundary-adjacent checks, warm-cache revocation/user disable,
  live MFA, membership/role invalidation; PAT hash, expiry, revocation, disabled
  user, fail-closed scopes, lab restriction, fresh grant removal and no session
  creation or activity writes. Existing normal activity-update tests remain.
- [middleware tests](../../tests/unit/auth_middleware_test.cpp): permission and
  credential registry disagreements before credential/limiter work, scoped and
  global grants, MFA/TokenOnly, refreshed RLS variables, and synchronized readers
  behind capacity-one zero-refill limiters. Background checks succeed after
  admission while another normal admission remains rate-limited.
- [interface mock](../../tests/unit/auth_types_test.cpp): explicit new method and
  virtual dispatch assertion.
- [sample](../../tests/integration/sample_service_integration_test.cpp) and
  [audit](../../tests/integration/audit_service_integration_test.cpp) streams:
  revocation, idle/absolute expiry, membership/read-permission loss, MFA reset,
  disabled user, authorized multi-poll controls, and unscoped admin control,
  rejection and downgrade. A test-only delegating provider/CV
  [barrier](../../tests/integration/watch_auth_test_helpers.h) pauses before the
  next authoritative read; invalidation and a later marker commit before release.
  No later marker is delivered, and exact terminal status is required within the
  bounded client deadline. There are no production hooks or sleep-only ordering
  proofs.
- [REST tests](../../tests/integration/rest_gateway_integration_test.cpp): both
  feeds receive an opening marker before logout or permission loss; raw socket
  chunk decoding requires exact terminal JSON code and HTTP body EOF. Permission
  grants are restored with audited typed-repository inserts on scope exit so
  same-process execution remains isolated. Explicit cursors avoid a race between
  HTTP response headers and gRPC stream admission.

Commands use `AGENT_SLOT=3 source scripts/agent/env.sh`, build parallelism 3 and
CTest parallelism 1, shared cached dependencies with `--build=never`.

```sh
cmake --build --preset dev
ctest --preset dev -R 'Revalidation|Watch.*Stops|Watch.*MultiplePolling|WatchAuditFeedUnscoped' --output-on-failure
```

Initial focused result: 40 passed / 0 failed. Two additional PAT cases subsequently
passed in the provider subset. Four REST terminal cases also passed independently.

```sh
out/build/dev/tests/integration/freezermanager_rest_gateway_integration_tests --gtest_filter='RestGatewaySse.*BodyEof'
```

Same-process result: 4 passed / 0 failed, proving grant restoration across cases.

```sh
scripts/agent/conan-install.sh tsan
cmake --preset tsan
cmake --build --preset tsan
ctest --preset tsan -LE 'grpc_integration|e2e' --output-on-failure
```

Result: 1,286 selected; 1,054 passed, 232 skipped, 0 failed (82.59 s). Skips are
PostgreSQL tests because `FMGR_TEST_POSTGRES_URL` is unset. The real gRPC/Drogon
transport and e2e tests are excluded by the required label expression; this run
covers the concurrent provider/cache/gate seams, not transport threads. The final
REST-only changes do not change the selected TSan seams. Local ASan was not run;
applicable Linux ASan/UBSan/TSan CI remains required.

```sh
git fetch origin
git rebase origin/main
cmake --build --preset dev
ctest --preset dev --output-on-failure
```

Final full dev result: 1,781 selected; 1,549 passed, 232 skipped, 0 failed.
All skips are PostgreSQL tests without `FMGR_TEST_POSTGRES_URL` (170.48 s).
After this full run, an equivalent test-only HTTP request string-append cleanup
was rebuilt and the four same-process REST cases were rerun successfully.

```sh
clang-format --dry-run --Werror $(git diff --name-only origin/main -- '*.cc' '*.cpp' '*.h')
tools/check-spdx-headers.sh
git diff --check
```

Formatting uses clang-format 17.0.6; all three checks pass. Local
`clang-tidy -p out/build/dev src/rpc/AuthMiddleware.cc` was attempted with 17.0.1
but fails parsing the current macOS libc++ SDK (`__builtin_clzg`/`__builtin_ctzg`
and related errors). This is not successful lint coverage; matching-head Linux
`clang-tidy` and the full applicable CI matrix are required before merge.
No local npm check was needed for this server-only change; CI's web job runs.

## Coordination

No proto, schema, dependency, CI, TTL, web, cleanup or WhoAmI changes. No lock
needed. Rebased onto `origin/main` (`f5611b7`) before final full dev validation.
Only #194 is closed by the PR. The lead owns review, merge and roadmap updates.

## CI lint follow-up

Linux matrix jobs passed at `fbeed09`; the actual
[clang-tidy job](https://github.com/Yuxin-Ren-SZ/FreezerManager/actions/runs/40578055649/job/122450054051)
reported only two `performance-inefficient-vector-operation` errors on the
four-thread `readers.emplace_back` loops. Each test now calls
`readers.reserve(4)` before that loop. This changes test setup only; no production
code or assertions changed.

```sh
cmake --build --preset dev
ctest --preset dev -R 'RevalidationDoesNotRenewActivityOrWriteWithConcurrentReaders|RevalidationDoesNotChargeEitherAdmissionGateConcurrently' --output-on-failure
clang-format --dry-run --Werror tests/unit/local_auth_provider_test.cpp tests/unit/auth_middleware_test.cpp
git diff --check
```

Result: build passed; 2 selected, 2 passed, 0 skipped/failed (0.26 s). Formatting
and diff checks pass. Prior full dev/TSan evidence above remains recorded; the
lead will require the complete matching-head Linux matrix and actual clang-tidy
before merging the lint follow-up.
