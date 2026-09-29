# Handoff note — 2026-09-29, [rbac] Member cannot read the lab layout (#54, worker-3)

A lab `Member` could not read the lab layout needed to interpret a sample's
location. Three **read** RPCs behind the layout screen — `ListFreezers`,
`ListStorageContainers`, `ListBoxTypes` — required *configure* permissions
(`freezer.configure`/`box.configure`) that a Member does not hold, so every
Member passed the SPA's declared `sample.read` route gate and then got `403` on
three of the four calls behind it. The Qt client's `LocationPathResolver` needs
the same data and had the same problem. Pre-existing, found by worker-2 while
building G3.1 (#51). The server was the enforcement point that was wrong; no
proto, route, REST or SPA change was needed. PRD §3.

**Changed:** `src/server/BoxServiceImpl.cc` only, 15 insertions / 7 deletions,
six semantic changes plus comments.

- `ListFreezers` (`:247`), `ListStorageContainers` (`:417`) and `ListBoxTypes`
  (`:622`) now `authorize()` on `core::Permission::SampleRead`.
- The three matching `AuthMiddleware::register_rpc` entries in the same file's
  constructor (`:213`, `:218`, `:228`) move to `P::SampleRead` in the same
  commit, so the gate and the registry cannot disagree.
- `tests/integration/box_service_integration_test.cpp`: three
  `*RejectsMember` tests inverted into positive ones asserting real rows come
  back, `ReadOnly` coverage added for the same three, a fourth principal
  (`readonly@example.com`, `RoleKind::ReadOnly` in lab1) seeded, and two new
  boundary tests.

**Decisions:**

1. **`sample.read`, not a new `layout.read`.** The issue offered both. Chosen
   because (a) it makes `BoxService` self-consistent with the precedent already
   in the same file — `ListBoxes` (`:680`) and `GetBox` are `SampleRead` today,
   so these three were the outliers; (b) the evidence for "the layout read set"
   is the consumers, and both name the same four RPCs: `src/qt/BoxServiceClient`
   calls exactly `ListFreezers`, `ListStorageContainers`, `ListBoxTypes` (+
   `ListBoxes`), and G3.1's `useLabLayout` (`src/features/layout/useLabLayout.ts`,
   merged as `037f3de` mid-task) composes exactly those same four — its own
   `src/api/hooks/layout.ts` comment names them. Nothing calls
   `GetFreezer`/`ListContainerTypes` on the layout path; (c) the SPA `layout`
   route declares `permissions: ['sample.read']` (`route-map.tsx:143`) and the
   REST gateway does no permission check of its own — it forwards to the gRPC
   gate — so the server now agrees with the contract it publishes instead of
   adding a third answer; (d) `doc/PRD.md:105` defines permissions as
   `(action, entity)` pairs over entities the PRD names, and there is no
   `layout` entity — a permission every role receives distinguishes nothing.
   Argued in full on PR #57.
2. **No role's grant set changed; the gates did.** `Member`, `ReadOnly` and
   `ApiClient` thereby gain the three list RPCs. That is the permission-model
   consequence the issue asked to be a decision on the record, so it is
   asserted in both directions rather than implied.
3. **Scope held at three RPCs.** `ListContainerTypes` (`:558`,
   `box.configure`) and `GetFreezer` (`:283`, `freezer.configure`) have the same
   *shape* of defect but are not in the layout read set — confirmed against the
   merged G3.1 screen, which composes exactly the four RPCs above and never calls
   either of these — so they keep their requirement;
   `ListContainerTypesStillRejectsMember` pins that boundary so a
   later "relax all the reads" commit has to argue with a red test. Widening
   them is a separate call, not an unrequested expansion here.
4. **Two line anchors in the issue were wrong and one was dangerous.** `:299` is
   `CreateFreezer` and `:558` is `ListContainerTypes`; the permission→RPC mapping
   was right in all three cases, but editing by line number would have relaxed a
   *mutating* RPC. Corrected on the issue before any code changed.

**Tests:** all in `tests/integration/box_service_integration_test.cpp`. Red
first — with the gates unchanged, `ctest --preset dev -R 'BoxService'` was
`84% tests passed, 7 tests failed out of 44`, and the seven were the new
`*AllowsMember`/`*AllowsReadOnly` tests plus
`MemberCanReadLayoutButStillCannotArchiveFreezer`, failing with
`caller lacks required permission for target lab` (the RBAC gate, not an
unrelated error).

After the fix:

- `ctest --preset dev -R 'BoxService'` → **44/44 passed** (was 37/44).
- `ctest --preset dev -R 'Rbac|AuthMiddleware|Permission|RoleService|ServerIntegration'`
  → **85/85 passed**, 4 Postgres skipped (no `FMGR_TEST_POSTGRES_URL`).
- `ServerIntegrationTest.RpcRegistryCoversAllExpectedMethods` → passed.
- `ctest --preset dev` → **100% tests passed out of 1441**, 84.88 s, exit 0.
- `clang-format --dry-run --Werror` (17.0.6) over all tracked C/C++/proto → exit
  0, no violations. `tools/check-spdx-headers.sh` → exit 0.

The negative direction is asserted, not assumed:
`MemberCanReadLayoutButStillCannotArchiveFreezer` calls `ListFreezers` as a
Member (expects OK) and then `ArchiveFreezer` on the same freezer (expects
`PERMISSION_DENIED`), so a relaxation that also loosened the mutating path fails
that test. Every pre-existing Member-denied test for `CreateFreezer`,
`UpdateFreezer`, `ArchiveFreezer`, the container create/update/archive trio, and
the box-type/box mutations stays green as the regression net.

**Known limitations / follow-ups:**

- **The RBAC registry is not an enforcement point and its test does not test
  what its name says.** `AuthMiddleware::registered_rpcs()` and
  `is_rpc_registered()` have **no callers anywhere in `src/`** — the map is
  write-only metadata, and the real gate is the `authorize()` call inside each
  method. `ServerIntegrationTest.RpcRegistryCoversAllExpectedMethods` asserts
  only `EXPECT_GE(registry.size(), 60U)`: a count floor, not per-RPC coverage
  and not that a registered permission matches the enforced one. An entry could
  say `sample.read` while its method enforces `freezer.configure` and every test
  stays green. This is why the fix updated both by hand; nothing checks the
  agreement. AGENTS.md §5's claim that the test "enforces this" is therefore
  weaker than it reads. Not fixed here — that is a new issue, the lead owns
  issue creation, and it is raised on #54 rather than absorbed into this commit.
- The `layout.read` escape hatch is deliberately not built. The day a lab wants
  "the layout but not the samples" — a facilities/estates persona `doc/PRD.md`
  does not currently have — `src/core/permissions.h` plus these three
  `authorize()` sites are the whole change.
- No sanitizer run: this is a permission-gate change touching no memory,
  concurrency, storage or parser code, so per AGENTS.md §4 `asan` is not
  required and was not run.
