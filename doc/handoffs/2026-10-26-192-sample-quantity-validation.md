# #192 — server sample quantity validation

Server module of parent #180; paired web module is #193. PR #199 closes only #192.
Implemented by worker-1 (Codex).

## Result

CreateSample, UpdateSample and CheckoutSample now share `parse_quantity` in
`src/server/SampleServiceImpl.cc`. It checks the original double is finite,
nonnegative, whole and strictly below the exact exclusive bound `0x1p63` before
conversion to int64. Refusals use INVALID_ARGUMENT and name `volume_value`,
`mass_value` or `volume_used`. Units and the integer storage model are unchanged.

`UpdateSampleRequest` has no field mask: it replaces the sample's optional
fields, and absence clears them. This existing behavior is preserved and covered
explicitly, as are omitted create quantities and explicit zero.

Both ImportSamples and `freezerctl sample import` use `cli::build_import` in
`src/cli/SampleImport.cc`, rather than the wire conversion helper. Its checked
`from_chars` parser reads int64 text directly and already rejects fractional,
nonfinite and out-of-range text. Inspection found it accepted negative integers;
the lead approved the bounded nonnegative check in the quantity-only parser
([decision](https://github.com/Yuxin-Ren-SZ/FreezerManager/issues/192#issuecomment-6429017145)).
CSV retains the full int64 integer range, including INT64_MAX; it never passes
through a double.

## Regression evidence

Tests were committed first in `8886457` (original pre-rebase `3054d55`). After
correcting the fixture's mass unit
to the supported `mg`, the eight new tests had 4 failures and 4 passes before the
implementation: create, update, shared CLI import parsing and RPC import accepted
invalid input. `cc41ccc` supplies the fix (original pre-rebase `e689ab8`).
`5537f50` changes test trace/CSV string construction to satisfy Linux clang-tidy;
test inputs and assertions are unchanged.

- `tests/integration/sample_service_integration_test.cpp` tests both volume and
  mass with 0.5, -1, -0.5, 1e19, exactly 2^63, NaN and +infinity. Refused creates
  and updates compare all persisted samples and the audit chain to snapshots.
  Accepted zero, 42 and the largest double below 2^63 are checked against raw
  stored int64 values after creation and an actual value-changing update.
- Checkout gains 1e19, exactly 2^63, NaN and +infinity cases; refusal preserves
  sample rows, audit and checkout events.
- Shared CLI parsing and RPC import reject invalid quantity text for both fields;
  the RPC regression also proves no sample/audit write. CLI parser tests prove
  zero and INT64_MAX remain exact.

## Validation

All build/test commands ran from `.worktrees/192-sample-quantity-validation`
after `AGENT_SLOT=1 source scripts/agent/env.sh`. The final branch is based on
`fc251ec`, which includes #195 and #200. An initial sandbox-denied listener run
is not counted as behavioral evidence; the successful gRPC/full runs used
permitted loopback listeners. The retained standard and initial strict UBSan
runs precede the final web-only rebase; the strict exclusion run finished on the
final base. Final dev verification was repeated after that rebase.

- `cmake --build --preset dev`: passed.
- `ctest --preset dev -R 'Sample' --output-on-failure`: 204 passed, 71 PostgreSQL
  skips, 0 failed (275 registered).
- `ctest --preset dev`: 1503 passed, 232 PostgreSQL skips, 0 failed (1735 registered).
- `scripts/agent/conan-install.sh ubsan`, `cmake --preset ubsan`,
  `cmake --build --preset ubsan`: passed, using the shared prebuilt dependencies.
- With `UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`,
  `ctest --preset ubsan -R 'SampleServiceTest.(CreateSampleRejectsInvalidQuantities|UpdateSampleRejectsInvalidQuantities|CreateAndUpdateSampleStoreWholeQuantity|SampleQuantityOmission|CheckoutSampleRejectsNonfinite|ImportSamplesRejectsInvalidQuantity)|SampleImportTest.(RejectsInvalidQuantity|PreservesZero)' --output-on-failure`:
  8 passed, 0 skipped, 0 failed, including gRPC integration regressions.
- Strict full `UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 ctest --preset ubsan`:
  1502 passed, 232 PostgreSQL skips, 1 failed (1735 registered). The only failure
  is `RestErrorTranslationTest.StatusCodeOutOfEnumRange`: its intentional cast of
  99 to `grpc::StatusCode` triggers an invalid-enum load at
  `src/rest/RestErrorTranslation.h:27`. The same strict one-test command reproduces
  it (0 passed, 1 failed); `git diff --exit-code origin/main -- src/rest/RestErrorTranslation.h tests/unit/rest_error_translation_test.cpp`
  is clean, proving both offending files match the baseline. The lead queued
  [#202](https://github.com/Yuxin-Ren-SZ/FreezerManager/issues/202); no REST fix is
  included here.
- Standard `ctest --preset ubsan --verbose`: 1503 passed, 232 PostgreSQL skips,
  0 failed (1735 registered). Verbose output retains exactly one UBSan diagnostic,
  the existing #202 enum test described above; its default recoverable behavior
  means the standard preset passes, matching CI. This is not claimed as a
  diagnostic-free strict run.
- Strict `UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 ctest --preset ubsan -E '^RestErrorTranslationTest.StatusCodeOutOfEnumRange$'`:
  1502 passed, 232 PostgreSQL skips, 0 failed (1734 registered, only #202 excluded),
  no UBSan diagnostics. This includes gRPC/e2e tests; there is no grpc exclusion.
  The eight new quantity regressions also passed a strict focused rerun (8/8)
  after the test-string tidy fixes.
- `clang-format --dry-run --Werror src/server/SampleServiceImpl.cc src/cli/SampleImport.cc tests/integration/sample_service_integration_test.cpp tests/unit/cli_test.cpp`:
  passed (LLVM 17.0.6).
- `tools/check-spdx-headers.sh` and `git diff --check`: passed.
- `clang-tidy -p out/build/dev <file>` on the four changed C++ translation units:
  failed during frontend parsing of the macOS SDK (e.g. missing `__builtin_clzg`),
  as documented in `doc/dev/clang-tidy.md`. This is not clean semantic-lint
  evidence; Linux CI's actual `clang-tidy` job remains required.
- No local npm check: this module makes no web edits; web validation belongs to #193.

## Limits and review

Local PostgreSQL tests skip because `FMGR_TEST_POSTGRES_URL` is unset. No storage,
proto, auth, stream, dependency or CI changes are included. The parent #180 stays
open until both modules merge. Linux CI and lead review gate merging.
