# Handoff note — 2026-09-29, #60 the RPC registry is now an enforcement point (#60, worker-2)

`AuthMiddleware`'s RPC → permission registry used to be write-only metadata:
nothing under `src/` called `registered_rpcs()` or `is_rpc_registered()`, and the
test that "covered" it asserted `registry.size() >= 60`. An entry declaring
`sample.read` while its handler enforced `freezer.configure` passed the entire
suite — the shape of #54's near miss, where following a wrong line anchor would
have relaxed a mutating RPC with every test green. This slice makes the registry
load-bearing at the gate (PRD §12 authorisation, `AGENTS.md` §5).

**Changed:**

- `src/rpc/AuthMiddleware.h/.cc` — `rpc::RpcCall` (bearer token **plus** the full
  method name of the RPC being served) is the new input to
  `AuthMiddleware::authorize()`. Step 0 checks the permission the handler asks
  for against the one its RPC registered and throws `RpcRegistryMismatch`
  (`std::logic_error` → `INTERNAL`) when they disagree, or when the served RPC is
  not registered at all. `verify_registry_covers()` is the startup half.
  `is_rpc_registered()` is deleted (no callers outside tests).
- `src/rpc/RpcMethodTracker.h/.cc` (new) — `(ServerContextBase* → method)` map
  plus the per-RPC interceptor that fills it. gRPC 1.5x's `ServerContext` has no
  method accessor, so this is where the name comes from; keyed by context, not by
  thread, so it does not depend on which worker thread runs a handler.
- `src/server/GrpcErrorTranslation.h` — `extract_bearer(ctx)` now returns an
  `RpcCall`. Every handler already calls it at its gate, so **no
  `*ServiceImpl.cc` changed** and #53/#57 could not be raced. `RpcCall` converts
  implicitly to `std::string_view` so the ~30 call sites that only want the
  credential (`validate_token`, …) keep compiling.
- `src/server/FreezerServer.{h,cc}` — `served_service_full_names()` /
  `served_rpc_names()` enumerate the served set from the server's own service
  list and the generated proto descriptors; `build()` registers from that list,
  installs the tracker interceptor, and calls `verify_registry_covers()` before
  binding, so a served RPC with no registration aborts startup.
- `AGENTS.md` §5 — the rule now describes what the code does, including the
  residual gap (RPCs that do not gate through `authorize()`).
- Tests: `tests/integration/server_integration_test.cpp`,
  `tests/unit/auth_middleware_test.cpp`.

**Decisions:**

- **Option 2, moved to the enforcement point.** Option 2 as the issue words it (a
  test that scans `*ServiceImpl.cc` for `authorize()` calls) was prototyped and
  rejected: with three gating idioms in the tree it misclassifies eight of the
  seventy served RPCs — `ListLabs` consults `LabProvision` for visibility,
  `VerifyAuditChain` is gated by `is_system_admin`, the share RPCs gate inside
  helpers — so it needs a hand-maintained exception table, i.e. a guard whose
  correctness rests on trusting the exceptions. Checking at the gate instead is
  exact, needs no source parsing, and fails the call rather than a test. Option 1
  (the middleware decides the permission) remains the better end state; this
  slice gets most of its guarantee without touching 40 handler call sites. The
  reasoning is on the issue: [worker-2] STATUS, #60.
- `RpcCall` carries the method name instead of every handler passing it, which is
  what kept the change out of `BoxServiceImpl.cc` while #57 edits it.
- The mismatch is `INTERNAL`, not `PERMISSION_DENIED`: it is a server defect, and
  a 403 would blame the caller. The message names the RPC and both permission
  keys (masked in release builds like every other internal error).
- Kept the coverage check a subset check at startup and the exact set-equality
  check in the test: a binary may construct services it does not serve.

**Tests:**

