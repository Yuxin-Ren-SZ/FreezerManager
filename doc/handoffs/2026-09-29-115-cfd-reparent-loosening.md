# Handoff note — 2026-09-29, re-parenting a custom-field definition (#115, worker-1)

TODO ID: none — filed by the lead from worker-1's own #103 review. PRD §4.3, N5.

#103 put the tighten-not-loosen rule in the server, evaluated against the
inheritance of the node a write is attached to. But the attachment is itself a
field of the write: `UpdateCfdRequest` carries `item_type_id`, so an override
could be *moved* out of the ancestor chain that bounded it, the check then ran
against the destination's unrelated inheritance, and the subtree the row left
behind fell back to the weaker definition it had been shadowing. Both ends of a
move are now checked. Branch `fix/115-cfd-reparent-loosening`, **stacked on
`fix/103-tighten-server-side`** because #103 (PR **#114**) was not yet on `main`
when this branch was cut; the PR targets that branch and is retargeted to `main`
when #114 merges.

**Changed:**

- `src/server/ItemTypeServiceImpl.cc` — `reject_loosening_abandoned_subtree`,
  called from `UpdateCustomFieldDefinition` with the row as it was stored and the
  row as the request replaces it. It returns immediately when the attachment is
  unchanged, so nothing on the existing no-move path changes. The source subtree
  is the old node and everything under it, which after the move all resolve what
  that node inherits once its own row is gone:
  `storage::resolve_inherited_custom_field_defs` on the old node — the same
  function the destination check uses, so the two ends cannot disagree about the
  ranking — compared against the stored row with `core::tighten_violations`, or
  with `core::removal_violations` when nothing defines the key above it.
- `src/core/custom_field_tightening.h` — `removal_violations(cfd)`, the ways
  taking a definition away loosens a subtree. It is `tighten_violations` against
  the most permissive definition of the same key (optional, non-PHI, no declared
  constraints), so a removal names the constraint it drops in the same vocabulary
  as every other loosening instead of inventing a second table that could drift.
- Tests: 5 in `tests/integration/item_type_service_integration_test.cpp` (two
  must-stay-allowed), 4 in `tests/unit/custom_field_tightening_test.cpp`, plus
  two fixtures — `create_sample_of_type`, which asks the server's resolver what a
  subtree resolves by creating a sample, and `stored_cfd_anywhere`, because
  `stored_cfd`'s empty `item_type_id` means "no filter" rather than "lab-global"
  and so cannot tell a global that stayed global from one that was narrowed.

**Decisions:**

- **Refuse the loosening move rather than make moving inexpressible.** The
  alternative — a separate RPC with its own rules — is not actually a smaller
  hole: `UpdateCfdRequest` still carries `item_type_id`, so "inexpressible" can
  only mean *refusing* a changed attachment (or ignoring it, which silently
  drops a field a client sent), and it needs `lock:proto` for the new RPC. It
  also makes a legitimate reorganization impossible: a move that leaves the old
  subtree exactly as constrained as it was is allowed here and has a test.
- **The check is on both ends, and the source end is the one that needed the
  test to assert *resolution*.** A destination-only check passes this move
  whenever the two subtrees inherit the same definition, which is the common
  case: the row is a no-op at the destination and only the source loses. So the
  integration tests assert what the old subtree resolves, through `CreateSample`,
  which runs the server's own resolver — not through a second copy of the
  ranking inside the test.
- **A constrained lab-global may not be narrowed onto one item type.** Its source
  subtree is every type in the lab, since every type without a definition of its
  own resolves it; narrowing it drops the field for all the others. Establishing
  that none of them relied on it would mean sweeping every lineage in the lab on
  a write path, so the check refuses instead, and the supported way to narrow is
  to leave the global and add a tighter definition at the type — which the rule
  already allows. The refusal is therefore conservative in one edge case: a lab
  with a single item type cannot narrow a constrained global even though nothing
  would be lost.
