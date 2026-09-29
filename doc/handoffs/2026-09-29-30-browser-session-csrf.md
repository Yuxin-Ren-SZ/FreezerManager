# Handoff note — 2026-09-29, G0.1 browser session cookie + CSRF (#30, worker-1)

The gateway could only authenticate with `Authorization: Bearer`, which a browser
SPA cannot hold (anything JavaScript reads, an XSS payload reads), and the SSE
routes worked around the missing cookie with `?access_token=`, which puts the
token in proxy and access logs. This slice gives the browser a session: an
`HttpOnly` cookie set by three new routes, cookie→gRPC-metadata bridging so a
cookie call passes the same `AuthMiddleware` RBAC gate as a bearer call, and a
CSRF/Origin gate in front of every cookie-authenticated mutation. PRD §7.1,
§6, §10. Branch `feat/30-browser-session-csrf`, PR **#53**.

**Changed:**

- `src/rest/BrowserSession.{h,cc}` — **new**, the pure helpers: cookie
  attributes (`session_cookie`, `csrf_cookie`, `expired_*`), `generate_csrf_token`
  (32 random bytes, base64url, via libsodium `randombytes_buf`), `parse_cookie_header`,
  `browser_request_from` (the five fields the gate needs out of a drogon request),
  `authorization_metadata`, `csrf_denial`, and the environment rules
  (`BrowserSessionConfig::from_env`, `validate_browser_session_env`).
- `src/rest/RestGateway.cc` — three `FMGR_ROUTE`s for
  `/api/v1/auth/browser/{login,submit-mfa,logout}`; `forward()` now resolves the
  credential from header-else-cookie, and runs the gate before parsing the body
  (so a rejected request answers 403 without touching gRPC). A `success_response`
  overload pair decorates login (set both cookies, body without the token) and
  logout (expire both cookies); the `FMGR_ROUTE` macro stays five arguments wide
  because `scripts/check-routes.mjs` parses exactly that shape.
- `src/rest/SseBridge.h` — the `?access_token=` fallback is gone; `stream_sse()`
  resolves the credential through the same `authorization_metadata()`.
- `src/server/main.cc` — refuses to start when `FMGR_DEV_INSECURE_COOKIES=1` and
  `FMGR_ENV=production`.
- `src/auth/AuthTypes.h`, `src/auth/LocalAuthProvider.cc`,
  `src/server/AuthServiceImpl.cc` — `AuthToken` carries the authenticated user id
  and `Login` fills `LoginResponse.user_id`.
- `src/web/src/api/routes.ts` — the three routes; `src/web/src/api/sse.ts` — the
  auth decision documented; `src/web/src/api/sse.test.ts` — a test that no stream
  URL carries a credential.
- `tests/unit/browser_session_test.cpp` (**new**) and the G0.1 cases in
  `tests/integration/rest_gateway_integration_test.cpp`.

**Decisions:**

- **The Origin half of the gate runs without a session cookie.** A cross-site
  form POST arrives with *no* cookies at all (`SameSite=Strict`), so a check that
  waited for `fmgr_session` would never fire on login CSRF — the one place it
  matters most. Bearer calls are still skipped entirely (a bearer token is not
  ambient), and safe methods are never gated, which is what keeps the SSE feeds
  working. Bearer calls with a foreign `Origin` and a stale cookie are tested as
  unaffected.
- **`Origin` is compared by authority, not by full origin.** Behind a
  TLS-terminating proxy the gateway cannot tell `http` from `https` from the
  request alone, so a scheme comparison would 403 every production mutation; the
  authority is what decides whether an origin is this site. `FMGR_WEB_ORIGIN` is
  compared as the full normalized origin, and is an *extra* accepted value — the
  request's own host always works, which is what the Vite proxy presents
  (`changeOrigin: false`, `doc/dev/web.md`).
- **`user_id` is filled at the source, not synthesized in the gateway.**
  `LoginResponse.user_id` is declared in `auth.proto` but `AuthServiceImpl::Login`
  never set it, and the browser login body needs it with no other RPC available
  (`WhoAmI` is G0.2). So `AuthToken` grew a `user_id` field. Additive for the
  existing `auth/login` route. The alternative — inventing a second response
  message in the gateway — would have needed the proto lock.
