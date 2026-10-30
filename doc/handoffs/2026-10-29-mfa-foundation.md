# MFA foundation — lead closeout

Lead: Codex, slot 0. Closeout issue: [#236](https://github.com/Yuxin-Ren-SZ/FreezerManager/issues/236).
Implementation scope: prerequisites #211, #227 and #228 for accepted
[ADR0002](../adr/0002-mfa-replay-backoff.md) and parent
[#179](https://github.com/Yuxin-Ren-SZ/FreezerManager/issues/179).
Three implementation workers were reused for independent reviews, with at most
three workers active. The lead reviewed and merged each PR under the owner's
delegation. Commits retain human authorship and signoff, without coauthor trailers.

## Outcome and provenance

All three prerequisites are merged with every applicable matching-head PR
check green. The checks included actual Linux clang-tidy, both dev compilers,
release, live PostgreSQL, ASan, UBSan and TSan. ASan/TSan retain CI's
`grpc_integration|e2e` exclusions; their success does not cover those transports.

| Issue / PR | Reviewed source | Main merge | Final Build |
| --- | --- | --- | --- |
| [#227 / #235](https://github.com/Yuxin-Ren-SZ/FreezerManager/pull/235) | `cd482735c8a0b2566b60e835318ac2b33ce3e198` | `5d4c77602f4ce1dde8e1f36f31b51112cb321e4d` | [40816317370](https://github.com/Yuxin-Ren-SZ/FreezerManager/actions/runs/40816317370) |
| [#228 / #234](https://github.com/Yuxin-Ren-SZ/FreezerManager/pull/234) | `5d96a5d38b66a8e78c8f07b7a7bc7e61acd34f55` | `8a45e351631c1f621c6dce1a11c289b3dbf45ce6` | [40816308390](https://github.com/Yuxin-Ren-SZ/FreezerManager/actions/runs/40816308390) |
| [#211 / #229](https://github.com/Yuxin-Ren-SZ/FreezerManager/pull/229) | `2fbc1a7f531aefd6ce87e5cfa0f9c55b7aa52014` | `d07dec98d980e802d7ed2b28d297467a6be84d63` | [40978933596](https://github.com/Yuxin-Ren-SZ/FreezerManager/actions/runs/40978933596) |

The merge decisions are recorded on
[#227](https://github.com/Yuxin-Ren-SZ/FreezerManager/issues/227#issuecomment-6489812009),
[#228](https://github.com/Yuxin-Ren-SZ/FreezerManager/issues/228#issuecomment-6489772418)
and [#211](https://github.com/Yuxin-Ren-SZ/FreezerManager/issues/211#issuecomment-6512922674).
The merged #211 tree and reviewed source tree are identical:
`5667ff3c2e27c5c8e2fd3076051a9772f91ced26`.

- **Pure MFA policy (#227):** fixed-profile whole-window matching, coincident
  counter/floor handling, bounded backoff and observed-time policy are available
  as pure helpers. Existing runtime verifier callers are unchanged.
- **Audit integrity (#228):** migration 16 verifies the complete existing chain
  before predecessor uniqueness, including genesis. Mixed-isolation PostgreSQL
  append conflicts roll back losing domain/audit batches; SQLite preserves its
  distinct invariant-error classification. Legacy migration checksums remain
  unchanged.
- **Session writers (#211):** owning-user locks precede security reads and
  repository factories. Current-state rereads prevent stale activity writes
  from undoing revocation/MFA completion or regressing activity. Rollback and
  transaction lifetime guards fail closed; background validation stays read-only.

## Test-first and review record

Detailed commands, failure histories, corrections and coverage limits are in
the per-slice handoffs:

- [Pure policy](2026-10-28-227-mfa-policy.md): initial tests failed to compile
  against the absent API. A genuine review regression then rejected a consumed
  floor without observed time at all five policy entry points. Fixed collision
  and maximum-time vectors were independently checked.
- [Audit integrity](2026-10-28-228-audit-predecessor.md): SQLite and live
  PostgreSQL reds demonstrated bad-chain migration acceptance and committed
  mixed-snapshot forks. Review corrected SQLite conflict classification and
  pinned the fixture to migration 16. Final Linux tidy corrections preserved
  behavior.
- [Session writers](2026-10-29-211-session-writers.md): original runtime tests
  reproduced three stale-write failures while audit rollback passed. Review
  found completed native-work access and a provisional-cache bypass after failed
  commit; genuine failing tests preceded both guards. Final terminal mutation
  assertions retained persisted-row/audit checks. The stale resilience test and
  analyzer-visible optional check were corrected after actual Linux evidence.

Provider/guard and backend/audit reviewers accepted exact source `ed470e8` on
[#211](https://github.com/Yuxin-Ren-SZ/FreezerManager/issues/211#issuecomment-6504477144)
and [its backend review](https://github.com/Yuxin-Ren-SZ/FreezerManager/issues/211#issuecomment-6504485315).
Worker-3 then independently verified the final three-file delta at `2fbc1a7`
with no findings; the lead recorded that verification in the
[final handoff](https://github.com/Yuxin-Ren-SZ/FreezerManager/issues/211#issuecomment-6511744391).

## Validation and limits

Each task used its slot-private environment, build parallelism 3 and serial
CTest. Full native-loopback `ctest --preset dev --output-on-failure` results:

| Slice | Selected | Passed | Skipped | Failed | Time |
| --- | ---: | ---: | ---: | ---: | ---: |
| #227 | 1,882 | 1,639 | 243 | 0 | 184.17s |
| #228 | 1,908 | 1,629 | 279 | 0 | 185.99s |
| #211 | 1,986 | 1,684 | 302 | 0 | 188.88s |

These are separate source snapshots, not additive test totals. Local PostgreSQL
was unavailable. #227's skips were 243 URL-unset PostgreSQL cases; #228 had 275
such cases plus four opposite-backend SQLite contracts; #211 had 298 such cases
plus the same four contracts. Final Linux CI supplied live PostgreSQL coverage.

#227's anchored pure-policy selection passed 60/60 in dev, ASan and strict
UBSan. #228 configured local sanitizer presets without running sanitized
binaries; its final Linux sanitizer jobs passed. #211's focused ASan selection
passed 246 with 85 skips and zero failures (331 selected, 15.44s): 81
PostgreSQL URL-unset cases and four opposite-backend SQLite contracts. Exact
eight-target build and test-filter commands are in its per-slice handoff.
No full local ASan suite or local #211 UBSan/TSan pass is claimed.

Whole-repo formatting, SPDX and diff checks passed for the final #211 changes.
Local LLVM 17 cannot parse this Mac's newer SDK builtins, so advisory tidy runs
were failures rather than lint clearance. The actual final Linux tidy jobs ran
and passed. Earlier sandbox-bind failures, intentional red checkpoints, genuine
review failures and historical CI failures remain documented in the slice
handoffs; none was silently reclassified as a passing check.

The lead ran #211's native full suite and focused ASan after interrupting a
worker permission request that had launched no process or log. That interruption
was not an owner rejection or a test failure. The lead also handled the final
Git/PR metadata; worker-1 retained implementation/handoff ownership. All native
permissions were requested normally, with no duplicate validation run.

The automatic main-push [Build40987811471](https://github.com/Yuxin-Ren-SZ/FreezerManager/actions/runs/40987811471)
at `d07dec9` was running during closeout preparation. Its final status and the
docs merge are recorded on #236 and board #28. This docs slice does not rerun or
claim additional C++/sanitizer coverage.

## Remaining work and cleanup

#179 remains open and gates deployment beyond loopback. The next dependent
phases remain ready and unassigned: #230 persisted state/proof/secret lifecycle,
#231 provider enforcement and observability, #232 authenticated recovery, and
#233 adversarial cutover acceptance. Pure helpers and writer locks do not yet
provide persisted runtime replay/backoff protection or administrative recovery.

The merged 3D track #215/#225 consumed migration 15; #228 consumed 16. Version
17 is next unreserved, to be reserved only on implementation assignment. No
schema/proto/dependency/CI lock remains held by this foundation section.
#207's unknown TLS allocator abort and #226's static-assets flake remain separate
findings; a passing foundation run does not close either one.

The lead verified that all three clean task heads exactly matched their merged
PR source heads, then removed worktrees #211, #227 and #228. Five final #211
validation logs were retained under the main checkout's ignored
`out/validation/211-session-writers/`. Owner cookie PR #214 and its worktree,
plus the owner's untracked `doc/architecture/`, remain untouched. The docs
worktree cleanup and final board reconciliation follow the closeout merge and
are recorded with its decision.
