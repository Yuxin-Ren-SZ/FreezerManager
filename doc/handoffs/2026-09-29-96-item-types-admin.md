# Handoff note — 2026-09-29, G3.9 item types and custom fields admin (#96, worker-2)

TODO.md **G3.9** (PRD §4.3, N5): the admin screen behind
`/labs/:labId/admin/item-types` where a lab defines what a sample *is* — the
item-type tree and the custom-field definitions each node owns and inherits. It
replaces the G1.3 placeholder in `src/features/item-types/`. No C++, proto,
CMake, workflow or lockfile file is touched. Branch `feat/96-item-types-admin`,
PR **#97**.

**Changed:**

- `src/web/src/features/item-types/itemTypeModel.ts` — the rules as values,
  pure and React-free (`layoutModel.ts` is the precedent): `buildItemTypeTree`
  (forest, depth, cycle and orphan reporting), `lineageOf`,
  `resolveFields`/`resolveInheritedFields` (the server's ranking: node >
  ancestors > lab-wide, one definition per key), `canReparent`,
  `tightenViolations`, `definitionProblems`, `parseValidation` /
  `serializeValidation`.
- `src/web/src/features/item-types/ItemTypesScreen.tsx` — the screen: the two
  panes, the four display states, the permission split, and the one place each
  mutation is called.
- `ItemTypeTree.tsx` — drag-and-drop plus the keyboard "Move…" dialog over one
  `item-type/update` call. `ItemTypeDialogs.tsx` — create/rename and move.
- `FieldList.tsx`, `CustomFieldForm.tsx`, `fieldSummary.ts`,
  `serverErrors.ts`, `useItemTypeAdmin.ts`, `ItemTypesScreen.module.css`,
  `locales/en/itemTypes.json`.
- `src/web/src/test/fakeApi.ts` — `item-type/{create,update}` and
  `custom-field-def/{create,update}` resolvers. The additions are confined to
  the item-type/custom-field area and appended after `item-type/get`.
- `src/web/src/test/session.ts` — `currentUserWith(…, { isPhiEnabled })`.
- `src/web/src/app/App.test.tsx` — the `item-types` row of `EXPECTED_SCREEN`
  flipped from placeholder to implemented. `src/app/route-map.tsx` is
  **untouched**: G1.3 already registered the route, the nav entry, the
  `item_type.define` gate and the lazy import, so there was nothing to
  serialize with G3.6 on that file.

**Decisions:**

- **The client guard and the server guard are both there, and they are not
  redundant.** `canReparent` refuses a drop on the node or into its own subtree
  (the drag mistake), walking *up* from the target with a `seen` set, so it
  terminates on data that already contains a cycle. The server refuses the write
  anyway (`ItemTypeRepositories.cc::check_no_cycle`, already covered by
  `ItemTypeRepositoryTest.ItemTypeRejectsCycle` / `ItemTypeRejectsSelfParent` in
  both backends), which is the half a *stale* client tree needs: the screen test
  re-parents `it-tissue` behind the running screen's back, drops `it-serum` onto
  it, and asserts the refusal is explained and the tree reloaded. **A guard
  nobody has watched fail is not a guard**, so the model test also runs the
  cyclic input through an unguarded copy of the walk with a 10,000-step budget
  and watches it burn all of it, while the visited-set version returns in two
  steps. There is no depth limit anywhere: a 1,000-deep chain resolves in full.
- **"Tighten, not drop" is enforced in the form, not by the server.** The
  resolver picks the most-derived definition per key and does not validate that
  the more-derived one is narrower, so the only place the rule can live today is
  the client (`tightenViolations`): required cannot go true→false, a range,
  `max_length` or enum set cannot widen or disappear, the data type cannot
  change, and PHI cannot be unmarked. The permissive direction is tested as
  hard as the refusal — optional→required, a narrowed `max_length` (the seeded
  `notes` on Serum over Blood), enum subsets — because that is the direction a
  careless "must match" rule breaks. This is a client guard on a server rule
  that does not exist yet; if a later slice moves it into `ItemTypeServiceImpl`,
  this form becomes the early, friendlier copy and the tests still hold.
- **`is_phi` + `indexed` is refused twice, with the reason.** The form refuses
  it with a sentence that says *why* (an index stores the value outside the
  encryption layer, L10); the server refuses it too
  (`reject_indexed_phi`), and the screen explains that refusal as well, because
  the two can disagree when the form was opened before a change.
