# Handoff note — 2026-09-29, `RevokeSession` ownership check (#77, worker-3)

`SessionServiceImpl::RevokeSession` called `validate_authed` and then handed the
caller-supplied `session_id` straight to `IAuthProvider::revoke_session`, whose
`LocalAuthProvider` implementation soft-deletes by id with no owner predicate
(`LocalAuthProvider.cc:286-294`). Any authenticated caller could therefore force
any session it could name to log out. The RPC's `AuthMiddleware` registry entry
claimed `core::Permission::SessionRevoke`, but no `authorize()` call and no
ownership check existed anywhere on the path — the registry entry was
documentation for an enforcement point that was never written.

**Severity is P1, a targeted denial of service, not data access**, and both
bounds were verified rather than assumed. Ids cannot be enumerated through the
API: `ListSessions` filters on `UserId == sctx.user_id`, and the request's
`user_id` filter — which the proto documents as the SystemAdmin escape hatch —
is not implemented, so there is no cross-user listing to harvest ids from. And
the revoke is not an existence oracle: the deny decision is made on ownership,
never on whether the row was found, so a live foreign id and a never-issued id
produce byte-identical denials.

**Changed:** `src/server/SessionServiceImpl.cc` — two anonymous-namespace
helpers and one guard in `RevokeSession`:

- `caller_owns_session(backend, sctx, session_id)` reads the target row through
  the typed query DSL (`find_by_id`) inside a `ReadCommitted` transaction with
  `inject_rls_vars`, and compares `user_id` with the caller's. It uses
  `find_by_id` rather than a `Query<Session>` because that path returns
  tombstoned rows too, which is what keeps a repeated revoke of one's own
  session the idempotent no-op `IAuthProvider` promises instead of turning it
  into a permission error. A session's owner never changes after creation, so
  the separate read cannot race the tombstone write.
- `holds_session_revoke_anywhere(sctx)` answers "does this caller hold
  `session.revoke`?" for an RPC whose request carries no lab. `session.revoke`
  is **not** a global-only permission (`core/permissions.h:210`), so a
  SystemAdmin holds it per lab and it lands in `permissions_by_lab`; the check
  is therefore `has_global(...) || any_of(permissions_by_lab, ...)`. An API
  token whose `scope_json` excludes the permission stays excluded, because
  `intersect_grants_with_scope` has already pruned it from both sets.
- The guard: `if (!caller_owns_session(...) && !holds_session_revoke_anywhere(...))
  throw auth::PermissionDenied(...)`, before `auth_.revoke_session(...)`.

Nothing else changed. No proto edit (so no `lock:proto`), no migration, no
`AuthMiddleware.*` / `FreezerServer.*` / `SampleServiceImpl.cc` touch.

**Decisions:**

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
  the code already documents.** Making the RPC require `session.revoke`
  outright would have been the wrong fix: a `Member` holds no `session.*` grant,
  and logging *yourself* out is the reason the RPC exists. So ownership alone
  authorises a self-revoke, and the cross-user path is the documented exception
  — the proto says "The session must belong to the caller unless the caller is
  a SystemAdmin", and the catalog describes `session.revoke` as "Revoke another
  user's sessions". Verifying that exception was the part I did not take from
  the issue text: it existed only as prose, and is now an explicit gate with its
  own test. Denying with `PERMISSION_DENIED` (not `NOT_FOUND`) was chosen
  deliberately: it is the honest answer, and it cannot become an existence
  oracle.
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
  system-admin-only. A `LabAdmin` holding `audit.read` therefore cannot read
  them; only a deployment SystemAdmin can, and a SystemAdmin is exactly the
  principal the documented exception already lets revoke any session. So the
  exploit needs a *known* id, and the P1-not-P0 bound stands.
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

**Tests:** `tests/integration/session_service_integration_test.cpp` (new, 7
tests), registered in `tests/integration/CMakeLists.txt` as
`freezermanager_session_service_integration_tests` with the `grpc_integration`
label, matching the other in-process-server suites. The fixture seeds one lab
and three principals — a SystemAdmin plus two `Member`s (`alice`, `bob`) — and
drives a real `FreezerServer` on a random port over gRPC.

- `MemberCannotRevokeAnotherUsersSession` — **the decisive test**. `bob` revokes
  `alice`'s session id; asserts `PERMISSION_DENIED` *and* that alice's token
  still works, because a forced logout is what the bug actually delivered.
