# Smaller REST and MFA-design sprint — 2026-10-27/28

Scope: two small REST changes and an implementation-ready MFA design. Five
smaller-model agents contributed, at most three active: Luna/medium authored
#202; Sol/high authored #196 and #208; separate Sol/high agents reviewed static
assets and the MFA design. Lead reviewed every PR, requested revisions and
checked all matching-head applicable CI before merging. Lead closeout is #209.

| Issue / PR | Merge | Evidence |
| --- | --- | --- |
| [#196 / #210](2026-10-27-196-static-dotfiles.md) | `1ae4d23` | Five red cases, then 31/31 focused. Full dev 1,557 passed / 232 PostgreSQL skips / 0 failed. Focused ASan/UBSan 46/46 each; TSan 22/22 pure cases. Separate review accepted and independently ran 46/46. All 11 final-head PR checks passed, including actual Linux clang-tidy. |
| [#202 / #213](2026-10-27-202-defined-rest-status-tests.md) | `ac9ccbd` | Existing fixture reproduced strict UBSan invalid-enum abort. Defined sentinel pins HTTP 500 and INTERNAL with no production change. Focused strict 16/16; full dev and unfiltered strict UBSan each 1,549 passed / 232 PostgreSQL skips / 0 failed. Lead independently ran 16/16. All 11 final-head PR checks passed, including actual Linux clang-tidy. |
| [#208 / #212](2026-10-27-208-mfa-hardening-design.md) | `120039e` | Accepted ADR after independent review and R1/R2/R3 revision. 35 local link targets and 34 source anchors validated. Final docs lint/web/CLA/changes passed; C++ matrix/tidy correctly skipped. No runtime tests or protection claimed for prose. |

The linked worker handoffs preserve exact build/test commands and selections.
Lead's independent command for #202 was
`ctest --preset dev -R '^RestErrorTranslationTest\.' --output-on-failure`
in slot 1: 16 passed, 0 skipped/failed (0.46 s). The separate static reviewer
recorded [46 focused passes on #196](https://github.com/Yuxin-Ren-SZ/FreezerManager/issues/196#issuecomment-6457961219),
without rebuilding or independently rerunning the full/sanitizer suites.

#202's earlier strict UBSan command excluded grpc_integration/e2e and passed
1,075 with 232 PostgreSQL skips; it is a subset, not the full preset. Lead
required the unfiltered strict run before handoff. The 1,549/232/0 result above
includes TLS/mTLS; #207's allocator abort did not recur locally. That does not
identify or fix its cause. PostgreSQL was unavailable locally; Linux CI supplies
live-backend evidence. Counts belong to each PR's source base, not a claimed
new combined-main test run.

#196 denies every dot-prefixed decoded path component, including .well-known,
and canonical hidden targets below web_root. Visible in-root aliases and a
hidden index fallback cannot bypass it. Ordinary interior dots and hidden
absolute root ancestors remain supported. HTTP coverage sends raw encoded paths
and checks headers and absence of secret/SPA bytes. Bare /api remains #186.

The static ASan result is local macOS with the existing Apple container-overflow
option for uninstrumented Drogon; it is not Linux LeakSanitizer proof. TSan
registers only 22 pure path/config cases and excludes the three existing Drogon
HTTP/TLS/disabled binaries. Local clang-tidy could not parse the current SDK;
actual Linux clang-tidy passed on both C++ PR heads. Qt6 was not installed;
no desktop-client coverage is claimed. No new sanitizer suppression was added.

[ADR0002](../adr/0002-mfa-replay-backoff.md) remains an implementation contract.
Independent review found two blockers; lead added an initialization-audit
clarification. The revised design adds common audit predecessor uniqueness and
whole-transaction conflict handling across isolation modes, durable
current-generation TOTP proof and guarded secret/session writers, and audited
lazy state initialization. Actor authorization precedes observable missing-target
errors. [Independent re-review accepted the revision](https://github.com/Yuxin-Ren-SZ/FreezerManager/issues/208#issuecomment-6458154474)
and [lead accepted it](https://github.com/Yuxin-Ren-SZ/FreezerManager/issues/208#issuecomment-6458164224).
The planned replay/restart/concurrency/provenance tests are not executed tests.

Implementation remains under P1 #179 with #211's stale session-writer
prerequisite. #211 is source evidence with deterministic runtime reproduction
pending. #179 continues to gate non-loopback deployment; #207 remains a separate
Linux allocator-stack investigation. No schema/proto/dependency/CI changes or
migration reservation occurred in this sprint.

Process note: PR #213 had one commit but was squash-merged instead of the guide's
rebase merge. Contents and signoff are retained; main history was not rewritten.
The deviation is also recorded in the issue's lead decision.

Owner parallel work is separate: cookie-ordering #204/#214 and 3D layout #215
with integration branch feat/215-layout-3d. Their PRs/worktrees are untouched.
3D PRs do not merge to main without the owner's instruction. Main migration 15
was free at closeout; provisional use on the 3D branch is renumbered at
integration if necessary, per its owner's board announcement. The owner's
untracked doc/architecture is preserved. Only this sprint's worker worktrees
were removed. The prior sprint's d527c0c main-push Build40610019599 subsequently
passed without a retry; new automatic main-push runs are reported separately
from the passing PR checks.

Lead closeout validation: full-diff `git diff --check origin/main...HEAD`,
`tools/check-spdx-headers.sh` and local Markdown resolution (15 file targets,
0 missing) passed. Only README, TODO and this handoff changed. No local C++ or
npm rerun is claimed for this prose-only PR; applicable PR CI still gates merge.
