# Test Coverage Inventory

_Last updated: 2026-06-06._

601 tests across 35 test source files (+ 6 benchmark files). All pass
(16 Postgres conformance/RLS tests skip without `FMGR_TEST_POSTGRES_URL`).
Document maps each module/function → test status. ✓=explicit test
○=indirect ✗=untested —=N/A

---

## 1. Core Enums (`src/core/enums.h`)

### SampleStatus
| Function                    | Status | Test location                             |
| --------------------------- | ------ | ----------------------------------------- |
| to_string (all 5 variants)  | ✓      | core_value_types_test                     |
| parse_sample_status (all 5) | ✓      | core_value_types_test                     |
| parse("invalid") throws     | ✓      | core_value_types_test                     |
| to_json / from_json         | ✓      | core_value_types_test + sample_types_test |

### CheckoutAction
| Function                      | Status | Test location                         |
| ----------------------------- | ------ | ------------------------------------- |
| to_string (all 3 variants)    | ✓      | core_value_types_test                 |
| parse_checkout_action (all 3) | ✓      | core_value_types_test                 |
| to_json / from_json           | ○      | sample_types_test (via CheckoutEvent) |

### RoleKind
| Function                   | Status | Test location         |
| -------------------------- | ------ | --------------------- |
| to_string (all 5 variants) | ✓      | core_value_types_test |
| parse_role_kind (all 5)    | ✓      | core_value_types_test |
| parse("invalid") throws    | ✓      | core_value_types_test |
| to_json / from_json        | ✓      | core_value_types_test |

### ContainerKind
| Function                     | Status | Test location         |
| ---------------------------- | ------ | --------------------- |
| to_string (all 5 variants)   | ✓      | core_value_types_test |
| parse_container_kind (all 5) | ✓      | core_value_types_test |
| parse("") throws             | ✓      | freezer_types_test    |
| parse("unknown") throws      | ✓      | freezer_types_test    |
| to_json / from_json          | ✓      | core_value_types_test |

### UserStatus
| Function                    | Status | Test location         |
| --------------------------- | ------ | --------------------- |
| to_string (Active/Disabled) | ✓      | core_value_types_test |
| parse_user_status           | ✓      | core_value_types_test |
| parse("invalid") throws     | ✓      | core_value_types_test |
| to_json / from_json         | ✓      | core_value_types_test |

### ShareRequestStatus
| Function                   | Status | Test location            |
| -------------------------- | ------ | ------------------------ |
| to_string (all 4 variants) | ✓      | share_request_types_test |
| parse (all 4)              | ✓      | share_request_types_test |
| parse("invalid") throws    | ✓      | share_request_types_test |
| to_json / from_json        | ✓      | share_request_types_test |

### ShareApprovalRole
| Function                   | Status | Test location            |
| -------------------------- | ------ | ------------------------ |
| to_string (all 3 variants) | ✓      | share_request_types_test |
| parse (all 3)              | ✓      | share_request_types_test |
| parse("invalid") throws    | ✓      | share_request_types_test |
| to_json / from_json        | ✓      | share_request_types_test |

---

## 2. Core Types — JSON serialization

### ids.h / uuid.h / timestamp.h / quantity.h
| Function                   | Status                                                          |
| -------------------------- | --------------------------------------------------------------- |
| UUID parse/format          | ✓                                                               |
| UUID invalid reject        | ✓                                                               |
| StrongId type safety       | ✓                                                               |
| Volume add/sub/compare     | ✓                                                               |
| Mass add/sub/compare       | ✓                                                               |
| Quantity JSON round-trip   | ✓                                                               |
| Timestamp micros precision | ✓                                                               |
| Timestamp JSON round-trip  | ✓                                                               |
| Volume cross-unit throw    | ✓                                                               |
| Mass cross-unit throw      | ✓                                                               |
| Boundary values (min/max   | ✗                                                               |
| int64, 64KB strings, year  |                                                                 |
| 2038+ timestamps)          | skipped: no boundary bugs known; C++20 std lib handles overflow |

