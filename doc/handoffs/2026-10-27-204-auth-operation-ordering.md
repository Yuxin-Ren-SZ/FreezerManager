# #204 — Order overlapping browser logout and login

Owner session (Claude Code), 2026-10-27. Branch `fix/204-auth-operation-ordering`.

## Problem

`RestGateway.cc` answers a successful `auth/browser/logout` with deletion
cookies for `fmgr_session` and `fmgr_csrf`, and `auth/browser/login` with fresh
ones. The browser applies each `Set-Cookie` when that response arrives. If a
logout is still in flight when a newer login succeeds, the late logout reply
expires the newer cookie, and its `announceSessionChange()` then reloads the
session into the signed-out state. #203 made this reachable in one tab, because
sign-out now shows the login screen before the server answers. Across tabs it was
already possible, because tabs share the cookie jar.

## Design

The gateway cannot fix this. A deletion cookie cannot be made conditional on the
jar still holding the session the request revoked, and the server never sees the
newer cookie on the old request. The order is therefore enforced where the
cookies are applied: in the SPA.

- **One order for every browser auth operation.** `signIn`, `submitMfaCode` and
  `signOutOfBrowserSession` (`src/web/src/api/auth.ts`) each run inside
  `withBrowserAuthOrder` (`src/web/src/api/authOrder.ts`). An operation starts
  only after the previous one's response, and the cookies on it, has been applied
  or abandoned. Its session-change announcement is made while the order is held.
- **Across tabs:** an exclusive Web Lock named `fmgr-browser-auth-v1`
  (`navigator.locks`). Without the Web Locks API (insecure context, old browser),
  a promise chain local to the tab keeps the same order within that tab only.
- **Delayed logout:** the logout fetch is aborted after
  `BROWSER_LOGOUT_TIMEOUT_MS` (10 s), so a hung logout cannot block every later
  sign-in. Aborting keeps the order sound. If the response headers had arrived,
  the deletion is already applied before the order is released. If not, the
  browser never delivers the aborted response, so its deletion cookie never lands.
  Local state is already signed out (`SessionProvider.signOut`, unchanged).
- **Failed operations** release the order like successful ones.
- **Transport:** `call()` accepts `{ signal }`. A body read cut off by an abort
  or a dropped connection now surfaces as `ApiError('UNAVAILABLE')` rather than a
  raw `DOMException`/`TypeError`, keeping the module's "always `ApiError`" rule.

Unchanged: cookies stay `HttpOnly` and out of JavaScript; nothing is written to
browser storage beyond the existing opaque session-change signal. CSRF
double-submit, MFA pending/complete semantics, server-side revocation and the
gateway are untouched. No proto, schema, dependency or CI change.

## Remaining limitations

- **No cross-tab order without Web Locks.** Production requires `Secure`
  cookies and so a secure context, where the API exists. The
  `FMGR_DEV_INSECURE_COOKIES` dev loop on a non-loopback HTTP origin gets the
  tab-local order only.
- **Login and MFA have no deadline.** A login that hangs holds the order until
  the browser's own network timeout fails it. Logout in another tab still clears
  that tab's local state at once; its request waits. Aborting a login mid-flight
  could leave a set cookie behind an error the user was shown, so it was not
  added.
- **Clients outside this SPA** (an older bundle still open in a tab, scripts
  using cookies) do not take the lock. The overlap stays possible for them,
  and its outcome remains fail-closed (signed out, never a wrong identity).
- **An abandoned logout may not have revoked the session**, the same as any
  failed logout today: the old cookie stays in the jar until a later login
  replaces it or the session expires server-side.
- **No real-browser regression.** The repo has no browser e2e harness. The
  regression models the browser cookie jar in MSW, applying `Set-Cookie` on
  delivery and never for an aborted request. A Playwright check would need
  `lock:deps`.

## TDD and validation

Node 22.23.3 (`out/tools/node22/bin` from the main checkout); commands run from
`src/web`.

- Regression test `src/web/src/api/authOrder.test.ts`, committed red first
  (`3223662`):
  `NODE_ENV=test npx vitest run src/api/authOrder.test.ts` gave **3 failed /
  1 passed**. Failing: same-tab login sent before the logout reply landed;
  cross-tab login/MFA not ordered behind another tab's logout; hung logout never
  abandoned (test timed out). Passing: the failed-logout guard.
  - Each test holds the old logout response, completes a newer login, releases
    the logout, then asks cookie-only `WhoAmI`, which must name the newer login.
  - The cross-tab test imports two independent module graphs sharing one fake
    `LockManager`, so only the origin-wide lock can order them. It also asserts
    that all three operations request `fmgr-browser-auth-v1` in exclusive mode.
- After the fix: `npx vitest run src/api/authOrder.test.ts src/api/auth.test.ts`
  gave **7 passed**. `src/api/authOrder.test.ts`, `src/app/session.test.tsx` and
  `src/app/App.test.tsx` passed 73/73 on three consecutive runs.
- `npm ci` passed (lockfile unchanged). `npm run check` passed: gen, routes,
  lint (0 errors; 1 existing warning in `src/ui/Table.tsx`), typecheck, format,
  **53 files / 875 tests passed**, build; initial JS 203.9 KiB gzipped of 250 KiB.
- C++ suite not run: no C++, proto or CMake file changed, and CI path-gates this
  web-only change.
