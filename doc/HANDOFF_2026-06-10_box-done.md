<!-- SPDX-License-Identifier: AGPL-3.0-or-later -->
# FreezerManager — Handoff 2026-06-10 (BoxService landed)

Continuation of `doc/HANDOFF_2026-06-10.md`. BoxService (F3, first slice) is
**done and verified**. This file hands off the **next slice: ItemTypeService**,
plus the one open repo-state action. Strict TDD; data-safety + security first.

---

## 0. What just shipped (this session)

- **PR #17 merged → `main`** (squash, `eec07cb`). Entire RPC layer + CI fixes
  now on `main`.
- **`dev` realigned locally** to `origin/main` (`git reset --hard origin/main`).
  Verified `dev...origin/main` == 0/0; `grpc_integration` label + clang-tidy
  `proto_generated` exclusion present.
- **BoxService implemented** on branch `feat/rpc-box-service` (commit `c961413`,
  **pushed; no PR opened**). All 18 box.proto RPCs, copying the LabService
  pattern. New `src/server/BoxServiceImpl.{h,cc}`,
  `tests/integration/box_service_integration_test.cpp` (39 cases). Stub removed
  from `UnimplementedStubs.cc`.
- **Verified green:** `ctest` dev 721/721 (SQLite) and 721/721 live `postgres:16`;
  BoxService 39/39; asan 656/656 (grpc excluded); tsan green w/ suppressions;
  clang-tidy clean; clang-format-17 + SPDX clean.

### ⚠ Open action before/with the next PR (repo state)
`origin/dev` is **still the stale pre-#17 unsquashed branch** — the force-push to
realign it onto `main` was blocked by a shared-branch guardrail. `dev...main`
content is identical; only the remote branch pointer is stale. Pick one:
- **Realign remote dev:** `git push --force-with-lease origin dev` (with local
  `dev` = `origin/main`), then PRs target `dev` cleanly. *(Needs the human to
  authorize the force-push — the agent classifier blocks it.)*
- **Or PR `feat/* → main` directly** (handoff §8 fallback). Because feat branches
  are based on `eec07cb` = `origin/main`, a `feat→main` PR shows only the slice's
  diff. Clean, CI runs.

`feat/rpc-box-service` is pushed but **has no PR** — the human deferred the base
choice. Open it (`→ main` is clean) before/while starting the next slice, or
stack the next branch on top of it.

---

## 1. Next phase — ItemTypeService (9 RPCs)

Replace the `ItemTypeServiceStub` in `src/server/UnimplementedStubs.cc`. This is
the second slice of the sample-placement vertical (a sample needs an
`item_type_id`). Proto: `proto/fmgr/v1/item_type.proto`. Two resources:

**ItemType** (gate `Permission::ItemTypeDefine`) — `ItemType` is a self-
referential taxonomy (`optional parent_id`), fields: id, lab_id, parent_id, name.
- ListItemTypes(lab_id) / GetItemType(item_type_id) / CreateItemType(lab_id,…) /
  UpdateItemType(ItemType) / ArchiveItemType(item_type_id)

**CustomFieldDefinition** (gate `Permission::CustomFieldDefine`) — fields: id,
lab_id, scope_kind (enum), optional item_type_id, key, label, data_type (enum),
required, validation_json, indexed, is_phi.
- ListCustomFieldDefinitions(ListCfdsRequest) / CreateCustomFieldDefinition(CreateCfdRequest) /
  UpdateCustomFieldDefinition(UpdateCfdRequest) / ArchiveCustomFieldDefinition(ArchiveCfdRequest)

Both permissions are **lab-scoped** (verify with `is_global_only_permission` in
`src/core/permissions.h` — both return false). Gate exactly as BoxService did.

Entities: `src/core/item_type.h` (`core::ItemType` @136, `core::CustomFieldDefinition`
@163, each with `Field` enum). Repos: `register_item_type_repositories(backend)`
(`src/storage/sqlite/ItemTypeRepositories.h`, postgres mirror exists). Traits:
`src/storage/ItemTypeTraits.h`. Proto↔core enum mapping needed for `ScopeKind` and
`FieldDataType` (see the ContainerKind mapping in `BoxServiceImpl.cc` for the
pattern — explicit `switch` both ways, default to UNSPECIFIED/first member).

**The template to copy is now `src/server/BoxServiceImpl.{h,cc}`** (closest match:
lab-scoped, entity-id-only RPCs, soft-delete handling). LabService is the older
reference. Mirror `tests/integration/box_service_integration_test.cpp` for tests.

---

## 2. The handler pattern (unchanged — copy BoxService)

Per RPC:
1. Parse ids. For RPCs whose request carries `lab_id` (List*/Create*/Update*):
   `auto sctx = middleware_.authorize(extract_bearer(*ctx), Permission::X, lab_id);`
2. For **entity-id-only** RPCs (Get*/Archive*): `auto sctx =
   auth_.validate_token(extract_bearer(*ctx));` + MFA check, open txn, inject RLS,
   `find_by_id`, then `if (!e || e->archived_at) NOT_FOUND;` and
   `if (!sctx.has_for_lab(e->lab_id, Permission::X)) throw PermissionDenied(...);`
3. `auto txn = backend_.begin(IsolationLevel::ReadCommitted|Serializable);`
   `rpc::AuthMiddleware::inject_rls_vars(*txn, sctx);`
