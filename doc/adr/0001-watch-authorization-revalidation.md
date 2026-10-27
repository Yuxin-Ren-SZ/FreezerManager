# ADR 0001: Revalidate watch authorization without renewing user activity

- Status: Accepted design; implementation pending in [#194](https://github.com/Yuxin-Ren-SZ/FreezerManager/issues/194).
- Date: 2026-10-26.
- Design issue: [#198](https://github.com/Yuxin-Ren-SZ/FreezerManager/issues/198).
- Decision: [lead acceptance](https://github.com/Yuxin-Ren-SZ/FreezerManager/issues/198#issuecomment-6429159909)
  of the [concrete proposal](https://github.com/Yuxin-Ren-SZ/FreezerManager/issues/198#issuecomment-6429140759).
- Parent: [#148 part 1](https://github.com/Yuxin-Ren-SZ/FreezerManager/issues/148).
- ADR requirement: [TODO.md Section N, N4](../../TODO.md#section-n--documentation-completeness-m0--ongoing)
  requires an ADR for a cross-module protocol change. This record covers this
  protocol; it does not complete the broader N4 documentation work.

## Context and evidence

Both watch handlers authorize at stream admission and retain that SessionContext
for every later query. Revocation, expiry, membership removal and permission
changes therefore do not stop an established stream. Simply calling the existing
middleware gate each poll would charge another admission token and could renew
the idle session indefinitely.

The inspected baseline is commit
[`fee540bc2b0cb1059e5a958c53907971c2bfaf20`](https://github.com/Yuxin-Ren-SZ/FreezerManager/commit/fee540bc2b0cb1059e5a958c53907971c2bfaf20),
which includes merged #178 and #44. Source anchors below are pinned to that
baseline so implementation changes do not alter the evidence.

| Evidence | Source |
|---|---|
| `validate_token` is incorrectly documented as read-only | [IAuthProvider.h:44](https://github.com/Yuxin-Ren-SZ/FreezerManager/blob/fee540bc2b0cb1059e5a958c53907971c2bfaf20/src/auth/IAuthProvider.h#L44) |
| Session validation checks live user status/authz_version, expiry, then updates last_seen_at | [LocalAuthProvider.cc:443](https://github.com/Yuxin-Ren-SZ/FreezerManager/blob/fee540bc2b0cb1059e5a958c53907971c2bfaf20/src/auth/LocalAuthProvider.cc#L443) |
| Expiry and rate-limited activity update are distinct helpers | [LocalAuthProvider.cc:490](https://github.com/Yuxin-Ren-SZ/FreezerManager/blob/fee540bc2b0cb1059e5a958c53907971c2bfaf20/src/auth/LocalAuthProvider.cc#L490) |
| Cached permission grants require current authz_version; MFA comes from the live session | [LocalAuthProvider.cc:524](https://github.com/Yuxin-Ren-SZ/FreezerManager/blob/fee540bc2b0cb1059e5a958c53907971c2bfaf20/src/auth/LocalAuthProvider.cc#L524) |
| API tokens have live revocation/expiry checks and fresh, intersected grants | [LocalAuthProvider.cc:568](https://github.com/Yuxin-Ren-SZ/FreezerManager/blob/fee540bc2b0cb1059e5a958c53907971c2bfaf20/src/auth/LocalAuthProvider.cc#L568), [context construction:615](https://github.com/Yuxin-Ren-SZ/FreezerManager/blob/fee540bc2b0cb1059e5a958c53907971c2bfaf20/src/auth/LocalAuthProvider.cc#L615) |
| Both admission gates enforce registry agreement and apply the data-tier limiter | [AuthMiddleware.cc:171](https://github.com/Yuxin-Ren-SZ/FreezerManager/blob/fee540bc2b0cb1059e5a958c53907971c2bfaf20/src/rpc/AuthMiddleware.cc#L171), [credential gate:207](https://github.com/Yuxin-Ren-SZ/FreezerManager/blob/fee540bc2b0cb1059e5a958c53907971c2bfaf20/src/rpc/AuthMiddleware.cc#L207) |
| Sample watch retains opening context; its registry entry is SampleRead | [SampleServiceImpl.cc:1038](https://github.com/Yuxin-Ren-SZ/FreezerManager/blob/fee540bc2b0cb1059e5a958c53907971c2bfaf20/src/server/SampleServiceImpl.cc#L1038), [registration:543](https://github.com/Yuxin-Ren-SZ/FreezerManager/blob/fee540bc2b0cb1059e5a958c53907971c2bfaf20/src/server/SampleServiceImpl.cc#L543) |
| Audit watch retains opening context; its credential gate precedes manual scope checks | [AuditServiceImpl.cc:397](https://github.com/Yuxin-Ren-SZ/FreezerManager/blob/fee540bc2b0cb1059e5a958c53907971c2bfaf20/src/server/AuditServiceImpl.cc#L397), [registration:187](https://github.com/Yuxin-Ren-SZ/FreezerManager/blob/fee540bc2b0cb1059e5a958c53907971c2bfaf20/src/server/AuditServiceImpl.cc#L187) |
| Deployment SystemAdmin is identified by global LabProvision | [AuditServiceImpl.cc:36](https://github.com/Yuxin-Ren-SZ/FreezerManager/blob/fee540bc2b0cb1059e5a958c53907971c2bfaf20/src/server/AuditServiceImpl.cc#L36) |
| SSE forwards a non-OK gRPC finish as an error frame then closes | [SseBridge.h:103](https://github.com/Yuxin-Ren-SZ/FreezerManager/blob/fee540bc2b0cb1059e5a958c53907971c2bfaf20/src/rest/SseBridge.h#L103), [REST envelope](https://github.com/Yuxin-Ren-SZ/FreezerManager/blob/fee540bc2b0cb1059e5a958c53907971c2bfaf20/src/rest/RestErrorTranslation.h#L102) |

## Decision: explicit non-renewing provider validation

Add this pure virtual member to `fmgr::auth::IAuthProvider`:

```cpp
virtual SessionContext revalidate_token(std::string_view bearer_token) = 0;
```

It returns a newly validated SessionContext with the same credential and grant
semantics as `validate_token`, but records no user activity and performs no
storage writes. In-memory permission-cache lookup/rebuild is allowed under the
provider's existing concurrency contract. There is no default implementation
that delegates to activity-renewing validation.

`LocalAuthProvider` implements both public methods through shared internal
validation helpers with a named activity mode. Normal `validate_token` retains
its current rate-limited `last_seen_at` update; correct the interface comment to
describe that behavior. `revalidate_token` never calls
`update_last_seen_if_needed`, opens an activity-write transaction, changes
`created_at`, or extends either expiry deadline. The opening client RPC still
counts as activity. Later ordinary client RPCs may update activity in the usual
way, even while a watch exists.

Each revalidation checks the authoritative session/token row and token hash,
revocation, current user status and current time. Session idle/absolute limits
remain the existing configured limits (defaults 12 hours / 7 days), including
their existing boundary comparison. The live MFA flag is returned. Read fresh
`users.authz_version` before accepting cached grants; a changed version must
force grant rebuilding even inside the cache TTL. Retain existing cache locking
and validation order. The opening SessionContext is never an authority cache for
future polls.

API tokens follow their existing dispatch path: current revocation, expires_at
and active-user checks; freshly resolved user grants intersected with token
permission scope and optional lab restriction; existing MFA-complete semantics.
No Session row is created for an API token and session idle renewal is not
invented. Do not introduce an API-token grant cache that could bypass current
permission changes.

The only existing implementations are `LocalAuthProvider` and the compile-check
mock in [auth_types_test.cpp:221](https://github.com/Yuxin-Ren-SZ/FreezerManager/blob/fee540bc2b0cb1059e5a958c53907971c2bfaf20/tests/unit/auth_types_test.cpp#L221).
Both must implement the new method; extend the mock's compile/dispatch assertion.
Future providers must explicitly implement the same non-renewing contract.
Handlers continue to depend on interfaces and middleware only.

## Decision: admission and revalidation are separate middleware operations

Add these members to `fmgr::rpc::AuthMiddleware`:

```cpp
[[nodiscard]] auth::SessionContext
reauthorize(const RpcCall& call, core::Permission required_perm,
            std::optional<core::LabId> lab_id = std::nullopt) const;

[[nodiscard]] auth::SessionContext
reauthenticate(const RpcCall& call, CredentialRule rule) const;
```

Keep `authorize` and `authenticate` as admission operations. Share private gate
bodies so the new operations use the same permission, credential and MFA checks,
substituting `IAuthProvider::revalidate_token` and omitting only limiter charging.
Do not expose a defaulted boolean that lets ordinary handlers disable admission
accounting.

| Operation | Provider method | Data-tier charge |
|---|---|---|
| `authorize` | `validate_token` | One, at its existing position before token validation |
| `authenticate(TokenAndMfa)` | `validate_token` | One, at its existing position after validation/MFA |
| `reauthorize` | `revalidate_token` | None |
| `reauthenticate(TokenAndMfa)` | `revalidate_token` | None |

Each stream opening passes through its ordinary gate exactly once. Failed
admission keeps the gate's existing accounting order. Background ticks never
consume limiter tokens. Existing `TokenOnly` behavior remains outside the
data-tier budget; its reauthentication equivalent validates without requiring
MFA. `reauthenticate(None)` is invalid, just as `authenticate(None)` is today.

Registry agreement remains the first check on every operation. `reauthorize`
requires the same Permission registration as `authorize`; `reauthenticate`
requires the same Credential registration and exact rule as `authenticate`.
Missing registrations and kind/rule/permission mismatches remain
`RpcRegistryMismatch` and terminate as INTERNAL. Retain the existing empty-method
exception for tests/tooling; served watches retain their original nonempty
method identity in `RpcCall`.

## Decision: refresh authority before every polling batch

Capture the original `RpcCall` and requested lab scope once. Keep existing
opening authorization and scope checks. At the top of every loop, before opening
the polling transaction, obtain a fresh SessionContext using the new operation:

- Sample watch: `reauthorize(call, Permission::SampleRead, lab_id)`.
- Audit watch: `reauthenticate(call, CredentialRule::TokenAndMfa)`, followed by
  `has_for_lab(lab, Permission::AuditRead)` for a scoped feed, or the existing
  `is_system_admin` check (global `LabProvision`) for an unscoped feed.

Audit retains its TokenAndMfa registry entry. Registering AuditRead for the
unscoped branch would misstate its actual gate. Repeat both manual audit scope
checks on every tick; a fresh token alone does not authorize the feed.

Inject RLS variables from that fresh context into every new transaction before
repository access. Keep the original requested lab filter; losing membership
must end the feed rather than widening its query. Preserve cursor ordering,
duplicate suppression, cancellation, PHI exclusions and transaction behavior.
No concrete provider/backend access or raw SQL is added to either handler.

On a failed credential/MFA/permission check, leave the loop through existing
exception translation before polling or emitting that batch. Revoked/expired or
invalid credentials map to UNAUTHENTICATED; lost membership/permission maps to
PERMISSION_DENIED. MFA retains UNAUTHENTICATED with its existing MFA marker;
registry disagreement remains INTERNAL. Storage failures also terminate using
existing status translation; a cached opening context is never a fallback.

The normal check cadence remains approximately one second: ten 100 ms
cancellation-aware sleep slices between polls in both handlers. There is no
extra 30-second authority cache. This is a check cadence, not a hard one-second
end-to-end revocation guarantee: database latency, scheduler delay, a large batch
or blocked synchronous `writer->Write` can delay the next check and finish.
Revocation committed after a successful check can race that batch. Frames already
written to gRPC or queued in the SSE event loop/transport cannot be recalled;
they can arrive after revocation or after the server observes invalidation.
The guarantee is that a failed check initiates no new polling batch or payload
write. Strict revocation linearization, queued-frame purging and backpressure
redesign are separate work.

## REST contract and paired client work

The existing bridge has already committed HTTP 200 for an open SSE stream. A
non-OK gRPC finish produces `event: error` with the existing RESTerror JSON
envelope in `data:`, then closes the stream. Revocation/expiry uses
`{"code":"UNAUTHENTICATED","message":"..."}`; permission loss uses
`PERMISSION_DENIED`. Preserve the special `MFA_REQUIRED` envelope code and avoid
client parsing of message text. Disconnect/cancellation may permit closure only.

[#182](https://github.com/Yuxin-Ren-SZ/FreezerManager/issues/182) must consume the
terminal code on an established stream: UNAUTHENTICATED ends the client session,
clears its state and stops reconnecting; PERMISSION_DENIED ends that feed and
shows lost access without declaring unrelated credentials expired. Preserve the
client's existing MFA flow for MFA_REQUIRED. Ordinary unary HTTP 401 responses
already end the client session. Native EventSource hides HTTP status on transport
errors, so this slice relies on the terminal SSE envelope and closure, including
when the current bridge rejects invalid opening credentials after committing
HTTP 200. Status-opaque transport errors remain retryable. A server that closes
an established stream cannot replace its already-sent status with HTTP 401.

## Implementation test plan

Implementation in #194 follows TDD, starting with failing regressions. All tests
use synthetic data and bounded waits; no PHI or credential values enter logs.

1. Extend [local_auth_provider_test.cpp](../../tests/unit/local_auth_provider_test.cpp)
   to cover fresh valid revalidation, revoked session, disabled user, current MFA,
   and both expiry limits. Use committed repository timestamp mutation, as the
   existing `backdate_session` tests do. Plant last_seen_at older than the update
   interval but inside idle TTL, revalidate repeatedly, and assert exact unchanged
   stored timestamps. Plant it beyond idle TTL and assert TokenExpired; separately
   move created_at past absolute TTL while last_seen_at is recent. Keep the normal
   validation activity-update regression. No long sleeps or new production clock
   interface are required. Test the existing exact expiry boundaries faithfully.
2. Warm the permission cache, remove membership or a RolePermission in a committed
   mutation that bumps authz_version, then call revalidation before cache TTL
   expiry and assert fresh grants. Test API-token revocation/expiry, disabled user,
   permission/lab intersections and grant removal. Reusing old SessionContext or
   relying on TTL expiration must fail these tests.
3. Extend [auth_types_test.cpp](../../tests/unit/auth_types_test.cpp) and
   [auth_middleware_test.cpp](../../tests/unit/auth_middleware_test.cpp) for the new
   interface/dispatch, both registry gate kinds, wrong rule/permission, MFA and
   fresh scoped/global grants. An existing transaction double can capture RLS
   variables and prove that newly returned context drives injection. With a
   capacity-one, zero-refill limiter, admit once, perform many revalidation ticks
   successfully, and prove a second ordinary admission is rate-limited. Test both
   sample-style permission admission and audit-style credential admission.
4. Extend both [sample](../../tests/integration/sample_service_integration_test.cpp)
   and [audit](../../tests/integration/audit_service_integration_test.cpp) gRPC watch
   fixtures. Receive an initial uniquely identified marker as the open-stream
   barrier; then commit revocation, backdated idle expiry, backdated absolute
   expiry, membership removal, or removal of the required read permission. Drain
   Read until completion and require the exact non-OK Finish status within a
   bounded test deadline (for example five seconds on an unloaded fixture), not
   DEADLINE_EXCEEDED or cancellation. The deadline is test tolerance, not a
   production latency promise. A Member-to-ReadOnly change alone is not a sample
   permission-drop test: both retain SampleRead.
5. Avoid timing races when proving absence of a post-invalidation marker. If an
   initial marker alone does not establish ordering of the next check, use a
   test-only delegating IAuthProvider wrapper with a condition-variable barrier
   before the next revalidation read. Commit invalidation and a later synthetic
   event before releasing it. Assert no later marker, terminal status, and prompt
   completion. Do not add production hooks or an application SQLite writer mutex.
   Keep a valid multi-poll control, an unscoped SystemAdmin audit control, unscoped
   non-admin rejection, and termination when the global admin marker is removed.
6. Extend [rest_gateway_integration_test.cpp](../../tests/integration/rest_gateway_integration_test.cpp)
   raw-socket SSE tests: receive an initial payload, log out/revoke the opening
   browser session, then assert a terminal error frame with RESTerror
   UNAUTHENTICATED and EOF. The existing helper returns on a needle; extend the
   test helper to distinguish a terminal frame, peer EOF and timeout. A timeout
   does not prove closure. Include permission failure and preserve positive feed
   behavior; generic client disconnect can be tested as closure-only.
7. Exercise shared provider cache/gate logic concurrently in unit tests using
   synchronized starts and joined threads; keep limiter installation lifetime
   controlled by the existing RAII guard. Run focused tests, full
   `ctest --preset dev`, and helper-configured TSan with
   `ctest --preset tsan -LE 'grpc_integration|e2e'`. Real-gRPC watch and REST gateway
   tests are labelled grpc_integration and excluded from TSan, as recorded in
   [tests/integration/CMakeLists.txt:349](https://github.com/Yuxin-Ren-SZ/FreezerManager/blob/fee540bc2b0cb1059e5a958c53907971c2bfaf20/tests/integration/CMakeLists.txt#L349).
   Pure/interface doubles and unit provider/middleware tests give TSan coverage
   of changed concurrent seams; a green excluded run does not validate actual
   gRPC/Drogon transport threads. Report exclusions, local PostgreSQL skips and
   unavailable tooling exactly; require applicable Linux CI and formatting/SPDX.

## Scope and consequences

This ADR changes no executable behavior. #194 implements the accepted protocol
after this design and #192 are merged. #148 remains open for WhoAmI expiry
visibility and expired-row cleanup; #179 MFA replay/backoff is separate. No new
TTL, proto field, schema migration, dependency or CI change is authorized here.

The explicit interface adds one provider obligation and two middleware entry
points. In exchange, ordinary request activity/accounting remains stable and
background polling can validate current authority without sustaining a session.
Reusing admission each tick, validating only the opening context, calling
LocalAuthProvider directly from handlers, and adding public skip flags were
rejected because they violate one or more of these contracts.