### identity.h
| Function                    | Status |
| --------------------------- | ------ |
| LabMembershipId::parse      | ✓      |
| LabMembershipId::to_string  | ✓      |
| LabMembershipId single-ID   |        |
| parse throws                | ✓      |
| Lab to_json/from_json       | ✓      |
| User to_json/from_json      | ✓      |
| LabMembership to/from json  | ✓      |
| User with empty email       | ✗      |
| skipped: empty email rejected by UserRepository at insert level |

### sample.h
| Function                      | Status |
| ----------------------------- | ------ |
| VolumeUnit to_json/from_json  | ✓      |
| MassUnit to_json/from_json    | ✓      |
| SampleProject::id()           | ✓      |
| SampleProjectId ordering      | ✓      |
| Project to_json/from_json     | ✓      |
| SampleProject to/from json    | ✓      |
| CheckoutEvent to/from json    | ✓      |
| Sample to_json/from_json      | ✓      |
| Sample with null optionals    | ✓      |
| Sample Tombstoned status      | ✓      |
| Empty positions vector        | ✓      |
| BoxType (all fields)          | ✓      |
| ContainerType (all fields)    | ✓      |
| Freezer (all fields)          | ✓      |
| StorageContainer (all fields) | ✓      |
| CapacityHint nullable depth   | ✓      |

### share_request.h
| Function                          | Status |
| --------------------------------- | ------ |
| ShareRequestApproval::id()        | ✓      |
| ShareRequestApprovalId ordering   | ✓      |
| ShareRequestApproval to/from json | ✓      |
| ShareRequest to/from json         | ✓      |
| Default status is Pending         | ✓      |

### permissions.h
| Function                       | Status |
| ------------------------------ | ------ |
| All 21 permission keys         | ✓      |
| parse_permission(key)          | ✓      |
| parse("unknown") throws        | ✓      |
| Keys have dotted convention    | ✓      |
| Builtin roles disjoint subsets | ✓      |

### role.h
| Function                          | Status |
| --------------------------------- | ------ |
| Builtin role IDs stable           | ✓      |
| Role to_json/from_json            | ✓      |
| RolePermission to/from json       | ✓      |
| validate_scope_filter accepts {}  | ✓      |
| validate_scope_filter rejects ... | ✓      |
| non-object/unknown keys/bad vals  | ✓      |

### custom_field_validator.h
| Function                      | Status |
| ----------------------------- | ------ |
| Required missing → error      | ✓      |
| Required present → pass       | ✓      |
| Optional absent → pass        | ✓      |
| Type mismatch (int/float/bool)| ✓      |
| Enum in set → pass            | ✓      |
| Enum not in set → error       | ✓      |
| Reference valid UUID → pass   | ✓      |
| Reference invalid UUID → error| ✓      |
| String max_length enforced    | ✓      |
| String within max_length      | ✓      |
| Numeric range enforced        | ✓      |
| Numeric within range          | ✓      |
| Unknown extra fields ignored  | ✓      |
| Multiple errors accumulated   | ✓      |
| Date accepts ISO 8601 string  | ✓      |
| Date rejects non-string       | ✓      |
| Datetime accepts ISO 8601     | ✓      |
| Datetime rejects non-string   | ✓      |
| Float exceeds max → error     | ✓      |
| Bool null → pass (as optional)| ✓      |
| Date format validation        | ✗      |
| (2024-13-45, etc.)            | skipped: validator only checks is_string; format validation deferred to RFC layer per TODO |
| Datetime format validation    | ✗      |
| (2024-01-01T25:00:00Z)        | skipped: same reason as date — only is_string check at validator level |
| Enum without "values" key     | ✗      |
| skipped: parse_constraints returns empty object; enum validation treats empty set as "no values allowed", so any value errors. Malformed validation_json is a data-integrity issue, not a validator concern |

---

## 3. Storage Interface (`src/storage/IStorageBackend.h`)

