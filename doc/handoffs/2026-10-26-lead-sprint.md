# Lead sprint — 2026-10-26

Owner assigned Codex as lead and requested 5–10 smaller-model subagents per
sprint. This sprint plans nine across bounded waves, at most three active,
with worker slots reused only after handoff. Each implementation has its own
issue, worktree and PR. Lead reviews code and matching-head applicable CI;
workers never merge. The owner's untracked `doc/architecture/` is preserved.
Work resumed on 2026-10-27 after the client worker's usage limit; its preserved
test edits and the server worker's completed test logs were reused.

## Carry-over reviews and merges

| Issue / PR | Result | Independent review evidence |
|---|---|---|
| #44 / #168 | Squash-merged `41d3cf3` | Sol/high; focused REST/static tests 104/104. All owner blocking findings fixed; matching-head Linux dev/release/ASan/UBSan/TSan/clang-tidy/lint/web/CLA green. |
| #178 / #190 | Squash-merged `fee540b` | Sol/high; focused admission/permission tests 8/8. Manual permissions and registry agreement retained; descriptor-driven limiter coverage. Matching-head Linux matrix and clang-tidy/lint/web/CLA green. |

For #168, Linux LeakSanitizer ran the new HTTP tests without leak-disabling
options. The three upstream Drogon HTTP integration binaries are excluded from
TSan as the owner directed; all 17 pure path/config tests remain enabled.
The production CSP smoke covers initial sign-in, not every interactive path.
Bare `/api` handling stays #186; dotfile hardening is queued as #196.

The independent review commands were:

- #168: `ctest --preset dev -R 'RestStaticAssets|RestGateway' --output-on-failure`
  — 104 passed, 0 failed.
- #190: focused descriptor/limiter, audit-scope, MFA and registry tests — 8
  passed, 0 failed. The issue/worker handoff records the exact focused filter;
  Linux CI run 39893983555 tested 1,689/1,689 in Clang dev and 1,245/1,245
  under TSan with `grpc_integration|e2e` excluded.

Both review runs needed unsandboxed execution to bind loopback test servers.
Initial sandbox setup failures were not reported as passing tests. Local
arm64 macOS ASan is not Linux leak coverage.

## Sprint implementation modules

| Issue / PR | Result | Evidence |
|---|---|---|
| #144 / #195 | Squash-merged `d7b8c57` | Sol/medium; 3 limiter tests each repeated 2,000 times, 11 file tests, full dev 1,495 passed / 232 PostgreSQL skips / 0 failed. Planted doubled refill failed at replay seed 144 and was reverted. All applicable Linux CI green, including actual clang-tidy. |
| #193 / #200 | Squash-merged `fc251ec` | Luna/medium implementation, Sol/high revision after lead reproduced decimal rounding. Final focused fake/form tests 96/96; full npm check 859/859; initial JS 203.6 KiB gzip. Final lint/web/CLA green, C++/tidy correctly path-skipped. |
| #198 / #201 | Squash-merged `a526645` | Sol/high design; accepted ADR plus handoff, 25 references and whitespace checked. Applicable lint/web/CLA green, C++/tidy correctly skipped. No local C++/npm result claimed for docs-only scope. |
| #182 / #203 | Squash-merged `337f9b6` | Sol/high implementation plus independent Sol/high review. Focused 119/119; full npm check 871/871; initial JS 203.7 KiB gzip. Final applicable CI green; C++/tidy correctly skipped. |

The bounded numeric parent #180 is split into #192 (server/shared CSV parser)
and #193 (web form/fake). The server stores whole int64 counts; fractional,
negative, nonfinite and out-of-range doubles must be refused before casting.
CSV uses checked int64 `from_chars`, so it retains the full integer range;
its separate negative-quantity gap is included by lead decision on #192.
UpdateSample has full-replacement semantics, not an update mask.

The first #200 version passed CI but still converted typed
`9007199254740993` to `9007199254740992`. Review required exact raw decimal
coefficient/exponent comparison with the submitted integer, no-request
regressions, and canonical display of existing large integers. The revision
added 19 tests; it preserves exactly representable large values rather than
reducing the server/fake range to JavaScript's safe-integer range.

Strict UBSan validation of #192 also exposed the unchanged baseline
`RestErrorTranslationTest.StatusCodeOutOfEnumRange`, which manufactures enum
value 99. Follow-up #202 tracks that fixture separately; strict full-suite
failure and any narrowly excluded run must be reported separately from the
standard preset and strict quantity regression results.

#194 implements P1 part 1 of #148 after #192 merges; the accepted ADR in #198
and client #182 are already merged.
Background authorization checks must neither refresh idle activity nor charge
another data-tier admission token. Parts 2/3 of #148 stay open separately.

#182 ends local state/cache before awaiting logout, maps terminal SSE errors
through the shared session-expiry listeners, and preserves MFA routing and
stale-session epochs. Reentrant callbacks cannot reconnect a closed stream.
The independent review found no blocker, while identifying a cookie-response
ordering limitation for overlapping logout/login calls; queued P2 #204 needs a
broader ordering design and real-browser regression. It is not claimed solved.

Implementation PRs and final validation will be recorded before this lead
handoff is marked complete.

## Remaining deployment and product work

#179 still gates non-loopback deployment: atomic TOTP replay prevention and
durable guessing backoff/admin unlock need a recorded design. Migration 15
is not reserved by this sprint. #133 is now unblocked by SPA hosting, then
#134 follows; dashboard, remaining admin screens, G5 end-to-end/accessibility,
packaging and operator docs remain open. No deployment gate was waived.
