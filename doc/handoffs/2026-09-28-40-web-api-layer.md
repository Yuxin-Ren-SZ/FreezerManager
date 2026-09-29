# Handoff note — 2026-09-28, G1.2 API layer: codegen, client, SSE and fakes (#40, worker-3)

`src/web/src/api/` is now a complete, typed data layer and `src/web/src/test/`
holds the fakes every later screen test builds on. This was the critical path of
wave 2: G1.3 (app shell) and G3.x (feature screens) are unblocked. TODO.md G1.2,
G-arch 4/5/10, PRD §6/§10.

**Changed:**

- `src/web/scripts/gen.mjs` — the G1.1 always-exit-0 stub is gone. `npm run gen`
  runs `buf generate ../../proto --clean` with `protoc-gen-es`, both resolved
  from `node_modules/.bin` (no system protoc). See "Decisions" for the guards.
- `src/web/buf.gen.yaml` (new) — the codegen template, `target=ts` into
  `src/gen/`. The input directory is passed on the command line, not in the
  template, because buf resolves `inputs:` relative to the process CWD.
- `src/web/src/api/routes.ts` (new) — 70 unary routes, keyed `<noun>/<verb>`,
  each with its canonical proto RPC name and its generated request/response
  schemas. Hand-written on purpose: a C++ PR that adds a route must add its line
  here, and `check-routes` is what forces that.
- `src/web/src/api/client.ts` (new) — `call(rpc, request)`, typed end to end from
  the schemas. Fresh `X-Request-Id` per call, `X-CSRF-Token` from the
  `fmgr_csrf` cookie, proto3 JSON with `preserve_proto_field_names`, responses
  parsed with `ignoreUnknownFields`. Everything throws `ApiError`.
- `src/web/src/api/errors.ts` (new) — `ApiError`, the 16 wire code names, and
  `onSessionExpired()`.
- `src/web/src/api/sse.ts` (new) — `subscribeSse()` over `EventSource`, typed
  frames, `event: error` frames as `ApiError`, capped-backoff reconnect, cursor
  resume, cleanup function.
- `src/web/src/api/hooks/` (new) — `samples.ts` (`useSamples` is a
  `useInfiniteQuery` over `page_token`, plus create/update/move/delete/checkout
  mutations) and `labs.ts`; every key includes `lab_id`.
- `src/web/src/api/helpers.ts` (new) — micros ↔ `Date`, zone-aware display
  formatting, enum names/labels, `ApiError` → i18n message.
- `src/web/src/test/fakeApi.ts`, `fakeEventSource.ts`, `render.tsx` (new) — MSW
  handlers for every route backed by an in-memory demo lab with per-RPC error
  injection and latency, a scriptable `EventSource`, and
  `renderWithProviders()`.
- `src/web/scripts/check-routes.mjs` (new) + `check:routes` in `check`.
- `src/web/src/api/*.test.ts(x)`, `src/web/src/test/fakeApi.test.ts` — 79 new
  unit tests; suite total 83.
- `doc/dev/web.md` — `gen` is documented as real, `check:routes` added, the
  layout table and rule 3 updated.
- `src/web/package.json` / `package-lock.json` (under `lock:deps`):
  `@bufbuild/protobuf` (runtime), `@bufbuild/buf` + `@bufbuild/protoc-gen-es`
  (dev), `vitest-axe` (dev, pre-installed for G1.3).

**Decisions:**

- **`gen` does not trust buf's exit code.** After a successful run every
  `.proto` must have produced a non-empty `src/gen/**/_pb.ts`, else `gen` exits
  1. The stub's failure mode was exactly that `build`, `test` and `typecheck`
  all pass with no generated types; a check on the *output* is the only thing
  that catches it. `--clean` stops a deleted `.proto` from leaving a stale
  module behind, and a content hash of protos + template + buf version skips a
  no-op run (0.36 s cold, ~0.05 s warm).
- **`BUF_CACHE_DIR` defaults inside the project** (`node_modules/.cache/buf`).
  buf defaults to `$HOME/.cache/buf`, which is not writable in a sandboxed or
  locked-down container, and it fails before generating anything. Override it
  and the default is used nowhere.
- **`FMGR_PROTO_DIR`** points codegen at another proto tree. It exists so the
  failure path can be demonstrated without touching `proto/`.
- **The SSE retry loop is ours, not the browser's.** `EventSource` reconnects on
  its own and resends `Last-Event-ID`, but that retry is uncapped and has no way
  to say "this error is permanent". `subscribeSse` closes on error and reopens
  with a capped backoff, carrying the same cursor as `?since=` — the parameter
  the gateway already reads when `Last-Event-ID` is absent. `UNAUTHENTICATED`
  and `PERMISSION_DENIED` are terminal: reconnecting into them is a request loop
  against the server.
