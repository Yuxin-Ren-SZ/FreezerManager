# Handoff note — 2026-10-02, #78 the registry can say "no permission required" (worker-3)

The `AuthMiddleware` RPC → permission registry held a bare `core::Permission`,
so it had no way to *express* "this RPC is gated by something other than a
permission". Ten entries named a permission their handler never checked: the six
`auth/*` RPCs and `SessionService/ListSessions`/`RevokeSession` registered
`session.revoke`, `LabService/ListLabs` registered `lab.configure`, and
`AuditService/VerifyAuditChain` registered `audit.read`. After #74 made the
registry enforced, none of them was dangerous — `authorize()` never runs for
those handlers, so nothing reads their entries at the gate — but each was an
untrue statement, and an untrue entry is what makes a future reader "correct"
working code to match it.

**Changed:**

- `src/rpc/AuthMiddleware.h/.cc` — `rpc::RpcGate` is the registry's new value
  type. `Kind::Permission` means exactly what #60 established: the handler's
  `authorize()` call must name this permission, and the gate refuses the call
  when it does not. `Kind::NoPermissionRequired` means the handler must not be
  permission-gated; it is constructed by `RpcGate::no_permission_required()` and
  carries no permission at all. `register_rpc` / `registered_rpcs` now take and
  return a `RpcGate`; the implicit conversion from `core::Permission` keeps the
  62 permission-gated registrations reading unchanged. `require_registry_agreement`
  refuses a `NoPermissionRequired` entry exactly like a permission mismatch, so
  the new state cannot be used to silence the #60 check.
- `src/server/AuthServiceImpl.cc` — six registration **values** only
  (`session.revoke` → `no_permission_required()`); no handler body changed, and
  no entry added, removed or renamed. Cleared with the lead on #78 before
  touching the file (its `ASSIGN` asked for that because #62 concerns it; #62 is
  still `status:ready` with no branch).
- `src/server/SessionServiceImpl.cc`, `src/server/LabServiceImpl.cc` — the three
  remaining `no_permission_required()` entries, each with a comment naming the
  credential rule its handler actually applies.
- `src/server/AuditServiceImpl.cc` — `VerifyAuditChain` given a **real
  enforcement point** instead of the new state. It is genuinely gated, on the
  deployment-admin predicate `is_system_admin()`, which is literally
  `has_global(Permission::LabProvision)`; the handler now calls
  `authorize(call, Permission::LabProvision, std::nullopt)` and the entry is
  `lab.provision`.
- `AGENTS.md` §5 — the "does not gate through `authorize()`" bullet now says to
  register `RpcGate::no_permission_required()` and never a permission the handler
  does not check, records what the gate does and does not enforce for those RPCs,
  and keeps the review obligation to say which credential rule the handler uses.
- Tests: `tests/unit/auth_middleware_test.cpp`,
  `tests/integration/server_integration_test.cpp`,
  `tests/integration/item_type_service_integration_test.cpp`.

**Decisions:**

- **A single new state does not fit all ten, and the finding is on the issue.**
  The issue's wording is "authenticated (and MFA) — no permission required".
  `VerifyAuditChain` is refused for a non-admin, so registering it that way would
  have been a fresh untrue claim; it got a real gate instead. `Login` needs no
  credential at all and `SubmitMfa` needs a token but deliberately not MFA, so a
  state *named* after authentication (`AuthenticatedOnly`) would have been false
  for both. The state is therefore defined by the only thing the gate can verify
  — "no permission required" — and the credential rule stays a comment at the
  registration plus the review obligation in `AGENTS.md` §5.
- **Why no `AuthenticatedOnly` runtime gate.** Forcing the nine handlers through
  a new middleware method would make their *entries* checked at runtime, but it
  edits the login/logout path and the auth handler bodies, which the `ASSIGN`
  explicitly fenced off. `(a)` accurate entries + pinning tests + the anti-bypass
  rule was taken; `(b)` (that plus routing the handlers) is additive and is
  offered to the lead on #78. The honest limitation is recorded below either way.
