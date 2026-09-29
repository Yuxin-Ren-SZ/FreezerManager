# Handoff note — 2026-09-29, [server] entity write failures leak backend schema object names (#123, worker-2)

Issue **#123**, raised by worker-2 while pinning #116: a rejected write answered
the client with the storage engine's own text — `UNIQUE constraint failed: index
'cfd_lab_scope_type_key_unique'` — which names a schema object the caller cannot
see and says nothing about which field collided. Every entity behaved that way,
which made it a decision rather than a patch. Branch
`fix/123-unique-violation-message`.

## The scope question, answered first

The issue framed this as a message-quality problem *and* an
information-disclosure one. They have different answers, and the disclosure half
turned out to be the wider of the two:

- **Disclosure is not limited to uniqueness.** `GrpcErrorTranslation.h` forwarded
  `error.what()` for `UniqueViolation`, `ConstraintViolation`,
  `ForeignKeyViolation` and `SerializationFailure`, and those messages were built
  by `sqlite3_errmsg()` / `pqxx::sql_error::what()`. On SQLite that is a schema
  object name (`item_types.lab_id, item_types.name`, `index
  'cfd_lab_scope_type_key_unique'`, `CHECK constraint failed: scope_kind IN (...)`).
- **On PostgreSQL it is worse than a schema name.** `pqxx::sql_error::what()` is
  `PQresultErrorMessage()` (verified in libpqxx 8.0.1, `.conan/p/*/s/src/src/result.cxx`:
  `status_error()` → `err = PQresultErrorMessage(...)` → `throw sql_error{Err, ...}`,
  and `failure::what()` returns that string verbatim). That text is multi-line and
  its **DETAIL line carries the values that collided** — an email for
  `users_primary_email_lower_unique`, a position label for a sample constraint.
  No PostgreSQL server was available locally (`FMGR_TEST_POSTGRES_URL` unset), so
  this half is read from the driver source, not observed.
- **Message quality is per-entity and cannot be centralised.** A generic mapper
  sees `UNIQUE constraint failed: index 'cfd_lab_scope_type_key_unique'` — an
  engine token, not a field. Mapping index → field needs each entity to supply a
  table, and even then the sentence cannot say `'mrn'`: the value is in the
  request, which only the service layer holds. So the honest split is a central
  *guarantee* plus per-entity *sentences*.

## What changed

**Changed:**

- `src/storage/IStorageBackend.h` — new `storage::BackendText` wrapper, and
  `BackendError` now carries two strings: `what()` (client-safe) and `detail()`
  (engine text, for the server log only). `BackendError(code, BackendText)` derives
  `what()` from `default_client_message(code)`; `UniqueViolation(message,
  BackendText)` is the service-side rewrite that keeps the engine text in the log.
- The nine SQLite engine-text helpers (`sqlite/AuditRepositories.cc`,
  `BoxGeometryRepositories.cc`, `IdentityRepositories.cc`, `ItemTypeRepositories.cc`,
  `LayoutRepositories.cc`, `RoleRepositories.cc`, `SampleRepositories.cc`,
  `SessionRepositories.cc`, `ShareRequestRepositories.cc`, plus `SqliteBackend.cc`
  and `SqliteAuthzVersion.h`) now return `BackendText` instead of `std::string`, so
  every call site that passes engine output necessarily takes the safe
  constructor.
- `src/storage/postgres/PostgresRepoSupport.h`, `PostgresBackend.cc` — same for
  `pqxx::sql_error::what()`.
- `src/server/GrpcErrorTranslation.h` — `log_refused_write()` logs the engine text
  at `storage.write_refused` before returning the status. **Only the first line**
  is logged: PostgreSQL's later lines are where the row values are, and PHI never
  goes in a log (AGENTS.md §5).
- `src/server/UniqueConflict.h` (new) — `commit_or_name_conflict(write, message)`
  rewrites a `UniqueViolation` into the handler's own sentence and carries the
  engine detail along. `write` must include the commit: repositories stage and
  flush at commit — except when a composite key is the entity id, where
  `stage_insert` refuses during `insert()`.
- `src/server/ItemTypeServiceImpl.cc` — `CreateItemType`, `UpdateItemType`,
  `CreateCustomFieldDefinition`, `UpdateCustomFieldDefinition`.
- `src/server/LabServiceImpl.cc` — `InviteMember`.

**Entities covered (5 write paths, 3 entities):**

| RPC | message |
|---|---|
| `CreateItemType` / `UpdateItemType` | `an item type named 'blood' already exists in this lab` |
| `CreateCustomFieldDefinition` / `UpdateCustomFieldDefinition` | `custom field 'mrn' is already defined on this item type` (or `on scope 'sample'` when no item type is set) |
| `InviteMember` | `this user is already a member of this lab` |

**Entities not covered, and why:**

- **Everything else, deliberately.** The central half covers *all* of them: no
  engine text reaches a client for any entity any more, and it is now impossible
  to hand engine output to an exception without going through `BackendText`. What
  the uncovered entities get is the class sentence — `a record with these values
  already exists` — which is honest but generic. Naming their field needs the same
  per-entity work done here, and each one needs its own test.
- **`SampleServiceImpl.cc` (position/move conflicts, `samples_position_unique`)** —
  worker-1 held that file's `CheckoutSample` region for #111 in this wave. A
  domain message here is the most valuable of the remainder, because the caller
  picked the position and the box.
- **Box / freezer / container label (`boxes_lab_label_unique`,
  `freezers_lab_name_unique`), role name (`roles_lab_name_unique`), project name
  (`projects_lab_name_unique`), lab name, share requests** — all reachable by
  users, none covered here. Same treatment as above when someone owns them.
- **A second class this issue did not name:** some repositories write their *own*
  refusal text that still names an internal entity — `lab_membership id already
  exists`, `sample_project link already exists`, `checkout_event id already
  exists`, `role id already exists`. Not a schema object name, but not a sentence
  about the caller either. `InviteMember` is fixed because it was in scope; the
  rest are not.

**Decisions:**

- **Do not touch the status code.** `ALREADY_EXISTS` is what clients branch on; only
  the text changed. `tests/unit/error_translation_test.cpp` pins that the code is
  unchanged for both the authored and the engine-text paths.
- **The engine text is logged, not dropped.** It is the only record of *which*
  index refused a row, which is what made the original message useful to a
  developer. `log_refused_write` keeps that, minus the lines that can hold values.
- **The fallback sentence is deliberately vague rather than absent.** An empty or
  status-only message would be the same disclosure fix, but "a record with these
  values already exists" at least tells a caller to change a value; a sentence
  that claimed to know the field would sometimes be wrong.
- **Repository-level mapping was rejected.** Each repository would need a
  constraint-name → field table that duplicates the migrations, differs between
  SQLite and PostgreSQL, and still could not name the value.

**Tests:**

- `tests/integration/item_type_service_integration_test.cpp` —
  `CreateCfdDuplicateKeyReportsTheKey`,
  `CreateCfdDuplicateKeyOnItemTypeReportsTheKey`,
  `UpdateCfdOntoAnExistingKeyReportsTheKey`,
  `CreateItemTypeDuplicateNameReportsTheName`. Each asserts `ALREADY_EXISTS`
  **and the exact message**.
- `tests/integration/lab_service_integration_test.cpp` —
  `InviteMemberDuplicateReportsTheMembership`.
- `tests/unit/error_translation_test.cpp` — `BackendUniqueViolationTextStaysOutOfTheClientMessage`,
  `BackendConstraintViolationTextStaysOutOfTheClientMessage`,
  `BackendForeignKeyViolationTextStaysOutOfTheClientMessage`,
  `CurrentExceptionKeepsEngineTextOutOfTheClientMessage`,
  `ServiceAuthoredUniqueViolationKeepsItsMessage`.
- `tests/backend_conformance/sqlite_backend_conformance_test.cpp` —
  `SqliteCustomFieldUniquenessConformanceTest.DuplicateDefinitionKeepsTheIndexNameOutOfTheMessage`
  (real engine, real repository: `detail()` holds the index name, `what()` does
  not), and the file's local `sqlite_error` helper now mirrors production.
- `tests/backend_conformance/postgres_backend_conformance_test.cpp` —
  `PostgresCustomFieldUniquenessConformanceTest.DuplicateDefinitionKeepsTheEngineTextOutOfTheMessage`.
  **Not run locally**: no `FMGR_TEST_POSTGRES_URL` on the owner's Mac, so it
  compiles here and runs in CI's postgres job.

Red → green: with the tests added, before the fix,
`ItemTypeServiceTest.CreateCfdDuplicateKeyReportsTheKey` failed with
`Which is: "execute sqlite item type statement: UNIQUE constraint failed: index
'cfd_lab_scope_type_key_unique'"`, the item-type test with
`UNIQUE constraint failed: item_types.lab_id, item_types.name`, and the invite
test with `lab_membership id already exists`. After:
`ctest --preset dev` → **1636/1636 passed** (Postgres tests skipped, no server), on
the commit rebased onto `08f9046`.
The log half was confirmed by running the CFD test and grepping the server log for
`storage.write_refused` → `storage refused a write: execute sqlite item type
statement: UNIQUE constraint failed: index 'cfd_lab_scope_type_key_unique'`.

The rebase crossed **#127**, which touched `src/server/GrpcErrorTranslation.h` and
`src/server/LabServiceImpl.cc` — the same two files this change edits — so the
clean textual merge was not treated as evidence: both meeting points were read by
hand (#127's `bearer_token`/`rpc_method_name` split and the `token_and_mfa()`
registration are intact next to `log_refused_write` and the `InviteMember`
wrapper), `#127`'s own fixtures were run **first**
(`ctest --preset dev -R 'ServerIntegrationTest|AuthMiddleware|CredentialRule|RpcRegistry'`
→ 54/54), and only then the full suite above.

**clang-tidy:** not claimed as clean. Local clang-tidy is LLVM 17 against
AppleClang's compile database, so semantic checks go silent and the run is not
evidence either way (see the board's note). `clang-format --dry-run --Werror` on
every changed file and `tools/check-spdx-headers.sh` were run and are clean.

**Known limitations / follow-ups:**

- `src/web/src/test/fakeApi.ts` still answers `item-type/create` and
  `custom-field-def/create` duplicates with the old engine text
  (`src/web/src/test/fakeApi.ts:1156`, `:1211`). No web test fails — nothing
  classifies on those strings — but the fake now models a server that no longer
  exists. Web is outside this issue's scope; whoever owns the web area should
  update it.
- The `detail()` PHI argument rests on PostgreSQL's `DETAIL` line carrying row
  values; on SQLite the text is schema-only. The first-line-only log rule is the
  guard, not a filter that recognises values.
- `commit_or_name_conflict` catches by exception type, so a *different* unique
  constraint failing inside the same write would be described with that handler's
  sentence. Today that means an audit-row `this_hash` collision would be reported
  as a duplicate item type / custom field / membership. Distinguishing them would
  mean parsing engine text to tell constraints apart, which is the fragility this
  change exists to remove; the status code is right either way.
- `commit_or_name_conflict` must wrap the whole write, not just the commit: a
  repository whose entity id is a composite key refuses during `insert()`
  (`LabMembership`), the others flush at `commit()` (custom fields, item types).
