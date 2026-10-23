# Progress reconciliation (#172)

The owner requested implementation of the 20-commit review plan. This docs
slice aligns the roadmap and GitHub records with main at `53cbe15` and the
owner's existing dashboard decisions.

- README now marks the Web UI in progress and names shipped and remaining flows.
- TODO records wave A and MFA limiting as merged, starts the P2 queue at #144,
  and distinguishes the three-block dashboard first cut from deferred charts.
- Board #28 records the separately authorized #169–#172 tranche and includes
  ASan/TSan failures alongside clang-tidy in #168's status and resume steps.
- Issue #133 now follows the owner's later decision: use ListSamples scope/RLS,
  defer the scope-filter plant to #151, retain the 100k/500ms measurement and use
  indexed queries in both backends first. Its reversed G4.2 dependency is fixed.

Evidence: #111/#128/#164 are closed; G0.2/G0.4/G2.1 are completed on main;
#168 remains open with failing ASan, TSan and clang-tidy checks. No hosting or
dashboard feature is marked shipped by these edits. The old wave-B queue stays
paused during the review-fix tranche.

Validation: `git diff --check` passes. Changes were compared with live issue
states, #133's owner DECISION and #168's check results. No C++/web implementation
or dependency changes are part of this PR, so application tests are not required
for this documentation slice. Shared C++ validation for the code-fix tranche is
reported on the implementation PRs rather than claimed here.

Implemented by lead (Codex).
