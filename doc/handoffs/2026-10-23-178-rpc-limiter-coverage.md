# Handoff note — 2026-10-23, #178 authenticated RPC limiter coverage (worker-2)

The data-tier limiter was applied by `authorize()` but not by the parallel
`authenticate()` admission used by TokenAndMfa handlers. Those handlers now
validate the session, require completed MFA, and only then consume the
data-tier budget. All service handlers that used to call `validate_token()`
directly use that same admission, including Audit List/Export/Watch, whose lab
and deployment-wide permission branches now share one limiter charge.

**Changed:**

- `src/rpc/AuthMiddleware.{h,cc}` — reordered TokenAndMfa admission to
  validate-token → MFA → data-tier limit; clarified that Credential registry
  entries declare the credential admission while handlers retain any separate
  scope/domain permission checks.
- `src/server/{Audit,Box,ItemType,Role,Sample,Share}ServiceImpl.cc` — migrated
  direct token validation to the middleware admission, changed registry entries
  to `TokenAndMfa`, and retained each existing manual lab/global permission
  check and RLS injection. Audit lab/global paths take exactly one bucket token.
- `src/server/FreezerServer.cc` — clarified that the installed data-tier gate
  is used by both permission-gated and TokenAndMfa-admitted data RPCs.
- `tests/unit/auth_middleware_test.cpp` — pins that invalid and pending-MFA
  tokens fail before the data-tier bucket is charged, while an admitted session
  is rate-limited.
- `tests/integration/server_integration_test.cpp` — registry-driven sweep of
  every Permission and TokenAndMfa registration, invoked twice with a fresh
  session per method; lab/global audit permission and once-only budget tests;
  correct TOTP after exhaustion returns `RESOURCE_EXHAUSTED` and leaves the
  session pending.
- `tests/integration/item_type_service_integration_test.cpp` — registry
  expectations now distinguish credential admission from the retained manual
  SampleRead / ItemTypeDefine / CustomFieldDefine checks.

**Decisions:**

- Keep the permission checks where the target lab or resource row is known only
  after loading it. A Credential registration does not claim that permission;
  integration tests pin the actual handler behavior separately.
- Audit List/Export/Watch first perform one TokenAndMfa admission, then branch
  into their existing lab AuditRead/AuditExport or deployment SystemAdmin check.
  This prevents double charging while preserving both refusal paths.
- TokenOnly `SubmitMfa` and `Logout` remain outside the data-tier bucket. The
  existing per-user MFA guess limiter and the ability to revoke a credential
  retain their independent behavior.
- No protobuf, dependency, schema, storage, or workflow changes were needed.

**Validation:**

- TDD: `AuthenticateAppliesTheDataTierLimiterToTokenAndMfa` failed against the
  old order (invalid token was rate-limited first), then passed after reordering.
- `cmake --build --preset dev` — passed.
- `ctest --preset dev` — 1689 discovered, 1457 passed, 232 skipped because
  `FMGR_TEST_POSTGRES_URL` was unset, 0 failed.
- Focused limiter unit test — 1 passed; descriptor-driven limiter sweep — 1
  passed; audit permission/budget and MFA exhaustion tests — 3 passed; updated
  ItemType registry tests — 2 passed.
- `clang-format --dry-run --Werror` on changed C++ files — passed.
- Changed-TU local clang-tidy is not conclusive on macOS: LLVM 17 aborts in the
  system libc++ on unsupported builtin declarations. Linux clang-tidy and TSan
  checks are running in PR #190 CI.

PR #190 is a draft on `fix/178-rpc-limiter-coverage`, rebased onto
`6f8acc6` (`origin/main`).
