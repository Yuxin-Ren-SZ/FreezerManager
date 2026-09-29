# Handoff note — 2026-09-28, tooling Conan preset collision in agent worktrees (#36, worker-2)

`conan install --output-folder=out/conan/<preset>` used to append that folder to a
single repository-root `CMakeUserPresets.json`. Conan names its presets after the
build type, so every Debug folder (dev/asan/ubsan/tsan/coverage) contributes a
preset called `conan-debug`; after a second install the aggregate defined that
name twice and the next `cmake --preset asan` aborted with
`CMake Error: Duplicate preset: "conan-debug"`. Every agent that needed a
sanitizer preset had to rediscover the workaround — hand-trimming the `include`
list — before it could run the tests the task required.

**Changed:**

- `scripts/agent/conan-install.sh` (new): installs one preset's Conan
  dependencies into `out/conan/<preset>`, always with
  `-c tools.cmake.cmaketoolchain:user_presets=` so Conan never writes the
  aggregate, and with `--build=never` so the shared cache stays the `lock:deps`
  holder's. It also drops a stale aggregate left behind by an older install, but
  only when the file carries Conan's own `vendor.conan` marker.
- `scripts/agent/worktree.sh`: the `dev` install now goes through that helper.
- `AGENTS.md` §4: the sanitizer bullet shows the helper instead of a raw
  `conan install`, and says not to hand-edit generated preset files.
- `CONTRIBUTING.md`: the human setup and sanitizer commands carry the same conf.
- `tools/run-tests.sh` already solved this for the local full-test runner
  (comment and `rm -f` at lines 50-56) — it was the precedent and is unchanged.

**Decisions:**

- Conan 2.32 has **no** per-folder preset namespace: `CMakeToolchain.presets_prefix`
  is hardcoded to `"conan"` and is not a conf, and there are no
  `tools.cmake.cmakepresets.*` confs at all (verified in the installed source:
  only `tools.cmake:cmake_program`, `configure_args`, `install_strip` and
  `ctest_args` exist). The available lever is
  `tools.cmake.cmaketoolchain:user_presets`; set to the empty string,
  `_IncludingPresets.generate` returns immediately and no aggregate is written.
  That is the same fix `tools/run-tests.sh` uses.
- Suppressing the file is safe because nothing reads it: `CMakePresets.json`
  points `CMAKE_TOOLCHAIN_FILE` straight at
  `out/conan/<preset>/conan_toolchain.cmake` and never inherits a `conan-*`
  preset. CI installs a single preset per job and only ever calls our own
  presets, so CI needs no change and `lock:ci` was not exercised.
- The stale-file cleanup is marker-guarded rather than an unconditional `rm -f`,
  so a hand-written `CMakeUserPresets.json` cannot be destroyed. When the file is
  not Conan's, the helper warns and continues.
- `worktree.sh` calls the helper through `$script_dir` (the tree the script runs
  from) rather than `$wt`: a new worktree is cut from `origin/main`, so it only
  contains the helper once this change has merged.
- README's developer-setup section still shows a raw `conan install`; it belongs
  to the lead, so it is flagged here instead of edited.

**Tests:** there is no unit-test surface for shell tooling, so the acceptance
criteria are end-to-end checks. All were run on this machine:

- **Fresh worktree** (`scripts/agent/worktree.sh 3699 fresh-verify feat`, removed
  afterwards): `conan-install.sh dev`, `… asan`, `… ubsan`, then
  `cmake --preset` in the order dev → asan → dev → ubsan → asan — all five
  configure successfully with **no hand-editing and no root
  `CMakeUserPresets.json` at any point**; `git status` stayed clean.
- In `.worktrees/36-asan-preset-namespace`: `conan-install.sh dev` →
  `cmake --preset dev` → `cmake --build --preset dev` (exit 0) →
  `ctest --preset dev -R 'SqliteBackendConformance'` → **10/10 passed**;
  `conan-install.sh asan` → `cmake --preset asan` →
  `cmake --build --preset asan` (exit 0) →
  `ctest --preset asan -R 'SqliteBackendConformance' -LE 'grpc_integration|e2e'`
  → **10/10 passed**; back to `cmake --preset dev` → **10/10 passed**.
- Self-heal: a recreated Conan aggregate is dropped and reported; a hand-written
  `CMakeUserPresets.json` is left in place with a warning.

**Known limitations / follow-ups:**

- README's setup block still lacks the conf (lead-owned, flagged above).
- Conan's per-folder `CMakePresets.json` files still define `conan-debug`; they
  are reachable only if someone explicitly includes two of them, which nothing
  in this repository does. A future Conan version with a preset-namespace conf
  could rename them, but the fix does not depend on it.
- The helper is the only dependency install agents should use; a new preset in
  `CMakePresets.json` needs no change here, since the preset name is an argument.