- **`RevokeSession` is in the nine, not in the `Permission` kind.** #77 made it
  check `session.revoke` by hand for another user's session, but it never calls
  `authorize()`, and `Kind::Permission` means "the gate enforces this on every
  call". Its entry states the admission rule (none beyond token + MFA) and the
  registration comment names the hand-check.
- **`GetShareRequest` is *not* one of the ten** on a re-read: it enforces
  `share.request` by hand through `can_read_request()`, so its entry names a
  permission a code path does check. #60's list counted the same set.

**Tests:**

- `cmake --build --preset dev` → exit 0.
- `ctest --preset dev -R 'AuthMiddleware'` → **30/30 passed**.
- `ctest --preset dev -R 'ServerIntegrationTest'` → **13/13 passed**.
- Planted disagreement (the issue's decisive check, mirroring #60's): with
  `ListSessions` re-registered as `core::Permission::SessionRevoke` —
  a permission its handler never enforces —
  `ServerIntegrationTest.RpcRegistryStatesTheGateEachNonPermissionRpcHas` fails
  at `tests/integration/server_integration_test.cpp:537` with
  `"/fmgr.v1.SessionService/ListSessions is registered as permission
  'session.revoke', i.e. it still claims a permission its handler never
  enforces"`; `ctest` reports `0% tests passed, 1 tests failed out of 1`.
  Reverted, and green again.
- Full `ctest --preset dev` on the final rebase (`origin/main` = `55016d6`,
  which brings #106 in): **100% tests passed out of 1542** in 256 s, exit 0. The
  machine was loaded while that ran (load average 9.6 — other agents building),
  so it is the loaded case, and it passed anyway; the same suite on the
  pre-rebase revision was 100% too (**1535/1535**, 101 s, load 1.4). The
  Postgres-backed tests skip without `FMGR_TEST_POSTGRES_URL`, as on `main`.
- `clang-format --dry-run --Werror` over every changed file: clean (17.0.6).
- `clang-tidy` 17.0.1 per file on the changed TUs. **No finding passes both
  filters the board's method asks for** — (a) on a line this change touched and
  (b) not also firing on the unmodified `origin/main` version of the same file.
  Every finding on a changed or line-shifted site also fires on the baseline:
  `performance-unnecessary-value-param` on `register_rpc`'s `rpc_name` (the same
  parameter, before this change); `bugprone-branch-clone` on `ListLabs`'s
  pre-existing if/else (`LabServiceImpl.cc:126` before the new comment shifted
  it); `readability-implicit-bool-conversion` on gtest `FAIL() << …` /
  `EXPECT_*` expansions, which the baseline test files report at the same
  constructs; and `readability-convert-member-functions-to-static`, which is
  malfunctioning in this local environment — it fires on *already-static*
  methods (`registered_rpcs`, `verify_registry_covers`) in the untouched
  baseline. No local-only `bugprone-easily-swappable-parameters` either, and no
  new helper has two adjacent same-type parameters, so no NOLINT was needed.
  The `clang-tidy` CI job is still the only real gate; see the PR for what it
  reported.

**Known limitations / follow-ups:**

- **The nine corrected entries are pinned by a test, not enforced by the gate.**
  For an RPC that never calls `authorize()` nothing observes its registry entry,
  so "someone re-registers `ListSessions` as `session.revoke`" is caught by
  `RpcRegistryStatesTheGateEachNonPermissionRpcHas` (and would be caught by any
  future source-scanning guard) rather than by a refused call. The gate does
  enforce the other direction: a `no_permission_required()` RPC whose handler
  calls `authorize()` is refused.
- **`VerifyAuditChain` now passes through the data-tier rate limiter** (120
  burst / 40 per second per bearer by default), because it goes through
  `authorize()` like every other data RPC. That is a tightening, not a
  loosening, and it is the only behaviour change in the slice.
- **`SessionRevoke` is no longer registered by any RPC.** It remains a real
  permission — `LocalAuthProvider` still grants it, `RevokeSession` still checks
  it for another user's session — but no registry entry names it, which is
  correct: no RPC's *gate* enforces it.