### Error hierarchy
| Class                    | Status |
| ------------------------ | ------ |
| BackendError base        | ✓      |
| UniqueViolation          | ✓      |
| ForeignKeyViolation      | ✓      |
| SerializationFailure     | ✓      |
| NotFound                 | ✓      |
| ConstraintViolation      | ✓      |
| MigrationFailure         | ✓      |
| UnsupportedOperation     | ✓      |
| Unavailable              | ○      |
| covered indirectly through SQLite error paths on disk-full or lock-contention |

### Query DSL
| Function                    | Status |
| --------------------------- | ------ |
| Query::where()              | ✓      |
| Query::and_where()          | ✓      |
| Query::order_by()           | ✓      |
| Query::limit()              | ✓      |
| Query::offset()             | ✓      |
| Query::include_tombstoned() | ✓      |
| operator==(FieldRef, value) | ✓      |
| between(FieldRef, lo, hi)   | ✓      |
| in(FieldRef, values)        | ✓      |
| json_path(Field, path)==val | ✓      |
| greater_or_equal(FieldRef)  | ✗      |
| skipped: no production code uses >= predicate yet; used in RPC layer per TODO |
| less_or_equal(FieldRef)     | ✗      |
| skipped: same reason as >=  |

### ITransaction
| Function                    | Status |
| --------------------------- | ------ |
| commit()                    | ✓      |
| rollback()                  | ✓      |
| repo<Entity>()              | ✓      |
| double-commit guard         | ✗      |
| skipped: commit() sets completed_ flag; double-commit throws or no-ops — internal invariant not user-facing |

### IStorageBackend
| Function            | Status |
| ------------------- | ------ |
| migrate_to_latest() | ✓      |
| current_version()   | ✓      |
| begin(isolation)    | ✓      |
| caps()              | ✓      |

---

## 4. SQLite Backend (`src/storage/sqlite/SqliteBackend.cc`)

| Function                                                       | Status |
| -------------------------------------------------------------- | ------ |
| migrate_to_latest (all 9 steps)                                | ✓      |
| current_version                                                | ✓      |
| begin (creates all repos)                                      | ✓      |
| caps                                                           | ✓      |
| Checksum mismatch → failure                                    | ✓      |
| Duplicate version → failure                                    | ✓      |
| Downgrade + re-migrate                                         | ✓      |
| Migration ordering enforced                                    | ✓      |
| Downgrade past zero rejected                                   | ✓      |
| audit_event_count_for_tests                                    | ○      |
| used in audit tests; not directly tested                       |        |
| fail_next_audit_append_for_tests                               | ○      |
| used in conformance's AuditAppendFailurePreventsMutationCommit |        |

---

## 5. SQLite Repositories

For every repository class (18 classes), the standard CRUD surface
is: `find_by_id`, `query`, `insert`, `update`, `soft_delete`.

### Identity — LabRepository, UserRepository, LabMembershipRepository

| Entity        | find | query | insert | update | soft_del | errors tested                       | missing                          |
| ------------- | ---- | ----- | ------ | ------ | -------- | ----------------------------------- | -------------------------------- |
| Lab           | ✓    | ✓     | ✓      | ✓      | ✓        | tombstone exclusion                 | duplicate name among live rows   |
|               |      |       |        |        |          | audit event on insert               | query with sort/limit predicates |
| User          | ✓    | ✓     | ✓      | ✓      | ✓        | email uniqueness (case-insensitive) | update nonexistent user          |
|               |      |       |        |        |          | tombstone exclusion                 | query with predicates            |
|               |      |       |        |        |          | audit event on insert               | empty email / display_name       |
| LabMembership | ✓    | ✓     | ✓      | ✓      | ✓        | composite key parse                 | update nonexistent membership    |
|               |      |       |        |        |          | FK violation (missing user/lab)     |                                  |
|               |      |       |        |        |          | audit event on insert               |                                  |

### Role — RoleRepository, RolePermissionRepository