- **What the rule is *not* about, stated as tests.** A move that leaves the old
  subtree inheriting an identical definition is allowed, and a lab-global that
  constrains nothing may still be narrowed. The rule is about which values are
  refused, not about which node carries a field — the same line #103 draws for
  `indexed`. Without these two tests the fix could have gone green by refusing
  every re-parent.

**Tests:**

- Red before the fix: `ctest --preset dev -R 'ItemTypeService'` → **3 failed of
  48, exit 8** (`UpdateCfdRejectsMovingAnOverrideOutOfTheSubtreeItConstrains`,
  `UpdateCfdRejectsMovingTheOnlyDefinitionOutOfASubtree`,
  `UpdateCfdRejectsNarrowingAConstrainedLabGlobalOntoOneItemType`). The failing
  assertion is the hazard itself, not a proxy for it: the update returns OK, and
  `create_sample_of_type(blood, "{}")` then **succeeds** — the old subtree
  accepts a sample missing the field it required. The two must-stay-allowed tests
  passed in that run, so the suite could not go green by refusing everything.
- After the fix: `ctest --preset dev -R 'ItemTypeService|CustomField'` →
  **178/178, exit 0**.
- `ctest --preset dev` → **1563/1563, exit 0**, 155.95 s. **444 tests skipped**
  (the Postgres legs: `FMGR_TEST_POSTGRES_URL` is unset locally; CI sets it).
  The machine was shared with other agents during this run — no failures, so no
  flake question arose.
- `clang-format --dry-run --Werror` (17.0.6) → exit 0 repo-wide (351 tracked
  files), after `clang-format -i` on the four changed files.
- `clang-tidy -p out/build/dev` (17.0.1), per changed TU →
  `src/server/ItemTypeServiceImpl.cc`, `tests/unit/custom_field_tightening_test.cpp`,
  `tests/integration/item_type_service_integration_test.cpp`: no finding on any
  line this branch changed. `bugprone-easily-swappable-parameters` does not fire
  on this machine at all, so the new two-definition-parameter helper carries the
  repo's `// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)` convention
  with a reason, the way `Totp.cc` and `KmsFactory.cc` do.
- `tools/check-spdx-headers.sh` → exit 0.

**Known limitations / follow-ups:**

- **Renaming a key at the same node is the adjacent operation, and it is
  deliberately not covered.** `UpdateCfdRequest` replaces `key` too, so a row that
  tightened an inherited definition can be renamed where it stands: the inherited
  definition takes over again and the tightening is gone from that subtree — the
  same loss as a move, reached without changing the attachment. **Confirmed with
  a throwaway test on this branch, which was deleted after it showed the hole** —
  it belongs to the follow-up rather than here: with a lab-global `notes`
  (`required`, `max_length: 100`) tightened at `blood` to `max_length: 5`,
  renaming the override *at `blood`* to `notes_v2` keeping every constraint is
  accepted, and a 50-character `notes` value that was refused before is then
  accepted — `blood` falls back to the inherited 100. The check does not run
  because the attachment did not change. It is a different question rather than the same one — the write
  also re-labels the field, and refusing would block fixing a typo in a key — so
  it needs its own decision instead of a widened condition here. Not filed; the
  lead should decide.
- **A lab-global cannot be narrowed while it constrains anything**, including the
  single-item-type lab where nothing would be lost (see Decisions). A precise
  version needs to know whether any type outside the destination's subtree
  resolves the key, which is a lineage sweep of the lab.
- **#116 interacts with this check.** Two definitions of one key on the same node
  are already non-deterministic in the resolver; for a move, `left_behind` is
  what the old node *inherits*, so a same-key sibling row at the old node is not
  considered and the refusal can be conservative there. #116 owns that.
- **Archiving is unchanged.** `ArchiveCustomFieldDefinition` removes a definition
  from its subtree by design and is not checked against the rule, before or after
  this change.