- `MemberCannotRevokeUnknownSessionId` — the same path with no row at all.
- `DeniedRevokeDoesNotRevealWhetherTheSessionExists` — guard: the denial for a
  live foreign id and for a never-issued id is identical, error message
  included.
- `MemberCanRevokeOwnSession` — the logout path: `OK`, and the token really
  stops working (a silent no-op would also have passed a status-only check).
- `MemberCanRevokeOwnOtherSession` — guard: the rule is ownership, not "is this
  my current session id", so "log out my other device" still works.
- `RevokingAnAlreadyRevokedOwnSessionStaysIdempotent` — guard: the
  `IAuthProvider` idempotency contract survives the new read.
- `SystemAdminCanRevokeAnotherUsersSession` — the documented cross-user path,
  which nothing tested before because nothing enforced it.

Red first, on the test commit with the fix absent:
`./out/build/dev/tests/integration/freezermanager_session_service_integration_tests --gtest_color=no`
→ `[  PASSED  ] 5 tests.` / `[  FAILED  ] 2 tests`, exit 1. The decisive
failure is `status.error_code()` `Which is: 0` against expected
`PERMISSION_DENIED` (`Which is: 7`) at `:242`, then
`token_is_usable(alice.token)` `Actual: false, Expected: true` at `:244`.
After the fix the same binary is `[  PASSED  ] 7 tests.`, exit 0.

Full suite after the fix: `ctest --preset dev` →
`100% tests passed out of 1453`, exit 0, 444 skipped (Postgres and the
parameterized backend suites; `FMGR_TEST_POSTGRES_URL` is unset locally). All
seven new cases ran: `ctest --preset dev -R '^SessionServiceTest'` → `7/7`
passed. Wall time on this shared machine ranged from 87 s (idle) to 1600 s
(worker-1 and worker-2 building at the same time); both runs are the same
`1453/1453`. `clang-format --dry-run --Werror` (the `.venv`'s 17.0.6, matching
CI's 17.x) is clean on both changed C++ files.

`run-clang-tidy-17` is not installed on the owner's Mac, so `clang-tidy` 17.0.1
from the shared `.venv` was run per file (the `#71` note's method). It is a hint,
not a verdict: the local compile DB is AppleClang and clang-tidy 17 cannot parse
that SDK's libc++ (`__builtin_clzg`, `__GCC_DESTRUCTIVE_SIZE`), so every TU
reports ~17 `clang-diagnostic-error`s plus bogus
`readability-convert-member-functions-to-static` findings on methods that
plainly use `this` — the CI-green `tests/integration/item_type_service_integration_test.cpp`
produces the same four, which is how I confirmed they are artifacts.

What that leaves is worth reporting honestly, because one of them was real:

- `src/server/SessionServiceImpl.cc`: A/B against the `origin/main` version of
  the same file is **identical**, 21 findings each, all in unmodified headers or
  the SDK — no new finding from this diff, and none on the new helpers.
- `tests/integration/session_service_integration_test.cpp`: the new `revoke(token,
  session_id)` helper tripped a genuine `bugprone-easily-swappable-parameters`
  (two adjacent `std::string` parameters). Fixed with the same
  `NOLINTNEXTLINE(bugprone-easily-swappable-parameters)` the existing fixtures
  use; it is gone from the re-run. The remaining findings on that file are the
  same `convert-member-functions-to-static` artifacts the baseline file shows
  under local clang-tidy.

CI's `clang-17 dev` job is the real gate for both files.

**Known limitations / follow-ups:**

- `holds_session_revoke_anywhere` is deliberately lab-agnostic because
  `RevokeSessionRequest` has no lab field, so a SystemAdmin who administers lab
  A can revoke a session belonging to a user of lab B. That is the same
  deployment-wide reach `Permission::SessionRevoke` already implies (nothing
  scopes it to a lab today), but it is a real widening of what the permission
  can currently do, and it is the one place a reviewer might want the opposite
  call. Scoping it would need a `lab_id` in the request — a `lock:proto` change.
- `ListSessions` still ignores `ListSessionsRequest.user_id` and is still
  registered against `SessionRevoke`; see the decision above. #60's structural
  registry check is the right place for the second half.
- The new suite carries the `grpc_integration` label, so it is excluded from
  the asan/tsan presets by design, like every other in-process-server test in
  `tests/integration/`. This change touches no memory or concurrency code, so
  no sanitizer run was made.
