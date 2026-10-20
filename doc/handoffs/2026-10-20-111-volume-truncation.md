# Handoff note — 2026-10-20, #111 a sub-unit volume truncates to zero (worker-1, finished by worker-3)

A check-in could accept a volume the domain cannot store and subtract nothing:
`parse_volume_used` cast the wire `double` with `static_cast`, so `0.04 mL` became
raw `0`; and `Volume::to_unit` divides µL by 1000, so `500 µL` against an
mL-tracked sample became `used = 0`. Both answered `OK`, returned the vial to
Active and left the volume unchanged — a silent no-op. This slice makes a
consumption **exact or refused** at the wire and at the subtraction, and mirrors
the refusal in `fakeApi.ts` so a client cannot be green against a more generous
fake. PR **#135**, branch `fix/111-volume-truncation`.

**Changed:**

- `src/core/quantity.h` — `Volume::converts_exactly_to(VolumeUnit)`: the
  refinement direction (`mL` → `µL`) is exact unless the product leaves the raw
  count's range; the truncating direction (`µL` → `mL`) is exact only when the
  raw count is a whole number of millilitres. `to_unit` keeps its truncating
  semantics for callers that want the nearest whole unit.
- `src/storage/SampleOps.h` — `apply_checkout` asserts `converts_exactly_to`
  next to the negative guard (#112), before its switch, so a consumption the
  sample's unit cannot hold is refused at the arithmetic that would otherwise
  silently consume the wrong amount.
- `src/server/SampleServiceImpl.cc` — `parse_volume_used` refuses a fractional
  count of the stated unit (`amount != std::trunc(amount)`, the same raw-double
  style as #112's sign check) and a value outside `int64` (the cast below it is
  UB there). The error names `volume_used`.
- `src/web/src/test/fakeApi.ts` — `exactVolumeConversion` returns `null` instead
  of truncating, and the checkout resolver refuses with `INVALID_ARGUMENT` at
  both points. Two tests in `fakeApi.test.ts` and one screen-level test in
  `SampleDetailScreen.test.tsx` pin the refusal and that nothing was applied.
- `tests/integration/sample_service_integration_test.cpp` — five tests: the
  mL/µL no-op, `0.04 mL`, partial truncation `2500 µL`, the accepted exact
  `1 mL` against a µL sample, and the granularity boundary (1 µL, 0.9 µL,
  explicit 0). The fixture gained `volume_ml` so a sample can be created in mL.
- `tests/unit/core_value_types_test.cpp` — the predicate, including the range
  edge. `tests/unit/sample_repository_test.cpp` — the storage guard with the
  sample and event read back afterwards.

**Decisions:**

- **Refuse, don't round** (settled on the issue by worker-1; no rounding policy
  is invented here). A `double` cannot express the distinction a "convert when
  exact" rule would need: `0.04 * 1000 == 40.0` exactly, while
  `1.001 * 1000 == 1000.9999999999999`; measured, 2945 of the 200000
  three-decimal values in `[0.001, 200]` fail that round trip. Any rule that
  accepts them has to round, which is a separate decision and the wrong thing
  to put in a validation helper.
- **Zero stays accepted and distinguishable.** It is a whole number of any
  unit; `volume_delta: 0` remains distinct from an absent delta, so #112's
  boundary is untouched.
- **The guard also lives in `apply_checkout`, not only at the wire**, for the
  same reason the sign guard does: that is the subtraction that would lose the
  quantity, and a caller arriving any other way is refused too.
- **The fake mirrors the refusal, not a rounded amount.** A fake that rounds
  would be more generous than `freezerd` — exactly how #100 and this issue
  drifted.

**Tests** (red first, then green; the assertions are on the stored sample and
the event, not the status alone):

- On `main` before the fix, the focused run failed **4 of 5** new integration
  tests (the guard against over-refusing passed before and after); the mL/µL
  case returned `status.error_code() == 0`, `ACTIVE`, volume unchanged at
  50 mL and `volume_delta` 0. Planted violation: with the `apply_checkout`
  guard commented out, the storage test and the two unit-mismatch integration
  tests go red; the fractional cases stay green because the wire check catches
  them — so both guards are load-bearing.
- This session, on the rebased tree (`bd4f952` + `8860a2e`, on `origin/main`
  `503245b`):
  - `ctest --preset dev -R 'Volume|Checkout' --output-on-failure` →
    **100% tests passed out of 54** (14 PostgreSQL parameters skipped without
    `FMGR_TEST_POSTGRES_URL`), including all six new/changed test names.
  - `ctest --preset dev --output-on-failure` → first run **1678/1679**, one
    failure: `GrpcTlsTest.MtlsRejectsClientWithoutCertificate` (SEGFAULT) at
    load average **8.3** while other agents compiled. Re-run isolated →
    **1/1 passed**; second full run → **100% tests passed out of 1679**. The TLS
    test is the known load-sensitive one, not a regression.
  - `npm run check` in `src/web` with Node **v22.23.3** (`out/tools/node22`),
    once with `NODE_ENV` unset and once with `NODE_ENV=production` → both
    exit 0: **50 test files / 805 tests passed**, `check-routes: ok — 74 unary
    routes and 2 SSE routes`, eslint **0 errors** (one pre-existing
    `src/ui/Table.tsx` warning), prettier and `tsc` clean, initial JS
    **203.0 KiB** gzipped (budget 250).

**Lint:**

- `clang-format --dry-run --Werror` on the six changed C++ files: clean. The
  local binary is Apple **clang-format 21.0.0**, not CI's `clang-format-17`; a
  whole-tree run flags pre-existing files this branch does not touch
  (`tests/unit/retention_policy_test.cpp`), which is the version difference, not
  this diff.
- **clang-tidy is not claimed as clean** from a local run. The previous push's
  CI `clang-tidy` job was green; this session changed no C++ after that.

**Known limitations / follow-ups:** a rounding policy for fractional volumes is
out of scope and still a separate decision. #155 proposes replacing the
hand-written fake rules with recorded server cases; until then `fakeApi.ts`'s
`exactVolumeConversion` is the mirror to keep in step with
`Volume::converts_exactly_to`.
