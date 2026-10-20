# Handoff note — 2026-10-20, [docs] the demo seed exercises audit history, the PHI gate and two box grids (#143, worker-2)

Issue **#143**: the demo database told the truth about none of three headline
features. Every sample row was written with raw SQLite `INSERT`s, so
`audit/list` was empty and a sample's History said "Nothing has happened to this
sample yet"; no custom field was marked PHI and no lab had PHI mode on, so the
`phi.read` gate could never be demonstrated; and `seed_box_types` keyed its
result by `size_class`, so both templates (which share `cryovial_2ml`) landed on
one key, the 10×10 overwrote the 9×9, and every box came out A1–J10.

Branch `feat/143-seed-demo-gaps`, PR **#162**.

**Changed:**

- `scripts/seed_demo.py`
  - **Samples now go through `freezerctl sample import`**, the same storage
    write path the server uses: one transaction per batch, `note_mutation` per
    row (a real `sample.insert` audit event), and `prepare_custom_fields`
    splitting/encrypting PHI. The script builds the rows, writes a CSV per lab,
    runs two passes (parents/standalone first, aliquots second with parent ids
    resolved by name), then verifies the hash chain with
    `freezerctl audit verify`.
  - **PHI demo:** the Neuroscience lab is PHI-enabled, CSF `diagnosis` is the
    one `is_phi = 1` definition, and the built-in **SystemAdmin** role is
    granted `phi.read` through `role_permissions` — so `admin@freezer.local`
    (the existing demo login) sees the diagnosis and the Member/ReadOnly logins
    do not. Values are synthetic trial labels ("Hydrocephalus", "NPH", …), never
    names, MRNs or dates of birth.
  - **Two grids:** `seed_box_types` is keyed by `"9x9"`/`"10x10"`, and the four
    boxes are 2 × 9×9 + 2 × 10×10, so the box view can show both layouts.
  - **Runs without argon2-cffi:** the demo password `admin` falls back to a
    precomputed Argon2id hash when the package is absent (CI has no
    argon2-cffi), so the seed and its test run on a bare python3. With argon2
    present it still mints a fresh hash per run.
  - **Requires `FMGR_MASTER_KEK`** (and a built `freezerctl`, found under
    `out/build/dev` or via `FMGR_FREEZERCTL`). It fails before writing anything
    if either is missing, because the PHI value must be encrypted under the key
    the demo server will decrypt with.
- `tests/integration/demo_seed_integration_test.cpp` (new) + `tests/integration/CMakeLists.txt`
  — runs the real seed against a fresh migrated SQLite file, then starts the
  real gRPC server over it and asserts the seeded shape:
  `SeedCompletesInOneCommandAndReportsWhatItCreated`,
  `EverySeededSampleHasRealAuditHistory` (32 `sample.insert` events, chain
  verifies, `ListAuditEvents` for the seeded sample returns the insert),
  `SeededPhiGateShowsTheValueToAPhiReaderAndHidesItFromReadOnly` (admin sees
  `diagnosis=Hydrocephalus`; ReadOnly `zhang` and Member `li` do not; at-rest
  envelope opaque), and `SeedUsesBothBoxGrids` (both types used; 81 and 100
  positions).

No production C++ changed; the test is labelled `grpc_integration`.

**Decisions:**

- **`freezerctl sample import`, not the server API.** The seed runs with the
  server stopped, and the import path is the same storage layer the server's
  `CreateSample` uses — same audit chain append, same PHI split. Driving the
  API would have required a running server, a login and TOTP handling inside the
  seeder for no extra fidelity.
- **Which events are seeded: one `sample.insert` per sample.** The samples are
  the entities whose History the demo screen shows, and inserting them is the
  only mutation the seed performs through the audited path. Reading a sample as
  `admin` then adds `phi.read` events, which is the visible gate in the demo
  History.
- **The PHI grant is on SystemAdmin, not a new demo user.** `phi.read` is
  excluded from every built-in role by default; granting it via
  `role_permissions` is the supported configuration mechanism (the existing
  service integration fixture does the same). It keeps the documented login
  list unchanged and gives the gate a positive and two negative accounts.
- **One field is PHI: CSF `diagnosis`.** Four CSF samples carry a diagnosis, so
  the demo has encrypted data without turning the whole lab into a PHI lab. The
  Plasma `diagnosis` definition stays non-PHI on purpose, which makes "tagged
  field" visible as a per-definition choice.
