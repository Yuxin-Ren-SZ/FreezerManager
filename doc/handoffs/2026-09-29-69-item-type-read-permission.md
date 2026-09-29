# Handoff note — 2026-09-29, [rbac] Member cannot read item types or custom field definitions (#69, worker-3)

A lab `Member` could not read the item-type taxonomy or the custom-field catalog
that a generated sample form is built from. `ListItemTypes`, `GetItemType` and
`ListCustomFieldDefinitions` required `item_type.define` / `custom_field.define`,
which a Member does not hold (`src/core/permissions.h:242-246`), so the generated
form rendered with no custom fields and no explanation — the screens degrade
gracefully, which is why it read as a missing feature rather than a `403`.
Pre-existing, found by worker-1 while building G3.3 (#59). Same class as #54, one
module over. The server was the enforcement point that was wrong; no proto, route
or REST change was needed. PRD §3.

## Anchors, verified before any code changed

The issue cited `ItemTypeServiceImpl.cc:184`, `:185` and `:190-191`. All three
registry anchors were **correct** — worth stating because the lead's anchors were
wrong on two of three RPCs in #54, so the check was not a formality. Each
registry entry also agreed with the method's own enforcement point:

| RPC | registry | enforcement |
|---|---|---|
| `ListItemTypes` | `:184` `ItemTypeDefine` | `:210` `middleware_.authorize(ItemTypeDefine)` |
| `GetItemType` | `:185` `ItemTypeDefine` | `:251` `has_for_lab(ItemTypeDefine)` |
| `ListCustomFieldDefinitions` | `:190-191` `CustomFieldDefine` | `:361` `middleware_.authorize(CustomFieldDefine)` |

`GetItemType` is the odd one of the three: it never calls
`middleware_.authorize` — it loads the row, then checks the owning lab, so its
enforcement point is a `has_for_lab` check. Editing it by line number alone would
have been easy to get wrong.

## Changed

`src/server/ItemTypeServiceImpl.cc` — six semantic changes plus comments:

- `ListItemTypes` (`:217`), `GetItemType` (`:260`) and
  `ListCustomFieldDefinitions` (`:373`) now require `core::Permission::SampleRead`.
- The three matching `register_rpc` entries (`:190`, `:191`, `:196`) move to
  `P::SampleRead` in the same commit, so the gate and the registry cannot
  disagree.
- The six mutating RPCs — create/update/archive for both resources — keep
  `*.define` in both places. **This diff changes no mutating path's permission.**

`src/server/ItemTypeServiceImpl.h` — the class comment described the old
one-permission-per-resource split; rewritten to state the read/write split.

`tests/integration/item_type_service_integration_test.cpp`,
`tests/integration/rest_gateway_integration_test.cpp` — see below.

## Decisions

1. **`sample.read`, not a new `sample_schema.read`, and not `sample.write`.**
   The evidence is the consumers, and they agree: sample **detail** renders the
   same definitions and its SPA route is already gated `sample.read`
   (`src/web/src/app/route-map.tsx`), so `sample.read` is the minimal permission
   that covers every principal who can already see a sample. `ReadOnly` holds
   `SampleRead` but not `SampleWrite`, so `sample.write` would leave the
   read-only case broken. A new permission would add a catalog entry and a
   role-table change for no reachability gain, and there is no `item_type`
   *action* the PRD names for a read — permissions are `(action, entity)` pairs
   and `item_type.define` is the define action. This is the same reasoning as
   #54; the two services now answer the same question the same way.
2. **The negative direction is asserted, not assumed.**
   `MemberCanReadItemTypeCatalogButStillCannotDefine` has one Member read all
   three RPCs (expecting real rows) and then attempt all six mutating paths
   (expecting `PERMISSION_DENIED`) in the same test, so a careless "relax
   everything in this file" change fails it. `CreateItemTypeAsMemberReturns403`
   is the same boundary at the REST layer. Every pre-existing Member-denied test
   for create/update/archive of both resources stays green as the regression net.
3. **Lab scoping is pinned too.** `ListItemTypesRejectsOutsiderCrossLab` is new;
   `GetItemTypeRejectsOutsiderCrossLab` and `ListCfdsRejectsOutsiderCrossLab`
   already existed and stay unchanged and green. A relaxation to `sample.read`
   must not become "readable by any authenticated principal".
4. **Registry agreement is pinned per RPC by test, not claimed.**
   `RegistryGatesCatalogReadsOnSampleRead` and `RegistryKeepsDefineOnCatalogWrites`
   assert the `AuthMiddleware` registry value for all nine RPCs in this service.
   `RpcRegistryCoversAllExpectedMethods` asserts only a count floor
   (`EXPECT_GE(registry.size(), 60U)`), so it cannot see a permission mismatch —
   AGENTS.md §5 says so, and #54's note is what got that written down. #60
   (worker-2, in flight) is the task that makes the pairing structural; these two
   tests are the local pin until then. The behavioural half is the positive tests:
   if a method's `authorize()` still demanded `*.define`, `ListItemTypesAllowsMember`
   would be red.

## Tests

Red first, before any server change (commit `8091522`):

```
$ ctest --preset dev -R 'ItemTypeService' --output-on-failure
81% tests passed, 7 tests failed out of 36
  ListItemTypesAllowsMember / ListItemTypesAllowsReadOnly / GetItemTypeAllowsMember /
  ListCfdsAllowsMember / ListCfdsAllowsReadOnly / MemberCanReadItemTypeCatalogButStillCannotDefine
      -> "caller lacks required permission for target lab"
  RegistryGatesCatalogReadsOnSampleRead
      -> registry has item_type.define (0x08) / custom_field.define (0x07)
         where sample.read (0x00) is expected
```

After the fix:

- `ctest --preset dev -R 'ItemTypeService'` → **36/36 passed**.
- `ctest --preset dev -R 'RestGatewayTest'` → **31/31 passed**.
- `ctest --preset dev -R 'ItemType|CustomField|Rbac|Permission'` → **223/223 passed**.
- `ctest --preset dev` (full suite, on the final rebase onto `origin/main` at
  `cd7c383`) → **100% tests passed out of 1451**, 146.31 s, exit 0. 21
  Postgres-gated tests skipped locally (`FMGR_TEST_POSTGRES_URL` unset); CI runs
  them against a `postgres:16` service. No test was skipped or excluded by this
  PR. (An earlier full run on the first rebase, `44b66c3`, was also 1451/1451.)
- `clang-format --dry-run --Werror` (17.0.6) over every tracked C/C++/proto file
  → exit 0, no output.
- `tools/check-spdx-headers.sh` → exit 0.

**The local clang-tidy gate could not be run end-to-end on this machine**
(pre-existing, environmental — flagging it because the board says it is
reproducible and this worker could not reproduce it). `run-clang-tidy-17` is not
installed in the shared `.venv`; only `clang-tidy` 17.0.1 is. Pointed at a TU
with `-p out/build/dev`, 17.0.1 cannot parse this Mac's SDK libc++ (LLVM 22,
which uses `__builtin_clzg`/`__builtin_ctzg`) and stops at
`protobuf/arena_align.h` (`has_single_bit`). Shimming the two builtins gets past
the SDK and the run then reports only pre-existing findings in code this PR does
not touch: `-to-static` on the fixture methods `login`/`create_item_type`/
`create_cfd`/`seed` (which do use members — an artifact of the incomplete
analysis, and main's green CI passes them) and in `src/obs/Health.h` /
`src/rpc/RateLimiter.h`. **No diagnostic points at a line this PR adds or
changes.** Per the board, local output is a superset of CI's and CI is the
authority for this gate; the PR says the same rather than claiming a local pass.

**The full suite caught a second pin of the bug.** `RestGatewayTest.ListItemTypesAsMemberReturns403`
already existed and asserted the wrong behaviour at the REST layer; the first
full run after the server fix was `99% tests passed, 1 tests failed out of 1444`
with that test as the only failure. It is now
`ListItemTypesAsMemberReturns200`, which creates a row as the admin and asserts
the Member lists it through JSON → proto → handler → repo → JSON, plus
`ListCustomFieldDefinitionsAsMemberReturns200`. Both were verified in both
directions: with the server gates reverted to the pre-fix revision they fail
(`403` where `200` is expected) while `CreateItemTypeAsMemberReturns403` stays
green, and with the fix they pass.

## Known limitations / follow-ups

1. **The SPA still hides the custom fields it can now fetch — end-to-end the bug
   is only half fixed.** `src/web/src/features/samples/SampleBrowserScreen.tsx:95-96`
   gates the query client-side: `const canDefineFields = useCan('custom_field.define', labId);`
   then `useCustomFieldDefinitions(labId, { enabled: canDefineFields })`. A Member
   therefore still gets no custom-field columns, and the same gate is what a
   generated form copy-pastes. `src/web/src/test/fakeApi.ts:76-79`,
   `sampleColumns.test.tsx:43-47` and `SampleBrowserScreen.test.tsx:41` document
   the old server contract in their comments, so a web task has to change the
   gate, the comments and those expectations together. **Not done here:** the
   issue scopes the SPA out ("the screens already handle the refusal; the server
   is the point that is wrong") and `src/web/` was off-limits to this worker. The
   lead should file it; the server half is done and tested.
2. **`GetItemType` loads the row before the lab check** (pre-existing, unchanged).
   An authenticated principal in another lab can therefore tell an existing item
   type id (`PERMISSION_DENIED`) from a missing one (`NOT_FOUND`) — a small
   existence oracle. It is not made worse by this change (the check now needs
   `sample.read` instead of `item_type.define`), it was left alone as out of
   scope, and it is worth a `NOT_FOUND`-first rewrite in whichever task next
   touches this method.
3. **No sanitizer run.** A permission-gate change touching no memory,
   concurrency, storage or parser code, so per AGENTS.md §4 `asan`/`tsan` are not
   required and were not run.
4. `Member`'s grant set did not change; the gates did. That is the intended
   consequence and it is asserted in both directions rather than implied.