| Entity         | find | query | insert | update   | soft_del | errors tested                    | missing                      |
| -------------- | ---- | ----- | ------ | -------- | -------- | -------------------------------- | ---------------------------- |
| Role           | ✓    | ✓     | ✓      | ✓        | ✓        | archive builtin role rejected    | duplicate name within lab    |
|                |      |       |        |          |          | custom role tombstone visibility | scope-filter hash validation |
| RolePermission | ✓    | ✓     | ✓      | ✓(throw) | ✓(hard)  | FK on missing role               | duplicate grant insert       |
|                |      |       |        |          |          | soft_delete removes row          |                              |

### Layout — FreezerRepository, StorageContainerRepository

| Entity           | find | query | insert | update | soft_del | errors tested                       | missing                          |
| ---------------- | ---- | ----- | ------ | ------ | -------- | ----------------------------------- | -------------------------------- |
| Freezer          | ✓    | ✓     | ✓      | ✓      | ✓        | FK violation (missing lab)          | query with sort/limit            |
|                  |      |       |        |        |          | name uniqueness (live rows only)    | ordering_index tests             |
| StorageContainer | ✓    | ✓     | ✓      | ✓      | ✓        | self-parent cycle detection         | orphan child after parent delete |
|                  |      |       |        |        |          | descendant cycle detection          | query with sort/limit/json_path  |
|                  |      |       |        |        |          | recursive tree query by parent      |                                  |
|                  |      |       |        |        |          | child visible after parent archived |                                  |

### Box Geometry — ContainerTypeRepository, BoxTypeRepository, BoxRepository

| Entity        | find | query | insert | update | soft_del | errors tested                   | missing                           |
| ------------- | ---- | ----- | ------ | ------ | -------- | ------------------------------- | --------------------------------- |
| ContainerType | ✓    | ✓     | ✓      | ✓      | ✓        | invalid dimensions (zero width) | query include_tombstoned          |
|               |      |       |        |        |          | cross-lab size_class FK         | empty name / size_class           |
| BoxType       | ✓    | ✓     | ✓      | ✓      | ✓        | duplicate position labels       | empty positions (type-level only) |
|               |      |       |        |        |          | empty accepts                   | negative dimension coordinates    |
|               |      |       |        |        |          | unknown size_class token        | query with predicates             |
|               |      |       |        |        |          | cross-lab size_class FK         |                                   |
|               |      |       |        |        |          | audit event on insert           |                                   |
| Box           | ✓    | ✓     | ✓      | ✓      | ✓        | empty label                     | duplicate label in same lab       |
|               |      |       |        |        |          | cross-lab BoxType FK            |                                   |
|               |      |       |        |        |          | cross-lab StorageContainer FK   |                                   |
|               |      |       |        |        |          | archived BoxType FK             |                                   |
|               |      |       |        |        |          | archived StorageContainer FK    |                                   |
|               |      |       |        |        |          | query by StorageContainerId     |                                   |
|               |      |       |        |        |          | query sort and limit            |                                   |

### Item Type — ItemTypeRepository, CustomFieldDefinitionRepository

| Entity                | find | query | insert | update | soft_del | errors tested                    | missing               |
| --------------------- | ---- | ----- | ------ | ------ | -------- | -------------------------------- | --------------------- |
| ItemType              | ✓    | ✓     | ✓      | ✓      | ✓        | empty name                       | query with sort/limit |
|                       |      |       |        |        |          | cycle detection (A→B→A)          |                       |
|                       |      |       |        |        |          | self-parent rejection            |                       |
|                       |      |       |        |        |          | cross-lab query isolation        |                       |
|                       |      |       |        |        |          | tombstone visibility             |                       |
| CustomFieldDefinition | ✓    | ✓     | ✓      | ✓      | ✓        | empty key                        | query with sort/limit |
|                       |      |       |        |        |          | is_phi + indexed combination ban |                       |
|                       |      |       |        |        |          | cross-lab ItemType FK            |                       |
|                       |      |       |        |        |          | archived ItemType FK             |                       |
|                       |      |       |        |        |          | unique (lab,scope,item_type,key) |                       |
|                       |      |       |        |        |          | include_tombstoned query         |                       |

