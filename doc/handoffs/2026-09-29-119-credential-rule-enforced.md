# Handoff note — 2026-09-29, #119 the credential rule of a non-permission RPC is enforced (#119, worker-2)

Makes the credential rule of the nine RPCs outside the permission gate **runtime-enforced**
rather than declared. #78 gave the registry an accurate way to say "this RPC requires no
permission" and pinned it with a test; because those handlers never call `authorize()`, the gate
never ran for them and nothing observed their entries during a call, so a handler could start
demanding MFA with the suite green. The rule is now one of three the registry carries, the gate
applies it, and the gate refuses the call when the rule a handler applies disagrees with the one
its RPC registered — #60's mechanism, applied to the handlers #60 could not reach. No operator-
visible behaviour changes (PRD §12's auth model is unchanged; nothing in `PRD.md` needed editing).

**Changed:**

- `src/rpc/AuthMiddleware.{h,cc}` — `RpcGate::Kind` gains `Credential` beside `Permission`;
  `CredentialRule{None,TokenOnly,TokenAndMfa}` and `rpc::to_key(CredentialRule)` (keys
  `no_credential` / `token_only` / `token_and_mfa`); `RpcGate::{no_credential,token_only,token_and_mfa,credential}()`
  and `credential_rule()`; `AuthMiddleware::authenticate(call, rule)` and
  `admit_no_credential(call)`, both running the new `require_credential_agreement()` as step 0.
  `RpcGate::no_permission_required()` and `Kind::NoPermissionRequired` are gone.
- `src/server/AuthServiceImpl.cc` — the six `auth/*` handlers ask the gate instead of validating
  the token themselves; the local `validate_authed()` / `validate_token_any_mfa()` helpers are
  deleted, so the MFA branch now exists in exactly one place.
- `src/server/SessionServiceImpl.cc`, `src/server/LabServiceImpl.cc` — same for `ListSessions`,
  `RevokeSession` and `ListLabs` (their local `validate_authed()` helper and `ListLabs`' inline
  token + MFA check are gone).
- `src/server/GrpcErrorTranslation.h` — `extract_bearer()` split into `bearer_token(ctx)` (throws
  as before) and `rpc_method_name(ctx)` (reads no credential, throws nothing). `Login` uses the
  latter: a handler that declares it needs no credential must not start refusing callers for an
  `Authorization` header it never reads.
- `tests/unit/auth_middleware_test.cpp`, `tests/integration/server_integration_test.cpp` — see
  Tests below. `#78`'s pin was replaced by
  `RpcRegistryStatesTheCredentialRuleEachNonPermissionRpcHas`, which pins all nine to a rule.
- `AGENTS.md` §5 — the registry bullet now names the three rules, the two gate entry points, and
  both halves of the anti-bypass.

**Decisions:**

- **`no_permission_required()` was removed rather than kept beside the new rules.** Keeping it
  would leave a way to register a non-permission RPC that nothing checks — the gap this issue
  closes. [#119 STATUS](https://github.com/Yuxin-Ren-SZ/FreezerManager/issues/119#issuecomment-5892052799)
- **The handler still names its rule.** The alternative — the gate looks the rule up and the
  handler passes nothing — cannot fail when a handler starts requiring something different, which
  is the acceptance criterion. Asking the gate keeps the handler's claim as the input the gate
  checks against the registration, exactly like `authorize(call, permission)`.
- **`authenticate()` returns a context; the `None` rule has its own entry point
  (`admit_no_credential()`).** One `std::optional<SessionContext>`-returning method would have put
  a provably-impossible empty branch in six handlers.
- **`authenticate()` does not apply the process data-tier rate limiter** (that stays `authorize()`'s
  step 1). None of the nine is on it today, so this is behaviour-preserving; adding it would be a
  real change for a REST client polling `ListLabs`/`ListSessions` and belongs in its own issue.
- **The symmetric anti-bypass is new.** `validate`-side: `authorize()` still refuses a `Credential`
  entry (unchanged code, #78). Credential-side: the gate now refuses a `Permission` entry whose
  handler asks it for a rule, because that entry would otherwise be enforced by nothing.

**Tests:**

- New/rewritten unit tests: the gate applies the declared rule (`TokenOnly` admits a pending-MFA
  session, `TokenAndMfa` refuses it and admits it once complete); a handler whose rule disagrees
  with its registration is refused and the message names both; a bad or absent credential is still
  refused; `admit_no_credential()` needs no token and refuses any other rule; each registry state
  refuses the other's entry point; the gate reports its kind and rule and refuses to be misread.
- New integration tests: `CredentialRuleDisagreeingWithEnforcedRuleIsRefused` plants four
  disagreements against a live server (wrong rule, weaker rule, a `Permission` entry, and #78's
  direction) and asserts each is refused with both sides named;
  `EveryRpcDeclaringTokenAndMfaRefusesAPendingMfaSession` walks all six `TokenAndMfa` RPCs.
- Acceptance evidence — `Logout`'s handler flipped from `TokenOnly` to `TokenAndMfa`, nothing else
  touched: `ctest --preset dev -R 'Logout|PermissionlessCaller|CredentialRule|RpcRegistry|PendingMfa'`
  → **6 of 16 failed** (`ServerIntegrationTest.LogoutRevokesSession`,
  `.LogoutRevokesAPendingMfaSession`, `.PermissionlessCallerReachesEveryRpcThatRequiresNoPermission`,
  `RestGatewayBrowserSession.LogoutRevokesTheSessionAndExpiresBothCookies`,
  `.PendingMfaLogoutRevokesTheSessionAndExpiresBothCookies`, `E2ESmokeTest.FullAuthFlowLoginLogout`),
  with `RPC /fmgr.v1.AuthService/Logout is registered as credential rule 'token_only' but its
  handler requires 'token_and_mfa'`. Reverted → the same filter is **62/62**.
- Second red/green — `require_credential_agreement()` commented out in `authenticate()`:
  `ctest --preset dev -R AuthMiddlewareTest` → **35/37, 2 fail**, exactly
  `CredentialGateRefusesAnRpcRegisteredWithAPermission` and
  `AuthenticateRefusesAHandlerWhoseRuleDisagreesWithTheRegistration`.
- `ctest --preset dev -R 'AuthMiddleware|ServerIntegration|Rbac|Logout|PendingMfa|E2ESmoke'` →
  **62/62 passed**. Full `ctest --preset dev` → see the PR body for the count on the rebased head.

**Known limitations / follow-ups:**

- A handler that validates its credential *inline* instead of asking the gate is still outside any
  check — the same boundary `AGENTS.md` §5 already documents for handlers that check a permission
  by hand (`gate_role_read`, `is_system_admin`). The nine RPCs this issue is about all go through
  the gate now; a tenth that does not would need its own issue.
- The RPC method name reaches the gate through `RpcMethodTracker`, which fails **open**: if the
  interceptor ever stopped being installed, the method name is empty and both registry checks are
  skipped. That is pre-existing and documented in `src/rpc/RpcMethodTracker.h`; the planted
  disagreement tests in `server_integration_test.cpp` are the tripwire.
- `SubmitMfa` has no brute-force limiter of its own (`AuthServiceImpl` throttles `Login` per IP
  only). Pre-existing, unrelated to the credential rule, and not touched here.
- The six `TokenAndMfa` RPCs remain outside the process data-tier rate limiter, as above.
