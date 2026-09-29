# Handoff note — 2026-09-29, N5 tighten-not-loosen enforced server-side (#103, worker-1)

TODO ID: none — found by worker-2 while building G3.9 (#96). PRD §4.3, N5.

G3.9's form refuses a custom-field definition that loosens the one it inherits,
but the rule lived only there: the definition resolver picks the most-derived
definition per key and never checked that it is narrower, so `freezerctl`, the Qt
client or anything speaking REST/gRPC could store a loosening override — a lab
whose sample data can lack a field the parent type requires. Both custom-field
write RPCs now refuse that with `INVALID_ARGUMENT` naming the constraint, and the
browser keeps its copy of the rule for fast feedback. Branch
`fix/103-tighten-server-side`, PR **#114**.

**Changed:**

- `src/core/custom_field_tightening.h` **(new)** — `tighten_violations(parent,
  child)`, the rule as a pure function: refused are `required` dropped,
  `data_type` and `scope_kind` changed, `is_phi` dropped, `max_length` raised or
  dropped, `min`/`max` widened or dropped, enum `values` grown or dropped;
  allowed is everything that tightens or stays equal (optional → required,
  `max_length` 20 → 5 *and* 20 → 20, an enum subset, a narrower range, a brand-new
  key with nothing above it). `indexed` is deliberately not part of the rule: an
  index is a lookup structure, not a constraint on values, and L10 forces it off
  when a field becomes PHI, so "index removed" would make the PHI rule
  unsatisfiable. `DeclaredConstraints` reads the same JSON the validator does.
- `src/storage/CustomFieldResolver.h` — the lineage walk and the best-per-key
  loop moved into shared `detail::` helpers, and
  `resolve_inherited_custom_field_defs` is the same ranking with the node itself
  left out: exactly what a write at that node shadows.
  `resolve_custom_field_defs` keeps its behaviour and its tests.
- `src/server/ItemTypeServiceImpl.cc` — `reject_loosening_inherited_definition`,
  called from `CreateCustomFieldDefinition` and `UpdateCustomFieldDefinition`
  before the row is staged; the refusal rolls the transaction back.
- `src/web/src/features/item-types/itemTypeModel.ts` — comment only: the server
  enforces `tightenViolations` too and is the authoritative half.
- Tests: 7 in `tests/integration/item_type_service_integration_test.cpp` (plus a
  `CfdWriteSpec` helper so a test can assert a *refusal*, and `stored_cfd` to
  prove a refused write left nothing behind), 13 in the new
  `tests/unit/custom_field_tightening_test.cpp`, 3 in
  `tests/unit/custom_field_resolver_test.cpp`; `tests/unit/CMakeLists.txt`
  registers the new file.

**Decisions:**

- **The comparison lives in `src/core/`, the resolution stays in storage.** The
  rule is a property of the definitions themselves — two values in, violations
  out, no I/O — which is `src/core/`'s contract (AGENTS.md §5), and it sits
  beside `custom_field_validator.h` because that header owns the constraint
  vocabulary it compares. *Which* definition a write shadows is not pure: it
  needs the ancestor chain, so it belongs with the resolver that already owns
  that ranking, as `resolve_inherited_custom_field_defs` rather than a second
  copy of the walk. Putting the comparison in storage would have made a domain
  rule depend on a backend header; putting the resolution in the service would
  have duplicated the ranking that must not drift from it.
- **Both write paths, not just `Create`.** `Update` replaces the whole
  definition (the proto has no field mask, attachment included), so it can turn
  a legal override into a loosening one; the same helper guards both.
- **Equality is a tightening.** `max_length` 20 → 20 is allowed. A first draft of
  the update test asserted a refusal there and was wrong — the SPA's table is the
  contract, and it refuses only `>`. Worth knowing before "fixing" a future
  report.
- **`scope_kind` is compared even though only sample-scoped definitions
  participate in the ranking.** That is what G3.9's `tightenViolations` does
  (its inherited list is sample-scoped, its comparison is not), and mirroring the
  client's table was the point of the issue rather than inventing a second rule.
- **The SPA keeps its check.** A form that explains the refusal before sending
  beats one that translates a failed request; the server is what a non-browser
  writer cannot avoid. Its comment now says which is which.

**Tests:**

- `ctest --preset dev -R 'ItemTypeService'` → **red: 4 failed out of 43, exit 8**
  before the fix (`CreateCfdRejectsLooseningInheritedDefinition`,
  `UpdateCfdRejectsDroppingRequiredInheritedField`,
  `UpdateCfdRejectsWideningInheritedConstraint`,
  `CreateCfdRejectsDroppingARequiredLabGlobalField`); **green: 43/43, exit 0**
  after. The three tests that must stay allowed passed in both runs, so the suite
  could not have gone green by refusing everything.
- `ctest --preset dev -R 'CustomField|ItemType|Sample'` → **422/422, exit 0**
  (Postgres legs skipped locally, `FMGR_TEST_POSTGRES_URL` unset; CI runs them).
- `ctest --preset dev` → full-suite count in the `HANDOFF` comment on #103.
- `clang-format --dry-run --Werror` (17.0.6) clean on changed files;
  `clang-tidy -p out/build/dev` (17.0.1) per changed TU, filtered by the board's
  rule, reports nothing on changed lines (the one class it reports —
  `performance-unnecessary-value-param` on by-value/moved params — also fires on
  unmodified `tests/unit/custom_field_validator_test.cpp` on `main`, i.e. it is
  this machine's broken libc++ AST); `tools/check-spdx-headers.sh` exits 0.
- `src/web`: `npm run check` was **not** run locally (no `node_modules` in this
  worktree or the main checkout). The only web change is a comment.

**Known limitations / follow-ups:**

- **Re-parenting a definition is a separate vector.** `UpdateCfdRequest` carries
  `item_type_id`, so an override can be moved to a node outside the ancestor chain
  that constrained it; the old node's subtree then falls back to the weaker
  inherited definition. The check compares a write against what its *new* node
  inherits, which is the rule this issue names. The SPA's edit mode never moves a
  definition (its node is fixed), so this is a non-web-writer path only. No issue
  filed — the lead should decide whether it is worth one.
- **Two definitions of one key at the same rank** (two rows on one node, or two
  lab-globals) are not compared with each other: the resolver's `>=` tie-break
  keeps whichever row it iterates last, so a looser sibling can shadow a tighter
  one. Closing that is probably a uniqueness constraint, not another comparison.