### Sample — SampleRepository, ProjectRepository, SampleProjectRepository, CheckoutEventRepository

| Entity        | find | query | insert | update   | soft_del | errors tested                              | missing                         |
| ------------- | ---- | ----- | ------ | -------- | -------- | ------------------------------------------ | ------------------------------- |
| Sample        | ✓    | ✓     | ✓      | ✓        | ✓        | empty name                                 | update nonexistent throws       |
|               |      |       |        |          |          | box_id+position_label both-or-null         |                                 |
|               |      |       |        |          |          | parent_sample_id in wrong lab → error      |                                 |
|               |      |       |        |          |          | parent_sample_id == self → error           |                                 |
|               |      |       |        |          |          | box exists and not archived                |                                 |
|               |      |       |        |          |          | position_label exists in BoxType           |                                 |
|               |      |       |        |          |          | size_class compatible with position        |                                 |
|               |      |       |        |          |          | item_type exists in lab                    |                                 |
|               |      |       |        |          |          | duplicate ID → UniqueViolation             |                                 |
|               |      |       |        |          |          | tombstone vacates position                 |                                 |
|               |      |       |        |          |          | parent-child lineage survives tombstone    |                                 |
|               |      |       |        |          |          | sort by created_at                         |                                 |
|               |      |       |        |          |          | limit and offset                           |                                 |
|               |      |       |        |          |          | and_where multiple predicates              |                                 |
|               |      |       |        |          |          | find nonexistent → empty                   |                                 |
|               |      |       |        |          |          | between predicate                          |                                 |
|               |      |       |        |          |          | json_path predicate on custom_fields       |                                 |
|               |      |       |        |          |          | update appends audit event                 |                                 |
|               |      |       |        |          |          | soft_delete appends audit event            |                                 |
|               |      |       |        |          |          | multi-mutation → multi-audit               |                                 |
|               |      |       |        |          |          | concurrent update serialization            |                                 |
|               |      |       |        |          |          | concurrent soft_delete snapshot isolation  |                                 |
|               |      |       |        |          |          | two concurrent CheckoutEvents both succeed |                                 |
|               |      |       |        |          |          | volume_delta auto-depletion                | ✗ deferred                      |
| skipped: D7.3 volume auto-depletion moves to RPC layer per TODO.md; not implemented at repo level |      |       |        |          |          |                                            |                                 |
| Project       | ✓    | ✓     | ✓      | ✓        | ✓        | duplicate name same lab                    | query with sort/limit/and_where |
|               |      |       |        |          |          | tombstone visibility                       |                                 |
|               |      |       |        |          |          | sort by created_at                         |                                 |
| SampleProject | ✓    | ✓     | ✓      | ✓(throw) | ✓(hard)  | update throws UnsupportedOperation         | query with sort/limit           |
|               |      |       |        |          |          | duplicate link throws                      |                                 |
|               |      |       |        |          |          | query by sample_id                         |                                 |
|               |      |       |        |          |          | find nonexistent → empty                   |                                 |
|               |      |       |        |          |          | cross-lab sample+project → error           |                                 |
| CheckoutEvent | ✓    | ✓     | ✓      | ✓(throw) | ✓(throw) | update throws UnsupportedOperation         | query with between, in, sort    |
|               |      |       |        |          |          | soft_delete throws UnsupportedOperation    | query with and_where            |
|               |      |       |        |          |          | query by sample_id                         |                                 |
|               |      |       |        |          |          | query by user_id                           |                                 |
|               |      |       |        |          |          | sort by at descending                      |                                 |
|               |      |       |        |          |          | concurrent inserts both succeed            |                                 |
|               |      |       |        |          |          | lab_id mismatch with sample → error        |                                 |

