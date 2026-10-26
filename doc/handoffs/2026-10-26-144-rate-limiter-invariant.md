# #144 — Valid token-bucket fuzz invariant

Worker: worker-3 (Codex Sol). Branch: `test/144-rate-limiter-invariant`.

The old assertion bounded the longest streak of successful requests at three
bucket capacities, even while time advanced and refilled the bucket. A valid
limiter can exceed that streak length. Production `RateLimiter.h` is unchanged.

The replacement checks every inclusive request interval `[i, j]`:

```
allowed[i..j] <= C + r * (t_j - t_i) + 1e-9
```

At request i there can be at most C available tokens, and only r times elapsed
seconds can arrive by request j. Both endpoints count; a zero-length interval
therefore permits at most C requests. No ceiling or integer rounding is used.
The epsilon absorbs double arithmetic error over 2020 requests (roughly 500
simulated seconds), while remaining far below one token.

Let A_before_i count successes strictly before i and A_after_j count successes
through j. Rearranging gives
`A_after_j - r*t_j <= C + (A_before_i - r*t_i)` for every i <= j.
Keeping the minimum right-hand term checks all interval starts in O(n) time and
O(1) space. Rejected requests remain in the interval sequence; they do not reset
anything. Initial empty-bucket and long-idle/instantaneous-burst phases also pin
the capacity behavior before the random 0–500 ms gaps.

The property now owns its generator, seeded solely from GTest's reported seed.
`SCOPED_TRACE` prints `--gtest_random_seed=<seed>` on failure. It does not depend
on ASLR or other tests consuming the shared generator. Two independent runs
with `--gtest_shuffle` to display the automatic seed printed 72960 and 72965;
each passed. An explicit `--gtest_random_seed=144` replays the planted failure.
The other fuzz properties are unchanged.

## Red/green evidence

After writing the replacement property, temporarily changed only the refill
expression in `src/rpc/RateLimiter.h` from
`elapsed.count() * config_.refill_per_sec` to
`elapsed.count() * config_.refill_per_sec * 2.0`, rebuilt the fuzz target, and ran:

```
out/build/dev/tests/fuzz/freezermanager_fuzz_tests --gtest_filter=FuzzRateLimiter.TokenCountNeverExceedsCapacity --gtest_random_seed=144
```

Exit 1; 0 passed, 1 failed. First diagnostic:

```
Expected: (allowed_total - refill) <= (kCapacity + min_interval_start + kEpsilon), actual: -9.61 vs -9.9999999989999999
allowed=11 elapsed_refill=20.609999999999999
Replay with --gtest_random_seed=144
```

The plant was reverted before all passing checks; `git diff` for the production
header is empty. Full mutation output: worktree-local
`out/planted-doubled-refill.log` (not committed).

## Validation

All commands run after `AGENT_SLOT=3 source scripts/agent/env.sh`.

- `cmake --build --preset dev --target freezermanager_fuzz_tests`: passed.
- `ctest --preset dev -R FuzzRateLimiter --output-on-failure --repeat until-fail:2000`:
  all 3 tests passed 2000 repetitions each, 0 failed/skipped, 22.06 s.
- `out/build/dev/tests/fuzz/freezermanager_fuzz_tests --gtest_random_seed=144`:
  11 passed, 0 failed/skipped.
- `clang-format --dry-run --Werror tests/fuzz/fuzz_ratelimit_and_validator.cpp`:
  passed after formatting, clang-format 17.0.6.
- `tools/check-spdx-headers.sh` and `git diff --check`: passed.
- `clang-tidy tests/fuzz/fuzz_ratelimit_and_validator.cpp -p out/build/dev`:
  unavailable as a usable local check: installed clang-tidy 17.0.1 exits 1 with
  20 errors in the newer macOS SDK's libc++ headers, including undeclared
  `__builtin_clzg` in `__bit/countl.h`. Linux CI remains required.

The first full CTest run was sandboxed and started before rebasing onto
`fee540b`; its gRPC setup failures (`failed to start gRPC server on localhost:0`)
are not claimed as passing evidence. The final rebuilt suite is run outside the
sandbox so test servers can bind loopback sockets.

Final base `fee540b` validation:

- `cmake --build --preset dev`: passed after rebasing (exit 0).
- `ctest --preset dev` (outside sandbox): 1495 passed, 232 PostgreSQL tests
  skipped without `FMGR_TEST_POSTGRES_URL`, 0 failed, 1727 total, 164.96 s.
  Output: worktree-local `out/ctest-final-dev.log`.
- The discarded sandbox/pre-rebase run had 1053 passed, 401 failed and 232
  skipped (1686 total), 123.91 s; failures came from blocked server binds.
- Initial draft-head CI lint identified line 64's refill-expression wrap; the
  final commit applies clang-format 17 and its dry-run passes. Matching-head
  Linux CI is required before merge.
