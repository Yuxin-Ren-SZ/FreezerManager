# Handoff note — 2026-09-29, `RevokeSession` ownership check (#77, worker-3)

`SessionServiceImpl::RevokeSession` called `validate_authed` and then handed the
caller-supplied `session_id` straight to `IAuthProvider::revoke_session`, whose
`LocalAuthProvider` implementation soft-deletes by id with no owner predicate
(`LocalAuthProvider.cc:286-294`). Any authenticated caller could therefore force
any session it could name to log out. The RPC's `AuthMiddleware` registry entry
claimed `core::Permission::SessionRevoke`, but no `authorize()` call and no
ownership check existed anywhere on the path — the registry entry was
documentation for an enforcement point that was never written.

The first round fixed that with an ownership check plus the SystemAdmin
exception the proto documents. Review then found the exception was **wider than
the proto**: `session.revoke` was lab-grantable, and `RevokeSessionRequest`
carries no lab, so a lab-scoped grant was spendable deployment-wide. Round two
closes that. Both rounds are in this branch; the note below describes the final
state and keeps the review's finding on the record, because the reasoning is the
useful part.

**Severity is P1, a targeted denial of service, not data access**, and both
bounds were verified rather than assumed. Ids cannot be enumerated through the
API: `ListSessions` filters on `UserId == sctx.user_id`, and the request's
`user_id` filter — which the proto documents as the SystemAdmin escape hatch —
is not implemented, so there is no cross-user listing to harvest ids from. And
the revoke is not an existence oracle: the deny decision is made on ownership,
never on whether the row was found, so a live foreign id and a never-issued id
produce byte-identical denials.

## The second finding: the exception was reachable from lab scope

