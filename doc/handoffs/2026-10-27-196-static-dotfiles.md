# #196 — Refuse hidden SPA assets

PR: [#210](https://github.com/Yuxin-Ren-SZ/FreezerManager/pull/210). Worker-2 (Codex), slot 2.

## Behavior and scope

The SPA handler refuses every decoded request component beginning with a dot,
including `.well-known`, at any depth and even when the path does not exist.
Canonical targets must also have no hidden component below the configured
`web_root`. That closes visible symlinks to hidden files/directories and an
`index.html` symlink used by SPA fallback. Root containment, traversal and
backslash/NUL refusal remain in place. The root itself and absolute ancestors
such as `.worktrees` remain allowed.

Pure path tests cover direct hidden names, missing hidden paths, in-root file
and directory symlink aliases, hidden index fallback, ordinary interior dots,
and hidden root/ancestor controls. HTTP tests send raw and percent-encoded
paths byte for byte (the existing harness disables client path encoding),
including encoded dots and separators, and verify refusal without secret or SPA
bytes. HTTP refusal preserves CSP, nosniff, referrer and no-cache headers.
Existing hashed asset, SPA route, traversal, escaping-symlink, TLS and disabled
static-serving cases remain part of validation. Bare `/api` is unchanged (#186).
No dependency/schema/proto/CI, TODO/README or owner architecture files changed.

## TDD evidence

Red commit `a672c67` preceded the implementation `2cb57e5`.

```sh
cmake --preset dev
cmake --build --preset dev --target freezermanager_rest_unit_tests freezermanager_rest_static_assets_integration_tests
ctest --preset dev -R '^RestStaticAssets(Resolve|\.)'
```

Before the fix: 31 tests, 26 passed / 5 failed, exit 8. The failures were:

- `RestStaticAssetsResolve.RejectsHiddenPathSegmentsIncludingMissingPaths`
- `RestStaticAssetsResolve.RejectsVisibleSymlinksToHiddenTargetsInsideTheWebRoot`
- `RestStaticAssetsResolve.RejectsIndexFallbackToAHiddenTargetInsideTheWebRoot`
- `RestStaticAssets.RefusesRawAndPercentEncodedHiddenPathSegments`
- `RestStaticAssets.RefusesVisibleSymlinksToHiddenTargetsInsideTheWebRoot`

After the fix, the same focused build/test commands passed 31/31, no skips.
Tests are in `tests/unit/static_assets_test.cpp` and
`tests/integration/rest_static_assets_integration_test.cpp`.

## Local validation

All commands ran in `.worktrees/196-static-dotfiles` after
`AGENT_SLOT=2 source scripts/agent/env.sh`: build parallelism 3, CTest serial,
private worktree `TMPDIR`. Platform: macOS / AppleClang. Sanitizer dependencies
were installed using `scripts/agent/conan-install.sh` with its cached
`--build=never`; generated presets were not edited.

Full dev:

```sh
cmake --build --preset dev
ctest --preset dev
```

Build passed. Full CTest: 1,789 registered, 1,557 passed / 232 PostgreSQL
skipped / 0 failed (exit 0; 203.54 s). No TLS allocator abort was observed in
this dev run.

Focused static-asset sanitizer builds/checks (not the full sanitizer suites):

```sh
scripts/agent/conan-install.sh asan
scripts/agent/conan-install.sh ubsan
scripts/agent/conan-install.sh tsan
cmake --preset asan
cmake --preset ubsan
cmake --preset tsan
cmake --build --preset asan --target freezermanager_rest_unit_tests freezermanager_rest_static_assets_integration_tests freezermanager_rest_static_assets_tls_integration_tests freezermanager_rest_static_assets_disabled_integration_tests
ctest --preset asan -R '^RestStaticAssets' -LE 'grpc_integration|e2e'
cmake --build --preset ubsan --target freezermanager_rest_unit_tests freezermanager_rest_static_assets_integration_tests freezermanager_rest_static_assets_tls_integration_tests freezermanager_rest_static_assets_disabled_integration_tests
ctest --preset ubsan -R '^RestStaticAssets'
cmake --build --preset tsan --target freezermanager_rest_unit_tests
ctest --preset tsan -R '^RestStaticAssets' -LE 'grpc_integration|e2e'
```

- ASan: build passed; 46/46 passed, no skips. Existing Apple-only CMake test
  properties set `ASAN_OPTIONS=detect_container_overflow=0` for the three HTTP
  binaries because prebuilt Drogon is uninstrumented. macOS does not establish
  Linux LeakSanitizer coverage.
- UBSan: build passed; 46/46 passed, no skips.
- TSan: build passed; 22/22 passed, no skips. Existing CMake configuration does
  not register the three Drogon static HTTP/TLS/disabled binaries under TSan;
  only the 22 pure path/config tests are expected here. This is an exclusion,
  not evidence of HTTP shutdown race coverage. The preset also retains the
  existing `tools/tsan.suppressions` configuration.

```sh
clang-format --dry-run --Werror src/rest/StaticAssets.cc src/rest/StaticAssets.h tests/unit/static_assets_test.cpp tests/integration/rest_static_assets_integration_test.cpp
tools/check-spdx-headers.sh
git diff --check
clang-tidy -p out/build/dev src/rest/StaticAssets.cc
clang-tidy -p out/build/dev tests/unit/static_assets_test.cpp
clang-tidy -p out/build/dev tests/integration/rest_static_assets_integration_test.cpp
```

Format (17.0.6), SPDX and diff whitespace checks passed. Each local clang-tidy
attempt exited 1 because LLVM 17 cannot parse the installed macOS SDK libc++
headers (`__countr_zero`, `__countl_zero`, type-trait builtins; too many errors).
These attempts give no lint clearance. See `doc/dev/clang-tidy.md`.

## Review and merge limits

Actual Linux `clang-tidy` and all applicable matching-head compiler/sanitizer
CI must pass before lead merge. Local focused sanitizer results do not replace
that matrix. P1 #207 (the unrelated TLS allocator abort) is not worked around or
suppressed here; any recurrence must stay visible and be reported to the lead.
No deployment gate is waived. PostgreSQL requires `FMGR_TEST_POSTGRES_URL`,
which is unset locally. CMake reported Qt6 not found and skipped the desktop
client, so no Qt coverage is claimed.
