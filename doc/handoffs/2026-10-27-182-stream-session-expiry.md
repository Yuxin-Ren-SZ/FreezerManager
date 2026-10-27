# #182 — SSE session expiry and immediate local sign-out

Worker-2 (Codex), 2026-10-27. PR #203, branch `fix/182-stream-session-expiry`.

## Result

Terminal SSE `UNAUTHENTICATED` now uses exported `reportSessionExpired()` in
`client.ts`, sharing the unary listener registry and session-epoch guard.
`MFA_REQUIRED` maps to `ApiError(UNAUTHENTICATED)` with `mfaRequired: true`, so
protected state/cache clear and routing resumes MFA rather than reconnecting.
Each opened connection captures its session epoch; stale terminal replies cannot
expire a newer cookie's session. Generic EventSource transport errors expose no
HTTP status and remain `UNAVAILABLE` with capped retries. The real gateway uses
HTTP 200 plus a terminal JSON error frame; the client does not infer an opaque
HTTP 401 from a native transport error.

Terminal streams stop and close before session listeners or subscriber callbacks
run. A callback may synchronously unsubscribe without reviving connection state
or scheduling a retry. Malformed/null error envelopes remain typed `INTERNAL`
errors and retry. `PERMISSION_DENIED` remains terminal without signing out.

Sign-out invalidates older identity loads, clears query data and sets local
unauthenticated state before awaiting the logout request. UserMenu commits
`/login` with `replace` and `flushSync`, preventing the protected-route guard
from adding an interrupted `next` URL. No delayed logout completion writes local
session state. The cross-tab opaque notification and stale unary-request guards
from #169, and the expiry notice from #181, remain intact.

## TDD and validation

Commands used Node 22 from the main checkout's `out/tools/node22/bin`; shell
setup was `AGENT_SLOT=2 source scripts/agent/env.sh`, with worktree-private TMPDIR.
Web commands ran from `src/web`.

- `NODE_ENV=development npm ci --cache "$PWD/../../out/npm-cache"`: passed,
  469 packages added. Existing ESLint deprecation and npm audit notices emitted;
  dependencies and lockfile unchanged.
- Red first, before production edits:
  `npm run test -- src/api/sse.test.ts src/app/session.test.tsx src/app/App.test.tsx`:
  **7 failed / 89 passed, 3 files failed**. Reproduced missing expiry/MFA routing,
  authenticated state during pending logout and dashboard bounce.
- Initial focused green attempt exposed the additional guard/navigation race:
  **1 failed / 116 passed**, because local sign-out added `next`. Fixed by the
  explicit synchronous menu navigation.
- `npm run test -- src/api/sse.test.ts src/app/session.test.tsx src/app/App.test.tsx src/api/client.test.ts`:
  **4 files / 119 tests passed** after final parser/cleanup regression additions.
- `npm run check`: **passed, 52 files / 871 tests**; generation, route parity,
  lint, both TypeScript checks, whole-tree Prettier, test and production build
  all passed. Initial JS **203.7 KiB gzipped / 250 KiB**. Existing
  `react-hooks/incompatible-library` warning in `ui/Table.tsx` remains (0 errors).
  Earlier check attempts found new TS-aware lint/test callback issues; fixed
  before this successful run.
- Focused final Prettier check over all seven changed web files: passed.
- `tools/check-spdx-headers.sh` and `git diff --check`: passed.

Coverage lives in `src/web/src/api/sse.test.ts`, `src/web/src/app/App.test.tsx`
and `src/web/src/app/session.test.tsx`: terminal expiry/MFA -> correct route and
empty cache; no reconnect; stale stream expiry; reentrant cleanup; pending logout
success/network failure; old WhoAmI completion; no Home heading or `next` bounce.

## Limits and follow-up

Local C++ build/CTest/sanitizers/clang-tidy were **not run**, under the lead's
explicit web-only waiver on #182. No server/proto/schema/dependency/CI or shared
fake/quantity/locale edits. Server watch revalidation is the separate #194 slice.
Native EventSource hides HTTP refusal status. Failed server revocation may leave
the cookie/server session valid until successful revocation or expiry; this
change guarantees immediate local sign-out, not remote revocation on a failed
request. Lead performs final code review and matching-head CI before merging.