`holds_session_revoke_anywhere` (the first round's helper) returned true when
`session.revoke` appeared in **any** lab's grant set. The comment justified that
as "by default only the SystemAdmin role holds it, per lab" — and the word
*default* was doing security work it cannot do, because the catalog deliberately
lets a lab admin create custom roles. The chain, each link verified in code:

1. `is_global_only_permission(SessionRevoke)` returned **false**
   (`permissions.h`), pinned by `permission_catalog_test.cpp`.
2. `RoleServiceImpl::GrantPermission` refuses only **global-only** permissions
   for a lab-owned role (`:380`).
3. `GrantPermission` authorises on `UserManageRoles` **for that lab** (`:372`),
   and `LabAdmin` holds `UserManageRoles`.
4. A non-global-only permission lands in `permissions_by_lab`
   (`LocalAuthProvider.cc:171-176`).
5. So one lab's grant satisfied the `any_of`, and the RPC — which has no lab —
   evaluated it deployment-wide.

**The attack, as a `LabAdmin` of lab A only:** `CreateRole(lab_A)` →
`GrantPermission(role, session.revoke)` → assign to self →
`RevokeSession(<a session belonging to a user who is only in lab B>)`. That
contradicts `session.proto:12-14` ("unless the caller is a SystemAdmin").

## Changed

- `src/server/SessionServiceImpl.cc` — `caller_owns_session()` reads the target
  row through the typed query DSL (`find_by_id`) inside a `ReadCommitted`
  transaction, and compares `user_id` with the caller's. It uses `find_by_id`
  rather than a `Query<Session>` because that path returns tombstoned rows,
  which keeps a repeated revoke of one's own session the idempotent no-op
  `IAuthProvider` promises. `RevokeSession` then allows the call when the caller
  owns the session **or** `sctx.has_global(Permission::SessionRevoke)`, and
  otherwise throws `auth::PermissionDenied` before `auth_.revoke_session(...)`.
- `src/core/permissions.h` — `SessionRevoke` moved into the global-only set
  (`is_global_only_permission`), alongside `SampleDeleteHard`, `BackupRun`,
  `KeyRotate` and `LabProvision`, with the reason recorded above the switch.
- `tests/unit/permission_catalog_test.cpp` — the assertion that pinned the old
  classification flips to `EXPECT_TRUE`. That test was pinning the
  misclassification, so changing it is part of the fix, not a workaround.
- `tests/integration/session_service_integration_test.cpp` (new, 9 tests) and
  its target in `tests/integration/CMakeLists.txt`.

Nothing else changed. No proto edit (so no `lock:proto`), no migration, no
`AuthMiddleware.*` / `FreezerServer.*` / `SampleServiceImpl.cc` touch.

## Decisions

- **The check lives in the service layer, not in `LocalAuthProvider`.** (1)
  Owner-or-permission is authorisation *policy*, and the provider is a
  storage-facing primitive with no permission model — its siblings
  (`verify_totp`, `revoke_all_sessions`) take ids the same way. (2)
  `IAuthProvider::revoke_session` is also called by `AuthServiceImpl::Logout`
  (`AuthServiceImpl.cc:137`) with the caller's own session id, where an owner
  predicate can never fail. (3) The SystemAdmin exception cannot be expressed
  inside the provider without widening `IAuthProvider` with a permission
  parameter. (4) The Postgres schema enables RLS on lab-scoped tables only —
  `sessions` has **no** policy — so a service-layer read of the target row is
  not filtered, and the check behaves identically on SQLite and Postgres. (5)
  Every other RBAC decision in this repo already lives in the service layer,
  with `RoleServiceImpl.cc:130` (`sctx.has_for_lab(...)`) as the precedent for a
  secondary check on the session context.
- **It is an ownership check, not a permission grant — plus the one exception
  the code documents.** Making the RPC require `session.revoke` outright would
  have been the wrong fix: a `Member` holds no `session.*` grant, and logging
  *yourself* out is the reason the RPC exists. So ownership alone authorises a
  self-revoke; the cross-user path is the exception the proto documents
  (`session.proto:12-14`) and the catalog describes ("Revoke another user's
  sessions").
- **The exception is scoped by making the permission global-only, not by
  weakening the helper.** The narrower alternative — keep the permission
  lab-grantable and check `has_global` — would leave a permission a lab admin
  can grant and that then silently does nothing. Classifying it correctly closes
  the grant path *and* the evaluation path, and it collapses the helper into
  `has_global`, so the special case disappears rather than growing.
- **Denying with `PERMISSION_DENIED`, not `NOT_FOUND`,** is deliberate: it is
  the honest answer, and it cannot become an existence oracle.
- **A grant that predates this change stays inert, not deleted.** A lab-owned
  role row carrying `session.revoke` — which the vulnerable version could create
  through the API — is skipped for non-SystemAdmin roles by
  `resolve_permissions`, so it grants nothing. Deleting such rows would need a
  migration (`lock:migration`); the behaviour is pinned by
  `LabScopedSessionRevokeGrantCannotRevokeAnotherUsersSession` so an upgrade
  cannot silently resurrect it.
- **SystemAdmin reach is unchanged and intended.** `SessionRevoke` appears in
  exactly one built-in role block, `SystemAdmin` (`permissions.h:232`), which is
  deployment-level; `resolve_permissions` promotes global-only permissions to
  `global_permissions` for a SystemAdmin-kind role. Cross-lab revocation by a
  SystemAdmin is inherent to the role, and `CreateRole` refuses SystemAdmin-kind
  custom roles at the door (`RoleServiceImpl.cc:92-97`), so no lab admin can
  reach that promotion.
- **Session-id exposure was checked, as the issue asked: no leak to anyone not
  already allowed to revoke the session.** `ListSessions` is caller-scoped and
  there is no admin view (`SessionService` has exactly two RPCs; REST exposes
  only `/api/v1/session/list` and `/api/v1/session/revoke`). Nothing under
  `src/obs/` touches sessions, so no session id reaches the JSON logs. Audit
  rows *do* carry the id — `entity_id`, `actor_session_id`, and the full session
  entity in `before_json`/`after_json` (`core/session.h:107`) — but session
  mutations are written with `lab_id = NULL` (the `MutationContext` built by
  `make_ctx` sets no lab), and `GetAuditEvent` (`AuditServiceImpl.cc:262`) plus
  the lab-scoped `ListAuditEvents`/`ExportAuditLog` all treat a lab-less row as
  system-admin-only. A `LabAdmin` holding `audit.read` cannot read them; only a
  deployment SystemAdmin can, and that is the principal the exception already
  allows. So the exploit needs a *known* id, and the P1-not-P0 bound stands.
- **Noted while verifying, deliberately not filed as a finding:** the same
  SystemAdmin-visible session audit rows contain `token_hash`. It is a BLAKE2b
  hash of a 256-bit random token, not a credential, so it is recorded here
  rather than filed.
- **`ListSessions`' registry entry and proto comment are left alone.** It is
  registered against `SessionRevoke` and its proto says "or any user for
  SystemAdmin", but the method scopes to the caller and ignores the request's
  `user_id`. That is stricter than documented and has no security effect;
  changing it means a proto edit (no `lock:proto` held) or a new cross-user
  listing feature. It is the mismatch class #60 exists to make structural, and
  it is written up in PR #81 instead of being smuggled into this diff.
- **`caller_owns_session`'s dependence on RLS is recorded in a comment.** The
  read (and therefore the self-logout path) is unfiltered only because
  `sessions` carries no RLS policy; a future migration that scoped sessions to a
  lab would change what it can see.

## Tests

`tests/integration/session_service_integration_test.cpp` — 9 tests, a real
in-process `FreezerServer` on a random port over gRPC, registered in
`tests/integration/CMakeLists.txt` as
`freezermanager_session_service_integration_tests` with the `grpc_integration`
label like the other in-process-server suites. The fixture seeds two labs and
six principals: a SystemAdmin, two `Member`s and a `LabAdmin` in lab1, a
`supervisor` whose lab1 membership points at a **custom** lab role carrying
`session.revoke`, and a `Member` (`carol`) in lab2 only. The custom role is
seeded at the repository layer because the point of the test that reads it is
that such a row must stay inert even when it exists.

- `MemberCannotRevokeAnotherUsersSession` — the cross-user exploit.
- `MemberCannotRevokeUnknownSessionId` — the same path with no row at all.
- `DeniedRevokeDoesNotRevealWhetherTheSessionExists` — guard: the denial for a
  live foreign id and for a never-issued id is identical, error message
  included.
- `LabAdminCannotGrantSessionRevokeToALabRole` — round two: the grant must be
  refused with `FAILED_PRECONDITION`, as it is for the other global-only
  permissions.
- `LabScopedSessionRevokeGrantCannotRevokeAnotherUsersSession` — round two's
  regression test, and the case the first suite could not express: a lab-owned
  role that *does* carry `session.revoke` must not unlock a cross-lab revoke.
- `MemberCanRevokeOwnSession` — the logout path: `OK`, and the token really
  stops working (a silent no-op would also have passed a status-only check).
- `MemberCanRevokeOwnOtherSession` — guard: the rule is ownership, not "is this
  my current session id", so "log out my other device" still works.
- `RevokingAnAlreadyRevokedOwnSessionStaysIdempotent` — guard: the
  `IAuthProvider` idempotency contract survives the new read.
- `SystemAdminCanRevokeAnotherUsersSession` — the documented cross-user path,
  still reachable after `session.revoke` became global-only.

Round one, red on the test commit with the ownership fix absent:
`./out/build/dev/tests/integration/freezermanager_session_service_integration_tests --gtest_color=no`
→ `[  PASSED  ] 5 tests.` / `[  FAILED  ] 2 tests`, exit 1, with
`status.error_code()` `Which is: 0` against expected `PERMISSION_DENIED`
(`Which is: 7`) and `token_is_usable(alice.token)` `Actual: false, Expected:
true`.

Round two, red on the commit that adds the two tests and before the
classification fix →
`--gtest_filter='*LabAdmin*:*LabScoped*'`: `[  FAILED  ] 2 tests`, exit 1 —
`GrantPermission` returned `OK` (`Which is: 0`) where `FAILED_PRECONDITION`
(`9`) is expected, and the supervisor's cross-lab revoke returned `OK` while
`carol`'s token stopped working. After the fix: `[  PASSED  ] 9 tests.`

Focused and full:
`./out/build/dev/tests/integration/freezermanager_session_service_integration_tests`
→ `[  PASSED  ] 9 tests.`, exit 0;
`./out/build/dev/tests/unit/freezermanager_core_unit_tests --gtest_filter='PermissionCatalog.*'`
→ `6/6`; `ctest --preset dev -R '^SessionServiceTest'` → `9/9`; and the full
`ctest --preset dev` → **`100% tests passed out of 1455`**, exit 0, 444
skipped (Postgres and the parameterized backend suites; `FMGR_TEST_POSTGRES_URL`
is unset locally). Wall time on this shared machine ran from 87.7 s (idle) to
1494.6 s (worker-1 and worker-2 building at the same time) — same 1455/1455
every time. `clang-format --dry-run --Werror` (the `.venv`'s 17.0.6, matching
CI's 17.x) is clean on all four changed files.

`run-clang-tidy-17` is not installed on the owner's Mac, so `clang-tidy` 17.0.1
from the shared `.venv` was run per file (the `#71` note's method). It is a hint,
not a verdict: the local compile DB is AppleClang and clang-tidy 17 cannot parse
that SDK's libc++ (`__builtin_clzg`, `__GCC_DESTRUCTIVE_SIZE`), so every TU
reports ~17 `clang-diagnostic-error`s plus bogus
`readability-convert-member-functions-to-static` findings on methods that
plainly use `this` — the CI-green
`tests/integration/item_type_service_integration_test.cpp` produces the same
four, which is how I confirmed they are artifacts.

What that leaves is worth reporting honestly, because one of them was real in
round one:

- `src/server/SessionServiceImpl.cc`: round one's A/B against the `origin/main`
  version was **identical**, 21 findings each, all in unmodified headers or the
  SDK. Round two adds no finding to the file; the `std::ranges::any_of` helper
  and its `<algorithm>` include are gone.
- `tests/integration/session_service_integration_test.cpp`: the round-one
  `revoke(token, session_id)` helper tripped a genuine
  `bugprone-easily-swappable-parameters`; the round-two `create_role` and
  `grant_permission` helpers are the same shape. All three carry the same
  `NOLINTNEXTLINE(bugprone-easily-swappable-parameters)` the existing fixtures
  use, and the check reports nothing on the file. The remaining findings there
  are the `convert-member-functions-to-static` artifacts the baseline file also
  shows.

CI's `clang-17 dev` job is the real gate for every one of these files.

## Known limitations / follow-ups

- `ListSessions` still ignores `ListSessionsRequest.user_id` and is still
  registered against `SessionRevoke`; see the decision above. #60's structural
  registry check is the right place for the second half.
- Lab-owned roles that already carry `session.revoke` stay in the database as
  inert rows. Cleaning them up is a migration (`lock:migration`) and was not
  part of this fix.
- The new suite carries the `grpc_integration` label, so it is excluded from the
  asan/tsan presets by design, like every other in-process-server test in
  `tests/integration/`. This change touches no memory or concurrency code, so no
  sanitizer run was made.
- `permission_catalog_test.cpp:65` was a green test pinning a wrong
  classification. It is fixed here, but it is the second time this slice found a
  test that could not fail on the bug it was covering (round one's fixture used
  built-in roles only, so it could not express a custom role at all). Worth
  remembering when reviewing permission work: ask what the fixture cannot
  construct.