- **Sample ids are minted by the import path (v4), not the old fixed `b101…`
  ids.** Nothing in the repo referenced the seeded sample ids; parent links are
  resolved by name after the first pass. The item types, boxes, labs and users
  keep their fixed ids.
- **The precomputed password hash is a fallback, not a replacement.** Demo
  credential, already printed by the script; argon2-cffi is still used whenever
  it is importable.

**Tests:**

Red first, against the pre-change seed (`FMGR_SEED_PYTHON` pointed at a python
with argon2-cffi so the old script could run):

```
$ FMGR_SEED_PYTHON=.venv/bin/python3 \
    CTEST_PARALLEL_LEVEL=1 ctest --preset dev -R DemoSeedTest
0% tests passed, 4 tests failed out of 4
  SeedCompletes…: no "32 audit events", no "OK: 32 audit event(s) verified"
  EverySeededSampleHasRealAuditHistory: audit events 0, expected 32
  SeededPhiGate…: is_phi_enabled "0", is_phi "0", envelope "{}",
                  and the ReadOnly account could still see diagnosis
  SeedUsesBothBoxGrids: 9x9 boxes 0, 9x9 positions 0
```

Green, final commit:

```
$ CTEST_PARALLEL_LEVEL=1 ctest --preset dev -R DemoSeedTest
100% tests passed out of 4

$ CTEST_PARALLEL_LEVEL=1 ctest --preset dev
100% tests passed out of 1661        # 0 failed, 231 skipped (Postgres URL unset)

$ clang-format --dry-run --Werror $(git ls-files …)   # exit 0
$ tools/check-spdx-headers.sh                         # exit 0
```

Demo flow, run against a scratch database (server stopped between seed and
serve, as the runbook does):

```
$ FREEZER_DB_PATH=/tmp/fmgr-143-demo/demo.db FMGR_MASTER_KEK=… \
    FMGR_FREEZERCTL=out/build/dev/src/cli/freezerctl python3 scripts/seed_demo.py
  32 samples, 32 audit events; OK: 32 audit event(s) verified
  PHI: Neuroscience Lab enabled, 4 encrypted sample(s), CSF `diagnosis` tagged
  box grids in use: 10x10 Cryobox x2, 9x9 Cryobox x2

# freezerd on 127.0.0.1:18100 (slot 2 ports), REST over a cookie-less bearer token:
login admin            -> session_token, mfa_required false
sample/list query=P001 -> 6 samples
sample/get P001        -> custom_fields has diagnosis=Hydrocephalus
audit/list (entity)    -> [{action:"insert"}, {action:"phi.read"}, …]
login zhang (ReadOnly) -> sample/get has no diagnosis key
box-type/list          -> 9x9 Cryobox 81 positions, 10x10 Cryobox 100 positions
box/list               -> boxes reference both type ids
container-type/list    -> 200, 4 types with dimensions absent
```

The last call needed #138's one-line seed fix (`outer_dimensions_json =
'null'`) applied to the scratch database by hand, because #138 is a separate
open PR on `main` at the time of writing; the demo seed itself inherits that
fix when #138 merges (it touches the same line in `seed_container_types`). The
#143 seed was rebased onto `main` after this run.

**Known limitations / follow-ups:**

- **`FMGR_MASTER_KEK` is now part of the demo.** After #143, `#142`'s
  `scripts/demo-setup.sh` must export a fixed demo key before seeding **and**
  before starting `freezerd`; otherwise the server has no KMS, `GetSample`
  returns without PHI even for `admin`, and the gate demo looks broken. That is
  a one-line change in #142's script (out of this PR's scope — #142 is another
  worker's branch), and #142's "re-verify after #143" step is where it belongs.
- The seed's sample import runs `freezerctl`, so the seed needs a built dev
  tree (the runbook already required the server binary). `tests/integration`
  now configures `Python3` so the new test can find an interpreter; both are
  present in CI.
- Postgres was not seeded or tested: `seed_demo.py` is SQLite-only by design,
  and the Postgres tests skip without `FMGR_TEST_POSTGRES_URL`.
- No asan run: no production C++ changed, and the new test is
  `grpc_integration`, which the sanitizer presets exclude.
