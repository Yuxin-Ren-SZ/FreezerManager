# #198 — Watch authorization revalidation design

Worker-3 (Codex), 2026-10-26. Design accepted; implementation remains #194.

## Result

[ADR 0001](../adr/0001-watch-authorization-revalidation.md) records the minimum
provider/middleware protocol approved by the
[lead DECISION](https://github.com/Yuxin-Ren-SZ/FreezerManager/issues/198#issuecomment-6429159909)
after the [concrete proposal](https://github.com/Yuxin-Ren-SZ/FreezerManager/issues/198#issuecomment-6429140759).
The actual ADR mandate is TODO.md Section N, N4; AGENTS.md has no N4 section.

- Add pure virtual `IAuthProvider::revalidate_token` and explicit middleware
  `reauthorize` / `reauthenticate`; handlers retain interface-only dependencies.
- Admission preserves activity and its one limiter charge. Background checks
  refresh token/MFA/grants and RLS, without storage writes, idle renewal or new
  limiter charges. Preserve authz_version and API-token scope semantics.
- Recheck both watch feeds before each batch at their existing ~1 second cadence,
  preserving scoped AuditRead and deployment-wide SystemAdmin semantics. Record
  terminal statuses, blocking-write/queued-frame limits and RESTerror plus EOF.
- Define deterministic marker/barrier tests and timestamp mutation, positive and
  negative controls, middleware accounting/registry tests, and honest unit/TSan
  coverage limits. No implementation or test files changed.

## Validation

Inspection baseline: `fee540bc2b0cb1059e5a958c53907971c2bfaf20`, including merged
#178/#44. Commands run from `.worktrees/198-watch-revalidation-design` with slot 3
environment:

- `python3 /private/tmp/fmgr-198-check-docs.py`: passes; verifies every relative
  local link and pinned source path/line in these two documents and the N4
  section. The temporary checker is not a repository change.
- `git diff --cached --check`: passes; zero whitespace errors.
- `git diff --cached --name-only`: exactly this handoff and the ADR.

Local C++/npm builds and tests are waived by the #198 assignment for this
documentation-only slice and were not run. No sanitizer result is claimed.
Applicable documentation-only CI is reported in the PR; C++ matrix/clang-tidy
skips must not be described as implementation coverage.

## Next owner

Lead reviews and merges this design. #194 starts only after this design and #192
merge, writes failing regressions first, implements the signatures and runs its
full dev/applicable sanitizer/CI checks. #182 consumes the terminal SSE contract.
No lock is held; no README, TODO, code, schema, proto, dependency or CI edits were
made. #148 parts 2/3, #179 and broader N4 work remain separate.