4. `txn->repo<core::Entity>()` → find/query/insert/update/soft_delete with
   `make_ctx(sctx, "reason")`. Serializable for writes, ReadCommitted for reads.
5. Marshal; wrap body in `try { … } catch (...) { return current_exception_to_grpc_status(); }`.
6. Register every RPC name in the ctor via `AuthMiddleware::register_rpc(...)`
   (keep the same permissions the stub already declares — a CI test asserts every
   method is registered).
7. Wire into `FreezerServer.{h,cc}` (member + init + `RegisterService`), delete the
   stub class + its `register_stub_services` entry, add the `.cc` to
   `src/server/CMakeLists.txt`.

### Hard-won lessons from BoxService (will bite ItemType too)
- **`find_by_id` returns tombstoned rows** (by-id load has no `archived_at`
  filter). `Get*` must return NOT_FOUND when `archived_at.has_value()`. `query()`
  excludes tombstoned by default, so `List*` is fine.
- **Repository `validate()` enforces shape/refs**, surfacing as
  `ConstraintViolation` → gRPC `INVALID_ARGUMENT`. Check the repo's `validate_*`
  before writing tests: e.g. CustomFieldDefinition likely requires non-empty
  `key`/`label` and may validate `validation_json`/scope. Seed prerequisites in
  test helpers (BoxService needed a live `container_type.size_class` before a
  `box_type` position's `accepts` would validate).
- **Proto enum ≠ core enum**: map explicitly both directions.
- Lab-scoped perms never appear in `global_permissions`; gate on the request lab.

---

## 3. Tests (TDD — write first)

Mirror `tests/integration/box_service_integration_test.cpp`:
- Reuse the fixture: temp SQLite, `register_all_repositories()`, server on
  `localhost:0`, `NewStub`. Seed **admin** (SystemAdmin lab1), **member** (Member
  lab1), **outsider** (SystemAdmin lab2-only) — member/outsider give negative
  authz; outsider doubles as cross-lab isolation. (Member holds `SampleRead`; pick
  the negative principal per the gated permission.)
- ≥1 positive + ≥1 negative authz per RPC; lab isolation; soft-delete-hides.
- Add target to `tests/integration/CMakeLists.txt`, link `server_lib`,
  `storage_sqlite`, `auth`, `gRPC::grpc++`, `GTest::gtest_main`, and
  `gtest_discover_tests(... PROPERTIES LABELS "grpc_integration")`.
- Suppress `bugprone-easily-swappable-parameters` on multi-`std::string` test
  helpers with `// NOLINTNEXTLINE(...)` (clang-tidy is warnings-as-errors).

---

## 4. Build / verify (exact)

```sh
conan install . --lockfile=conan.lock --output-folder=out/conan/dev \
    --build=missing -s build_type=Debug -s compiler.cppstd=20
cmake --preset dev && cmake --build --preset dev && ctest --preset dev
```
- **Local preset gotcha:** `cmake --preset/--build --preset` fail with
  `Duplicate preset: "conan-debug"`. Bypass with explicit dirs:
  `cmake --build out/build/dev -j"$(nproc)"` and
  `ctest --test-dir out/build/dev`.
- Live Postgres (else PG suites skip):
  ```sh
  docker run --rm -d --name fmgr-test-pg -e POSTGRES_DB=fmgr_test \
    -e POSTGRES_USER=postgres -e POSTGRES_PASSWORD=test -p 5432:5432 postgres:16
  export FMGR_TEST_POSTGRES_URL=postgresql://postgres:test@localhost/fmgr_test
  ctest --test-dir out/build/dev
  ```
- clang-tidy (CI-exact): `run-clang-tidy-17 -p out/build/dev -j"$(nproc)" -quiet '^(?!.*proto_generated).*$'`
- clang-format: **`/usr/bin/clang-format-17`** only.
- Sanitizers exclude the gRPC integration label:
  `ctest --test-dir out/build/asan --label-exclude grpc_integration` (likewise tsan).
  **tsan via `--test-dir` drops the preset's suppressions** — pass them manually:
  `TSAN_OPTIONS="suppressions=$PWD/tools/tsan.suppressions" ctest --test-dir out/build/tsan --label-exclude grpc_integration`.
  The `sqlite3.c walIndexWriteHdr` race is a known false positive (the test still
  asserts/passes); CI's preset wires the suppressions automatically.

---

## 5. Definition of done (ItemTypeService PR)

- All 9 RPCs implemented; `ItemTypeServiceStub` removed from
  `UnimplementedStubs.cc` and `register_stub_services`.
- Positive + negative authz per RPC; lab isolation + soft-delete-hides covered.
- `ctest` green on SQLite **and** live `postgres:16`.
- clang-tidy exits 0 (proto filter); clang-format-17 + SPDX clean; asan/tsan green
  (grpc_integration label-excluded).
- Commit: `git commit -s`, Conventional Commits, **no `Co-Authored-By`** (CLAUDE.md).
- PR base resolved per §0 (clean `→ main`, or realigned `dev`).

---

## 6. After ItemTypeService

Per the original handoff order: **SampleService** (8 RPCs — uses
`storage::move_sample` in `src/storage/SampleOps.h` + audit chain), then
**RoleService** (8), **AuditService** (4, reuse `src/audit/AuditChainVerifier.h`),
**ShareService** (6). One service per PR, smallest RPC first, TDD.