- `ServerIntegrationTest.RegisteredPermissionDisagreeingWithEnforcedPermissionIsRefused`
  (new, the acceptance test): plants the disagreement in-process for `ListSamples`
  (read) and `CreateSample` (mutating, #54's case), asserts the call is refused
  with a message naming both permissions, and that the authorised call succeeds
  again after the restore. On the parent revision it failed as the issue
  describes: `ListSamples` returned OK while registered `freezer.configure`.
- `ServerIntegrationTest.RpcRegistryHoldsExactlyTheServedRpcs` replaces
  `RpcRegistryCoversAllExpectedMethods`' count floor with set equality in both
  directions.
- Unit tests: mismatch refused, unregistered served RPC refused, matching
  permission accepted, a genuine `PermissionDenied` still denied, no-RPC-identity
  calls skip the check, coverage check names every missing RPC, tracker
  note/lookup/forget.
- `cmake --build --preset dev` → exit 0; `ctest --preset dev` →
  **1454/1454 passed, 0 failed** on the final rebase (origin/main `77680f6`,
  which brought #71's tests in); Postgres-backed tests skip without
  `FMGR_TEST_POSTGRES_URL`, as on `main`. One incremental build during the
  rebase failed to regenerate the gtest-discovery files for the e2e and REST
  gateway targets — the discovery step runs each freshly linked binary with
  `--gtest_list_tests`, and both binaries return 0 and list their tests now, so
  it was environmental, not a compile error. The rebuild immediately after
  reported `ninja: no work to do` (exit 0).
- Hand demonstration: planting `P::SampleRead` on `CreateSample`'s registration
  in `src/server/SampleServiceImpl.cc` turns **38 of 49** `SampleService` tests
  red with `internal server error: RPC /fmgr.v1.SampleService/CreateSample is
  registered as permission 'sample.read' but its handler enforces 'sample.write'`.
  Reverted; 49/49 green again.
- `clang-format --dry-run --Werror` over every changed file: clean.
- The acceptance test keeps the refusal message readable by setting
  `server_opts_.mask_internal_errors = false` in the fixture. Without it the
  assertion only passes in debug builds, where INTERNAL detail is unmasked by
  default (`FreezerServerOptions` keys masking off NDEBUG) — the release presets
  CI builds would return "internal server error" and the test would fail there.

**Known limitations / follow-ups:**

- **The gate check covers the 39 of the 72 served RPCs that call
  `middleware_.authorize()`.** The other 33 gate somewhere else, so nothing
  verifies their registry entry yet. Measured on this branch, they are:
  - **eighteen** that check the *same* permission by hand, usually after loading
    the row: `GetFreezer`/`ArchiveFreezer`/`ArchiveStorageContainer`,
    `GetBox`/`ArchiveBox`, `GetItemType`/`ArchiveItemType`/
    `ArchiveCustomFieldDefinition`, `ArchiveRole`, `GrantPermission`,
    `RevokePermission`, `GetSample`, `SoftDeleteSample`, `MoveSample`,
    `CheckoutSample`, `GetAuditEvent`, `ListShareRequests`, `RevokeShareRequest`.
    `GetItemType` is the archetype the lead called out on #60: the lab is only
    known after the row is loaded, so it cannot become an `authorize()` call
    without splitting "the registry supplies the permission, the method supplies
    the scope";
  - **five** gated through a local helper — `GetRole`/`ListRolePermissions`
    (`gate_role_read`), `GetShareRequest`/`ApproveShareRequest`/
    `RejectShareRequest` (`gate_read`/`gate_approver_role`);
  - **eight** that require only a token and MFA — `Login`/`SubmitMfa` (pre-auth),
    `Logout`, `CreateApiToken`, `ListApiTokens`, `RevokeApiToken`,
    `ListSessions`, `RevokeSession`;
  - **two** whose registration disagrees with the code: `ListLabs` consults
    `LabProvision` for *visibility* while registered `LabConfigure`, and
    `VerifyAuditChain` requires `is_system_admin` (`LabProvision`) while
    registered `AuditRead`.
  All of them are listed with evidence in the `[worker-2] QUESTION` on #60, and
  `AGENTS.md` §5 now says plain that they are review-time obligations. Owning the
  follow-up (aligning the registry, or splitting permission from scope) is the
  lead's call. Re-measured after rebasing onto `f95779d` (which brings #69's
  item-type read split in): still 39 of 72 through `authorize()` and 33 not;
  `GetItemType` stays in the hand-checked group and now checks `sample.read`,
  matching its registration.
- **`SessionService/RevokeSession` may be a live authorization hole**, not just a
  stale row: `validate_authed` + `LocalAuthProvider::revoke_session` has no
  ownership check, so any authenticated caller can revoke any session id while the
  registry claims `session.revoke`. Reported on #60; deliberately not changed
  here, since it is a behaviour change and #54's territory.
- **Cost of the check**: every `authorize()` call now takes a process-global
  mutex (one map lookup) and `extract_bearer()` copies the bearer token and the
  method name. That is per authenticated RPC, on the request path, and it is
  fine at this scale — but it is a new global lock in the hot path, so a future
  RPC-per-second-sensitive change should know it is there. The interceptor also
  adds one map insert/erase per RPC on the server side.
- **Local `clang-tidy` 17.0.1 cannot lint this project on the owner's Mac**: LLVM
  17 cannot parse the installed SDK's libc++ (`__builtin_clzg` in
  `__bit/countr.h`), so every TU dies with ~20 `clang-diagnostic-error`s from
  system headers before any project check runs. The `Run clang-tidy` CI step is
  therefore the only real gate for this PR; `clang-format --dry-run --Werror` and
  `tools/check-spdx-headers.sh` both pass locally.
- **One full-suite run after the rebase reported 1 failure out of 1449**, and the
  name was lost because that run's log was not kept; the two full runs after it
  were 1449/1449 and 1449/1449. The known flake in this wave is the #29 SSE
  shutdown test, but I cannot claim that is what it was.