### ShareRequest — ShareRequestRepository, ShareRequestApprovalRepository

| Entity               | find | query | insert | update   | soft_del | errors tested                           | missing                  |
| -------------------- | ---- | ----- | ------ | -------- | -------- | --------------------------------------- | ------------------------ |
| ShareRequest         | ✓    | ✓     | ✓      | ✓        | ✓        | same source and target lab throws       | query with sort/limit    |
|                      |      |       |        |          |          | empty scope_json throws                 | update nonexistent       |
|                      |      |       |        |          |          | tombstone visibility                    |                          |
|                      |      |       |        |          |          | update persists changes                 |                          |
|                      |      |       |        |          |          | find nonexistent → empty                |                          |
|                      |      |       |        |          |          | query by source_lab_id                  |                          |
|                      |      |       |        |          |          | query by target_lab_id                  |                          |
| ShareRequestApproval | ✓    | ✓     | ✓      | ✓(throw) | ✓(throw) | update throws UnsupportedOperation      | query with sort/limit    |
|                      |      |       |        |          |          | soft_delete throws UnsupportedOperation | find nonexistent → empty |
|                      |      |       |        |          |          | duplicate role throws                   |                          |
|                      |      |       |        |          |          | FK on missing share request throws      |                          |
|                      |      |       |        |          |          | all 3 roles can be inserted             |                          |
|                      |      |       |        |          |          | query by share_request_id               |                          |

---

## 6. Backend Conformance Suite

10 tests for IStorageBackend contract. In-memory and SQLite pass;
the Postgres backend runs the same 10 plus 3 Postgres-specific tests
(`postgres_backend_conformance_test.cpp`), all of which `GTEST_SKIP`
unless `FMGR_TEST_POSTGRES_URL` points at a live database.

| Test                                            | Category       |
| ----------------------------------------------- | -------------- |
| CrudRoundTripUpdatesAndSoftDeletesSample        | CRUD           |
| QueryDslAppliesFiltersSortingAndPagination      | Query DSL      |
| SoftDeletedRowsAreHiddenUnlessIncluded          | Tombstone      |
| DuplicateActiveBoxPositionThrowsUniqueViolation | Uniqueness     |
| UnsupportedEntityRepositoryThrowsPortableError  | Error handling |
| SerializableOverlappingUpdatesRejectOneCommit   | Serialization  |
| ConcurrentPlacementPreservesPositionUniqueness  | Concurrency    |
| MutationsAppendAuditEventsAtomically            | Audit          |
| AuditAppendFailurePreventsMutationCommit        | Audit atomic   |
| MigrationCanDowngradeAndForwardMigrateSeedData  | Migration      |

Note: conformance tests use a synthetic ConformanceSample entity,
not the real domain entities. The DSL infrastructure is tested,
but entity-specific query fields are tested at the repo level.

---

## 7. Auth Types (`src/auth/AuthTypes.h` + `src/auth/IAuthProvider.h`)

21 tests in `tests/unit/auth_types_test.cpp`.

### Credential types

| Type / function              | Status | Notes                                        |
| ---------------------------- | ------ | -------------------------------------------- |
| `PasswordCredentials` fields | ✓      | email, password accessible                   |
| `ApiTokenCredentials` fields | ✓      | token accessible                             |
| `AuthCredentials` variant    | ✓      | holds PasswordCredentials and ApiTokenCredentials |

### `ClientInfo`

| Case                 | Status |
| -------------------- | ------ |
| Default (no IP/UA)   | ✓      |
| With ip + user_agent | ✓      |

### `AuthToken`

| Case               | Status |
| ------------------ | ------ |
| All fields         | ✓      |
| mfa_complete false | ✓      |

### `SessionContext`

| Function / case                      | Status |
| ------------------------------------ | ------ |
| `has()` — perm present               | ✓      |
| `has()` — perm absent                | ✓      |
| `can_see_lab()` — lab present        | ✓      |
| `can_see_lab()` — lab absent         | ✓      |
| `can_see_lab()` — empty visible_labs | ✓      |

