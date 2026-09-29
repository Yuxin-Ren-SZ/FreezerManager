# Handoff note — 2026-09-27, board-2 SSE shutdown use-after-free (#29, worker-1)

`freezerd`'s REST/SSE watch streams no longer post to a destroyed Drogon IO loop,
which is what made the gateway integration tests SEGFAULT on macOS and could
crash a real shutdown on any platform. Spec: PRD §6 (REST/SSE gateway). No TODO
ID: this is board starter-backlog item 2, and it is the prerequisite for G0.1
(#30).

**Changed:** `src/rest/SseBridge.h` only.

- New `fmgr::rest::detail::SseLoopGuard`, one per IO loop thread
  (`thread_local` in `sse_guard_for_current_loop()`), holding `mu`, the loop
  pointer, and a weak list of that loop's live streams.
- `SseLoopGuard::post(func)` takes `mu` and calls `loop->queueInLoop(func)` only
  while the loop pointer is non-null, so a worker that finishes after the loop
  is gone drops its frames instead of queueing into freed memory.
- `sse_guard_for_current_loop()` installs the loop's `runOnQuit` hook (capturing
  the guard as `owned_guard`). The hook nulls the loop pointer under `mu` and,
  while the loop still exists, `TryCancel()`s every parked `Read` (so the worker
  wakes up and exits) and resets each `ResponseStream` — on the loop thread,
  where Drogon requires it.
- `SseStreamState` (formerly `StreamState`) moved next to the guard, and every
  read of `state->stream` is now a null-checked loop-thread access: the
  keepalive timer, the posted frame lambda, and the posted finish lambda
  (which otherwise returns immediately).
- The posted finish lambda resolves its loop with
  `trantor::EventLoop::getEventLoopOfCurrentThread()` instead of capturing a
  raw pointer, and `std::shared_ptr<drogon::ResponseStream>` lets the stream
  outlive the callback that created it.
- `stream_sse()` now only resolves the bearer, wraps the async response and
  registers the routes' content type/headers: the loop-thread kickoff is
  `detail::start_sse_stream()` and the worker body is `detail::sse_read_loop()`.
  That split exists to keep `stream_sse()` inside clang-tidy's cognitive
  complexity budget (it was 28 against a threshold of 25), the same shape
  `src/server/main.cc` uses with `apply_tls_env()`. The async-stream handler is
  `mutable` and moves its captured `open_reader`/`frame_fn` into the helper, so
  it relies on `newAsyncStreamResponse()` invoking the handler once per
  response.

**Decisions:** the fix keeps the existing "one worker thread per stream" design
and adds no new locking on the hot path beyond the guard mutex around
`queueInLoop`; the alternative (a `shared_ptr`-owned loop or a
`std::weak_ptr<trantor::EventLoop>`) does not work because Trantor's loop
lifetime is not managed by the caller. The worker still touches only the reader,
`ClientContext::TryCancel()` and the atomic liveness flag. This landed as a
faithful reproduction of the owner's uncommitted fix; two divergences from that
reference, both forced by CI gates the lead reported in #29: the
`clang-format-17` line join at `SseBridge.h:65` (`template <typename F> void
post(F&& func) {`), and the renames plus helper extraction that
`run-clang-tidy-17 -warnings-as-errors` requires (short identifiers `fn`/`g`,
and the cognitive-complexity budget).

**Tests:** no new test file — the four existing `RestGatewaySse.*` tests in
`tests/integration/rest_gateway_integration_test.cpp` are the regression tests,
and two of them segfaulted before this change.

- `ctest --preset dev -R 'RestGatewaySse' --output-on-failure` — before:
  2 of 4 SEGFAULT (`AuditWatchStreamsNewEvent`, `SampleWatchStreamsNewSample`),
  both in `Global test environment tear-down`; after: 4/4, and 4/4 again across
  `--repeat until-fail:10` both before and after the clang-tidy refactor.
- `ctest --preset dev --output-on-failure` → 1399/1399, 0 failed (219 Postgres
  tests skipped, `FMGR_TEST_POSTGRES_URL` unset).
- `ctest --preset asan --output-on-failure --label-exclude 'grpc_integration|e2e'`
  → 1125/1125, the asan command CI actually runs. On macOS the
  label-*included* variant (`ctest --preset asan -R 'RestGateway'`) aborts every
  test with ASan reports inside un-instrumented Conan libraries (libc++
  container annotations in `drogon::CacheMap`, `use-after-poison` in
  `absl::Status` / gRPC `status_helper.cc`); the first aborts are tests that
  never open an SSE stream, so it is the instrumentation-boundary class the CI
  comment at `.github/workflows/build.yml:167-175` excludes.
- `clang-tidy -p out/build/dev src/rest/RestGateway.cc` (17.0.1, local) → no
  finding in `SseBridge.h`. Local clang-tidy output is a superset of CI's
  (AppleClang here vs clang-17 on the runner): the five remaining findings are
  pre-existing ones in `src/core/uuid.h` and `src/rest/RestGateway.cc` that the
  green CI job does not report.

**Known limitations / follow-ups:**

- The guard is disarmed by the loop's `runOnQuit` hook. A loop destroyed
  *without* running its quit callbacks would leave the pointer armed; Drogon's
  main loop and the test harness both call `quit()`, and the worker's frames
  would then land in `post()` — worth revisiting only if a future embedder
  tears the loop down some other way.
- The `?access_token=` query-parameter fallback is still in `stream_sse()`
  (tokens in URLs land in access logs). Removing it is G0.1 (#30).
- Both fixes for the local `asan`-preset friction are out of scope here and
  belong to the tooling issue the lead filed: the Conan-generated
  `CMakeUserPresets.json` cannot include `out/conan/dev` and `out/conan/asan`
  at once (both define `conan-debug` → `Duplicate preset`), and the
  `grpc_integration` false positives on macOS are only documented in the CI
  workflow comment, not in `doc/dev/`.
