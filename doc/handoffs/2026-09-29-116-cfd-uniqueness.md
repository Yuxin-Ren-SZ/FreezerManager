# Handoff note — 2026-09-29, same-rank custom-field keys resolve by iteration order (#116, worker-2)

TODO ID: none — found by worker-1 while implementing #103. PRD §4.3, N5.

`resolve_custom_field_defs` ranks candidates and keeps the most specific, but two
definitions of one key *at the same rank* are never compared: the `>=` tie-break
keeps whichever row iterates last. Two lab-globals, or two rows on one node, would
therefore make a `required` or `is_phi` flag depend on storage iteration order — a
flag that could differ between backends or after a vacuum.

**Answer: already prevented, and the constraint predates every deployment.**
Migration 7 creates the table and the index together, identically on both
backends:

```sql
CREATE UNIQUE INDEX IF NOT EXISTS cfd_lab_scope_type_key_unique
  ON custom_field_definitions(lab_id, scope_kind, COALESCE(item_type_id, ''), key)
  WHERE archived_at_micros IS NULL;
```

**No migration was added and `lock:migration` was not taken.** What the issue
asked for that did not exist is a test that holds the rule in place, so that is
what the branch carries: `fix/116-cfd-uniqueness`, PR **#122**.

**Changed:**

- `tests/backend_conformance/sqlite_backend_conformance_test.cpp` and
  `tests/backend_conformance/postgres_backend_conformance_test.cpp` —
  `Sqlite/PostgresCustomFieldUniquenessConformanceTest`, the same three
  assertions in both files, and the first thing in either file to run on the
  **domain** schema (default migrations, real repositories, real resolver)
  instead of the reduced conformance one. A second same-rank definition is
  refused with the portable `UniqueViolation` in *either* insertion order;
  exactly one live row survives; `resolve_custom_field_defs` returns that row.
  Covered: the one-node pair (the duplicate is the tighter one in one order and
  the looser one in the other, so #103's tighten-not-loosen check cannot be what
  refuses it), the lab-global pair (the `COALESCE` NULL sentinel — without it
  both engines would let two globals through, since NULLs compare distinct in a
  unique index), and the archived-row exemption that keeps "archive and
  redefine" legal.
- `tests/integration/item_type_service_integration_test.cpp` — the paths a client
  actually reaches: create on one node in both orders, create lab-global, and
  `UpdateCfdRefusesMovingADefinitionOntoAnExistingKey`. Each pins
  `ALREADY_EXISTS` (not a masked `INTERNAL`) and the survivor; the update one
  pins that the refusal is atomic.
- `tests/unit/item_type_repository_test.cpp` — the existing
  `CustomFieldDefinitionUniqueKeyPerLabScopeType` covered only the lab-global
  case; it now covers the one-node case too, which its name already promised.
- `src/storage/CustomFieldResolver.h` — **comment only.** The `>=` tie branch is
  stated as unreachable, the index is named as the rule rather than the
  comparison, and what would happen if the index were ever dropped (the tie
  returns, non-deterministically) is written where the tie lives.

**Decisions:**

- **Schema, not the resolver, and the schema already had it.** `COALESCE` and the
  partial `WHERE` are both load-bearing, and both migration runners apply a
  migration inside one transaction, so a deployment cannot hold the table
  without the index. Adding a redundant constraint — plus a dedupe step for data
  that cannot exist — would have been risk for no behaviour change. This is also
  why there is no "what to do about existing duplicates" answer to write: the
  index shipped with the table, so none can exist; a hand-modified database
  would fail migration 7 loudly rather than silently pick a winner.
- **Why not a resolver tie-break rule:** it would be a rule that cannot fire, and
  the fix for "two rows we cannot tell apart" is to keep them from existing
  rather than to rank them. It would also have to be mirrored in
  `src/web/src/features/item-types/itemTypeModel.ts`, which deliberately mirrors
  "last row wins" today; changing the server half alone would create exactly the
  divergence that mirror exists to prevent, and the web is outside this issue. If
  the index is ever removed on purpose, the conformance test goes red and that is
  the moment to argue about a rule, with a reason.
- **Why not the service layer:** `freezerctl custom-field-def import` inserts
  through the repository directly (`run_entity_import`), bypassing
  `ItemTypeServiceImpl` entirely — a service-level check would not have covered
  every writer, and the index already does.

**Tests:**

- `cmake --build --preset dev` → exit 0.
- `ctest --preset dev -R 'ItemTypeService|CustomField|Conformance|ItemTypeRepository'`
  → **232/232, exit 0**, 426.23 s.
- `ctest --preset dev` → **see the HANDOFF comment on #116**; re-run after the
  rebase onto `origin/main`.
- `clang-format --dry-run --Werror` (17.0.6) on the changed files → exit 0.
- `tools/check-spdx-headers.sh` → exit 0 (no new source files).
- Postgres legs → **skipped locally**: no `FMGR_TEST_POSTGRES_URL` and no
  PostgreSQL/container runtime on this machine. They run in CI only.

**Do not read a green run as "the test was not needed".** The tests pass on the
unfixed tree because the invariant holds. They were proven able to fail by
deleting the `CREATE UNIQUE INDEX` line from SQLite migration 7 and re-running:
2 of 3 red (`Actual: it throws nothing`, two live rows), and with a temporary
probe the resolver's answer followed insertion order (alpha-then-beta resolved to
**beta**; beta-then-alpha resolved to **alpha**). Both temporary edits were
reverted before the commit.

**Known limitations / follow-ups:**

- `tests/integration/item_type_service_integration_test.cpp` is **appended to at
  the end of the file**, and worker-1 is editing the same file for #115, so a
  rebase conflict is possible there even though the regions differ.
- A refused duplicate surfaces the backend's own message (`UNIQUE constraint
  failed: index 'cfd_lab_scope_type_key_unique'` / the pqxx equivalent) inside an
  `ALREADY_EXISTS`. That is the convention every entity in this repo follows, so
  it is consistent — but it does name a schema object to the client, and a
  friendlier `"custom field 'x' is already defined on this item type"` would be
  the thing to add if the lead wants it. Not in this issue's scope.
- `itemTypeModel.ts`'s equal-rank branch is dead code for the same reason the
  server's is (the schema forbids the pair). Left alone: web scope, and no
  behaviour depends on it.
