# Handoff note — 2026-06-15, M3 SSE streaming slice 1 (WatchAuditFeed → SSE)

First server-streaming RPC + reusable SSE bridge, proving the pattern end-to-end.

**Changed/new:**
- `proto/fmgr/v1/audit.proto` — `rpc WatchAuditFeed(WatchAuditFeedRequest) returns
  (stream AuditEvent)` + request message (lab/entity filters + `since` cursor).
- `src/server/AuditServiceImpl.{h,cc}` — poll-tail `ServerWriter` handler: authorize
  once at stream-open (same gating as `ListAuditEvents`), then re-query
  `at >= cursor` every ~1s, dedup same-microsecond ids, write each as proto, exit
  on `ServerContext::IsCancelled()`. Registers the RPC (AuditRead). Helpers
  `build_watch_query` / `emit_new_events` keep cognitive complexity under budget.
- `src/rest/SseBridge.h` (new) — generic `stream_sse<RespT>()`: drives the gRPC
  `ClientReader` on a worker thread, posts each frame to the connection's event
  loop via `queueInLoop` (trantor `AsyncStream::send` is loop-thread-only), 15s
  keepalive comments, maps a non-OK `Finish()` to an `event: error` frame. The
  loop-side keepalive `TryCancel`s a parked Read when the client disconnects.
- `src/rest/RestGateway.cc` — `GET /api/v1/audit/watch` route; `since` resume via
  `Last-Event-ID`/`?since=`; emits `id:`+`data:` SSE frames.
- Tests: `audit_service_integration_test.cpp` (+5: live-tail receives event,
  member/outsider/no-bearer denied, `ListSince` range regression);
  `rest_gateway_integration_test.cpp` (+2: raw-socket SSE positive + no-bearer
  error frame).

**Bug fixed along the way:** the SQLite **and** Postgres audit repositories
rendered *every* predicate as `column = ?`, so `since`/`until` (range predicates)
silently matched nothing — `ListAuditEvents` since/until were broken and untested.
Now render `=`/`>=`/`<=` by operator and throw on unsupported ops.

**Known limitations / follow-ups:**
- Polling (~1s), not LISTEN/NOTIFY — portable across SQLite/Postgres; push is a
  later optimization (`caps().listen_notify` still unused).
- SSE auth failures surface as an `event: error` frame (HTTP 200 already
  committed), not an HTTP 401.
- TSan does **not** cover the bridge: the streaming tests are `grpc_integration`-
  labelled and excluded from asan/tsan (Conan gRPC/absl are uninstrumented). The
  bridge's safety rests on the queueInLoop serialization design + review.
- Next on this bridge: `WatchSampleList` (delta add/update/remove), bulk-import
  progress, periodic mid-stream re-authorization.

Verification: `cmake --build --preset dev` clean; `ctest --preset dev -j1` — only
the 12 pre-existing baseline failures (SqliteBackend file-detection,
CustomFieldResolver, E2E unauth); new streaming/SSE tests green;
`run-clang-tidy-17 -p out/build/dev src/server src/rest src/storage/...` clean;
`clang-format` clean.