- **`is_phi` is offered only in a PHI lab, and that gate is the client's.** The
  server does not consult `Lab.is_phi_enabled` when a *definition* is created —
  it consults it when PHI *values* are written
  (`SampleServiceImpl.cc::prepare_custom_fields`) — so a definition that
  arrives with `is_phi` in a normal-mode lab would be a field whose values are
  refused later. Refusing it at the definition is the honest place, and
  `definitionProblems` says so; the fake deliberately does **not** mirror a lab
  check the server does not perform.
- **Two permissions, and the read is a third.** The route stays
  `item_type.define` (#70 moved only the *read* paths to `sample.read`, and the
  define permissions belong on this screen). `custom_field.define` gates the
  field catalogue separately, and `sample.read` gates the catalogue *read*, so
  the field pane has a "cannot read" state rather than an empty lie. A role
  holding `item_type.define` without `sample.read` reaches the tree and nothing
  else — no default role splits them, but custom roles can; recorded on the
  board as an input to G3.10.
- **The fake had to grow the write RPCs, and that is the point.** Until this
  slice, `item-type/{create,update}` and `custom-field-def/{create,update}` had
  no resolver, so they answered with the response message's default values: a
  test that posted a parent cycle would have *passed*. The resolvers mirror
  `check_no_cycle`, both partial unique indexes and `reject_indexed_phi`,
  message for message; without them the acceptance criterion "a cycle rejected
  by the server" would have been untestable while looking tested.
- **Enum values are edited comma-separated.** One `TextField` rather than a
  dynamic list: a value containing a comma cannot be expressed, which is a
  deliberate simplification for this slice (no seeded value has one). A later
  slice that needs it grows a list editor; `serializeValidation` already takes
  an array.
- **`serializeValidation` carries unknown constraints through.** The editor
  knows the keys `custom_field_validator.h` implements; a rule written by a
  newer server that this bundle cannot show is preserved rather than deleted by
  a rename. Constraints that no longer apply to a changed data type are dropped,
  because the validator ignores them and they read as enforced rules.

**Tests:**

- `itemTypeModel.test.ts` — 43 tests: the tree (including the unguarded control
  and a 1,000-deep chain), inheritance, `tightenViolations` both ways,
  `canReparent`, validation parsing/serialising, `definitionProblems`.
- `ItemTypesScreen.test.tsx` — 22 tests: the taxonomy and inheritance display,
  the drag guard, a legal drag, the stale-tree server refusal, the move dialog,
  the required-drop refusal, the two permissive tightenings, the widen refusal,
  L10, PHI gating, create, the duplicate key, the four failure states, axe.
- `serverErrors.test.ts` — 6 tests pinning both refusal messages to the C++
  that throws them.
- Both test files were written red first: `itemTypeModel.test.ts` failed with
  `Failed to resolve import "./itemTypeModel"` (`Test Files 1 failed (1)`,
  `Tests no tests`) and `ItemTypesScreen.test.tsx` failed 19/19 against the
  placeholder, before their implementations existed.
- On the rebased tree (`aa4171f` + this branch):

  ```
  cd src/web
  NODE_ENV=production npm run check   # exit 0 — 44 files, 691 tests passed
  unset NODE_ENV; npm run check       # exit 0 — 44 files, 691 tests passed
  ```

  Both runs build the same artifact (`dist/assets/index-Cr9l3_3o.js`), initial
  JS **162.4 KiB gzipped** of the 250 KiB budget. Lint reports **0 errors** (one
  pre-existing `react-hooks/incompatible-library` warning in `src/ui/Table.tsx`).

**Known limitations / follow-ups:**

- **No archive/delete path.** The RPCs exist (`item-type/archive`,
  `custom-field-def/archive`) but the screen does not call them, so the fake
  does not resolve them either. Item-type archiving needs a policy the issue
  does not set — `soft_delete` does not cascade, so archiving a parent leaves
  its children attached to a row the list no longer returns, and they render as
  orphans at the top level (which the tree reports). A slice that adds archive
  should decide the cascade, and its PR must add the two resolvers with it.
- **Scope is fixed to `SAMPLE`.** The field editor manages sample-scoped
  definitions, which is what `resolve_custom_field_defs` resolves for a sample;
  a box- or freezer-scoped definition attached to an item type is neither shown
  nor editable here, and belongs to the screen that owns that scope.
- **`N5` (the custom-field guide for lab admins) is still open.** The rules this
  slice implements are the ones it should document: `tightenViolations` and the
  L10 refusal are the two a lab admin will not guess.
- The four write RPCs are mirrored in the fake but only `create`/`update` are
  reachable from this screen; the sample-facing screens still read the
  catalogue through `sample.read`.