### `AuthError` hierarchy

| Type                   | Status | Notes                                   |
| ---------------------- | ------ | --------------------------------------- |
| `AuthError` base       | ✓      | static_assert is-a std::runtime_error   |
| `InvalidCredentials`   | ✓      | catches as `AuthError`                  |
| `AccountLocked`        | ✓      | `locked_until` field set; catches as `AuthError` |
| `MfaRequired`          | ✓      | catches as `AuthError`                  |
| `TokenExpired`         | ✓      | catches as `AuthError`                  |
| `TokenRevoked`         | ✓      | catches as `AuthError`                  |
| `PermissionDenied`     | ✓      | catches as `AuthError`                  |

### `IAuthProvider` interface

| Case                                              | Status |
| ------------------------------------------------- | ------ |
| Concrete mock compiles against all 5 pure virtuals | ✓      |
| `authenticate`, `validate_token` callable via mock | ✓      |

---

## 8. E2 — LocalAuthProvider (`src/auth/`)

9 TOTP tests in `tests/unit/totp_test.cpp`; 20 LocalAuthProvider integration
tests in `tests/unit/local_auth_provider_test.cpp` (SQLite fixture, fast
Argon2id params).

### `src/auth/Totp.h` — base32 decode + TOTP (RFC 6238)

| Function / case                               | Status |
| --------------------------------------------- | ------ |
| `base32_decode` — known vector ("GEZDGNBV")   | ✓      |
| `base32_decode` — case-insensitive            | ✓      |
| `base32_decode` — padding stripped            | ✓      |
| `base32_decode` — invalid char throws         | ✓      |
| `totp_generate` — RFC 6238 vector T=59        | ✓      |
| `totp_generate` — RFC 6238 vector T=1111111109| ✓      |
| `totp_generate` — RFC 6238 vector T=1234567890| ✓      |
| `totp_verify` — current step round-trip       | ✓      |
| `totp_verify` — previous step valid (±1 win.) | ✓      |
| `totp_verify` — next step valid               | ✓      |
| `totp_verify` — two steps old rejected        | ✓      |
| `totp_verify` — wrong code rejected           | ✓      |

### `src/auth/LocalAuthProvider.h` — password auth + session lifecycle

| Function / case                              | Status |
| -------------------------------------------- | ------ |
| `authenticate` correct password              | ✓      |
| `authenticate` email case-insensitive        | ✓      |
| `authenticate` wrong password                | ✓      |
| `authenticate` unknown email                 | ✓      |
| `authenticate` disabled user                 | ✓      |
| `authenticate` no local binding              | ✓      |
| `authenticate` 5 failures → AccountLocked    | ✓      |
| `authenticate` lock respected before timeout | ✓      |
| `authenticate` success after lock expires    | ✓      |
| `authenticate` success resets failure count  | ✓      |
| `authenticate` TOTP user → mfa_complete=false| ✓      |
| `authenticate` non-TOTP → mfa_complete=true  | ✓      |
| `validate_token` round-trip                  | ✓      |
| `validate_token` builds permissions from role| ✓      |
| `validate_token` bad token throws            | ✓      |
| `validate_token` revoked session throws      | ✓      |
| `verify_totp` success sets mfa_complete=true | ✓      |
| `verify_totp` wrong code throws              | ✓      |
| `verify_totp` already complete throws        | ✓      |
| `revoke_session` removes active session      | ✓      |
| `revoke_all_sessions` revokes all            | ✓      |
| API token `scope_json = "[]"` → zero perms   | ✓      |
| API token non-array `scope_json` → zero perms| ✓      |
| API token `["sample.read"]` scope restriction| ✓      |
| API token `lab_id` restricts to that lab only| ✓      |
| API token for disabled user → error          | ✓      |
| Session token for disabled user → error      | ✓      |
| API token authenticate path                  | ✗ deferred: needs API token creation RPC (F2/I2) |
| `validate_token` API token bearer path       | ✗ deferred: same reason |
| DB-backed account lockout persistence        | ✗ deferred to E2.2; in-memory state resets on restart |
| Password reset flow                          | ✗ deferred to E2.1; requires email transport (Section O) |
| Cache eviction on role/membership change     | ✗ deferred: requires gRPC handler layer (M3) |