- **Backoff is deterministic** (1 s doubling to 30 s, no jitter). Jitter spreads
  a thundering herd, but a self-hosted instance has a handful of clients and an
  exact delay is something a test can assert. `reconnectDelayMs` is exported and
  unit-tested.
- **The checker fails when it cannot parse.** `check-routes.mjs` verifies that it
  parsed every `FMGR_ROUTE(` occurrence in `RestGateway.cc` and every `path:`
  literal in `routes.ts`; a checker that quietly finds no routes would pass
  forever. It also fails if the `FMGR_ROUTE` macro stops registering
  `{drogon::Post}`, which is what lets `client.ts` hardcode POST.
- **`ApiError` never surfaces the server's `message` to a user.** The gateway's
  text can name internal paths and identifiers (AGENTS.md §5), so
  `apiErrorMessage()` picks an i18n sentence from the gRPC code and the raw text
  stays on the error object for a support request.
- **`vitest-axe` is installed here even though G1.3 uses it.** The owner's #37
  review asked for exactly this so G1.3 would not need `lock:deps`; #37 merged
  without it, so the last `lock:deps` holder before wave 2 does it.

**Review round 1 (lead, on PR #42) — four findings, all taken:**

- **Any 401 clears the cache, not only `UNAUTHENTICATED`.** `fail()` in
  `client.ts` now notifies the session-expired listeners when the *status* is
  401 as well as when the code is `UNAUTHENTICATED`. A reverse proxy or load
  balancer answers 401 with its own body, which `toApiError` can only call
  `INTERNAL`; keying off the code alone left the TanStack cache intact and the
  SPA rendering stale data as if still signed in. It also covers a 401 whose
  body is not JSON at all.
- **The streaming half of `check-routes.mjs` had no drift guard.** It now
  classifies every literal `registerHandler(...)` path — a `…/watch` feed, or
  one of the three non-streaming endpoints on an explicit allowlist — and pins
  the number of registrations without a literal path to exactly one (the
  `FMGR_ROUTE` macro body). A planned bulk-import progress feed registered
  through a variable would have been invisible while the checker still printed
  `ok`.
- **`readCookie` no longer throws.** An undecodable `fmgr_csrf` value is treated
  as absent, so a corrupt cookie fails closed as a clear 403 instead of escaping
  `call()` as a raw `URIError`. The CSRF value is base64url so this cannot happen
  today, but the module's promise is that every failure is an `ApiError`.
- **`doc/dev/web.md`** records that routes without a resolver answer with the
  response message's defaults, so "the grid is empty" can pass for the wrong
  reason.

**Guards proven to fail (each planted, observed, reverted — `RestGateway.cc` is
unchanged in this branch):**

| Planted | Result |
|---|---|
| an `FMGR_ROUTE` in `RestGateway.cc` with no `routes.ts` entry | exit 1, `missing from routes.ts: /api/v1/sample/planted` |
| a `routes.ts` entry with no C++ route | exit 1, `missing from RestGateway.cc` |
| a wrong `rpc` name for an existing path | exit 1, `rpc mismatch for /api/v1/sample/list` |
| a renamed SSE path in `sse.ts` | exit 1, `missing from sse.ts` |
| the whole macro renamed so nothing parses | exit 1, `no FMGR_ROUTE lines found at all` |
| deleting `src/gen/` before `npm test` | the suite fails at collection time |
| `FMGR_PROTO_DIR` pointing at nothing | `npm run gen` exits 1 |
| a new `registerHandler("/api/v1/import/progress", …)` | exit 1, "neither a streaming …/watch feed nor on `NON_STREAMING_HANDLER_PATHS`" |
| `/metrics` registered through a `const std::string` | exit 1, "only 4 have a literal path … invisible to this checker" |

**For G1.3 and the feature tasks:**

- Import hooks from `src/api/hooks/`, never call `call()` from a component.
- `server.use(...fakeApi({ fail: { 'sample/rpc': 'CODE' } }))` — the key is the
  route key from `routes.ts`, and a typo throws at factory time rather than
  silently doing nothing.
- `fakeApi()` answers *every* route in `routes.ts`; the ones without a resolver
  return the response message's defaults. Adding a resolver there is cheaper
  than stubbing the same route in five test files.
- `subscribeSse('sample/watch', { schema, params, onFrame, ... })` returns the
  cleanup function — return it from the `useEffect` that starts the feed.
- `renderWithProviders()` already supplies the query client and a
  `MemoryRouter`; `createWrapper()` is there for `renderHook`.
- `enums.<EnumName>.<PROTO_NAME>` is the translation-key convention; add entries
  as features need them. `SampleStatus` is the worked example.