- **A re-login while a stale session cookie is still in the jar must pass the
  gate.** On a fresh browser `login` carries no cookie and is not
  cookie-authenticated; with a stale one it is, and the SPA always has both
  cookies (logout clears both together, and both are session cookies), so
  `client.ts` already sends the matching `X-CSRF-Token`. Tested both ways.
- **Logout clears the cookies only on a 2xx.** Clearing on a *gate* rejection
  would turn a cross-site 403 into a logout CSRF, so the failure path is left
  alone; an already-invalid session gets a 401 and the SPA's
  `onSessionExpired` handles it.
- **`switch`ing on the response type instead of widening the route macro.** The
  checkout macros are parsed by a regex in `check-routes.mjs` that requires
  exactly five arguments, and a sixth would have made the three new routes
  invisible to the checker — which fails in *both* directions, so the SPA side
  would have had to be wrong too. Overloads on the response type keep the macro
  shape and the decoration next to `forward()`.
- **Comments inside `apiRoutes` may not contain commas.** The checker's
  brace-aware entry scanner splits on a top-level comma and is not comment-aware,
  so a prose comment with a comma becomes a bogus "entry without a path/rpc
  pair". The explanation therefore lives in the table's doc comment and the
  in-table header is comma-free.

**Tests:**

- `tests/unit/browser_session_test.cpp` → 38 new cases; whole binary
  `./out/build/dev/tests/unit/freezermanager_rest_unit_tests` → **58/58 passed**.
- `./out/build/dev/tests/integration/freezermanager_rest_gateway_integration_tests`
  → **57/57 passed**, three consecutive runs (the login rate limiter is per
  source IP at 30/5-per-second, so the browser tests share one cached session per
  account; only the tests *about* logging in or out mint their own).
- `ctest --preset dev` → **100% tests passed out of 1493**, 89.27 s (after
  rebasing onto `origin/main` `925f423`).
- `ctest --preset asan --label-exclude "grpc_integration|e2e"` (what CI runs) →
  **100% passed out of 1172**. `tsan` was not run: the SSE worker thread and loop
  guard from #29 are untouched.
- `cd src/web && npm ci && npm run check` → `check-routes: ok — 73 unary routes
  and 2 SSE routes agree`; 258/258 tests; bundle 152.5 KiB gzip of 250 KiB; lint
  0 errors (the one warning is the pre-existing `Table.tsx:99` one #46 recorded).
- `clang-format --dry-run --Werror` over every tracked C++/proto file → clean.
  `tools/check-spdx-headers.sh` → clean.

**Known limitations / follow-ups:**

- **`run-clang-tidy-17` could not be run on this machine.** The `clang-tidy`
  17.0.1 wheel cannot parse the macOS SDK's libc++ headers at all
  (`__builtin_clzg`/`__builtin_ctz` undeclared, 17 `clang-diagnostic-error`s) —
  an untouched file (`src/obs/Log.cc`) fails identically, so it is the
  environment, not this diff. The CI gate on Linux is the first real check; the
  code avoids the patterns that would obviously trip it (no `std::optional`
  dereference after a separate `ASSERT_TRUE`, no short non-loop identifiers, all
  pure helpers `[[nodiscard]]`).
- **`scripts/agent/conan-install.sh` (from #43) does not exist on `origin/main`**
  (#43 is still open), so the `asan` tree was configured by hand with the
  `CMakeUserPresets.json` include-list workaround from #36. Nothing in the PR
  depends on either.
- The rest-gateway integration suite cannot run under `asan` on this machine: it
  aborts in `absl::Status::ok()` (`use-after-poison`, `cq_end_op_for_pluck`) as
  soon as the second gRPC call runs, which is the documented library-internal
  false positive that makes CI exclude the `grpc_integration` label. New logic is
  covered under asan by `freezermanager_rest_unit_tests`.
- `auth/browser/*` in `routes.ts` uses `LoginResponseSchema` as its output, so a
  caller that reads `session_token` from that route gets the empty default. The
  alternative is a new proto message, which needs `lock:proto`; the table comment
  says so.
- `fakeApi()` answers the three new routes with the response message's defaults
  (no `fmgr_session`/`fmgr_csrf` cookies, since MSW cannot set them the way the
  gateway does). A sign-in screen test will need a resolver plus a cookie
  double — G2.1 owns the sign-in screen; G0.2 (`WhoAmI`) owns "who am I after a
  reload".
- Nothing in this PR was added to the `AuthMiddleware` registry: the three routes
  forward to `Login`/`SubmitMfa`/`Logout`, which are already registered.