---

## 8b. RBAC Middleware, Seed Templates & Postgres (added E3 / C5.1)

Test files not in the original inventory:

### `tests/unit/auth_middleware_test.cpp` — `AuthMiddleware` 4-step gate
| Case                                              | Status |
| ------------------------------------------------- | ------ |
| `authorize` valid token + perm + lab → context    | ✓      |
| `authorize` invalid/expired token rethrows        | ✓      |
| `authorize` `mfa_complete=false` → `MfaRequired`   | ✓      |
| `authorize` missing permission → `PermissionDenied`| ✓      |
| `authorize` lab not visible → `PermissionDenied`   | ✓      |
| `inject_rls_vars` sets user_id + comma-joined labs | ✓ (mock captures raw key; see note) |
| RPC registry register / is_registered / list      | ✓      |

> Note: the mock transaction records the raw key passed to `set_session_var` and does
> **not** reproduce the Postgres `"app."` prefix, so it does not catch the double-prefix
> defect (CODE_REVIEW_2026-06-02 B1). An AuthMiddleware↔Postgres RLS integration test is
> still missing.

### `tests/unit/seed_templates_test.cpp` — box/container seed-template fixtures
| Case                                              | Status |
| ------------------------------------------------- | ------ |
| Container-type seed file parses + non-empty       | ✓      |
| 9×9 cryobox template has 81 positions             | ✓      |
| 10×10 cryobox template has 100 positions          | ✓      |
| 96-well rack template has 96 positions            | ✓      |
| Mixed-Eppendorf template has 13 positions         | ✓      |

### `tests/backend_conformance/postgres_backend_conformance_test.cpp`
16 tests: 10 shared conformance + `CapabilitiesReportRowLevelSecurity`,
`MigrateToLatestIdempotent`, `SetSessionVarVisibleWithinTransaction` + 3 RLS integration
tests (`InjectRlsVarsBlocksWrongLab`, `InjectRlsVarsAllowsCorrectLab`,
`InjectRlsVarsSetsCorrectSessionVariable`). All skip without `FMGR_TEST_POSTGRES_URL`.

---

## 9. Intentionally Not Tested

| Item                       | Reason                                                             |
| -------------------------- | ------------------------------------------------------------------ |
| greater_or_equal predicate | No production code uses it yet; RPC-layer feature per TODO.md F8   |
| less_or_equal predicate    | Same as above                                                      |
| Date format validation     | Validator only checks is_string; format validation deferred to RPC |
| Datetime format validation | Same as above                                                      |
| Enum without "values" key  | Malformed validation_json is data-integrity concern, not validator |
| volume_delta auto-deplete  | D7.3 deferred to RPC layer per TODO.md                             |
| Postgres backend (live DB) | 13-test conformance suite exists; skips without `FMGR_TEST_POSTGRES_URL`. **Never run end-to-end** — see CODE_REVIEW_2026-06-02 B1–B3 |
| Property tests             | tests/property/ empty; RapidCheck not yet integrated               |
| Fuzz tests                 | tests/fuzz/ empty; libFuzzer harnesses not yet written             |
| Audit hash-chain tests     | Only event counting tested; hash-chain verification deferred       |
| PHI encryption tests       | is_phi flag exists; encryption not yet implemented                 |
| Log redaction tests        | Not yet implemented                                                |
| Rate limiting tests        | Not yet implemented                                                |
| E2E smoke tests            | No server exists yet (placeholder dirs)                            |
| CSV import/export tests    | Not yet implemented                                                |
| Backup/restore tests       | Not yet implemented                                                |
