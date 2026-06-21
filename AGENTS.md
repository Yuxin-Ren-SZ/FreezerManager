# Repository Guidelines

## Project Structure & Module Organization

FreezerManager is in pre-alpha design phase. The current root contains project
docs (`README.md`, `doc/PRD.md`, `TODO.md`), licensing files (`LICENSE`,
`LICENSING.md`, `NOTICE`, `CLA.md`), and placeholder top-level directories:
`src/`, `tests/`, and `data/`. Planned source layout is C++20-first: `src/core/`
for pure domain types, `src/storage/` for `IStorageBackend` and backend
implementations, `src/auth/`, `src/kms/`, `src/audit/`, `src/rpc/`,
`src/server/`, `src/cli/`, `src/qt/`, `src/web/`, and `src/py/`. Tests should
live under `tests/unit/`, `tests/property/`, `tests/integration/`,
`tests/fuzz/`, `tests/e2e/`, and `tests/backend_conformance/`.

## Build, Test, and Development Commands

Install CMake 3.25+, Conan 2, Ninja, clang-format, and clang-tidy. Configure
dependencies first, then build and test with CMake presets:

```sh
conan profile detect --force
conan install . --lockfile=conan.lock --output-folder=out/conan/dev --build=missing -s build_type=Debug
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

Use `cmake --preset asan`, `ubsan`, or `tsan` when touching memory,
concurrency, storage, or parser code. CI also runs formatting, clang-tidy,
SPDX-header checks, and advisory coverage tooling.

Run clang-tidy with the parallel runner that ships with LLVM rather than a bare
`clang-tidy` loop — it is many times faster across the tree. Cap parallelism at
2: each clang-tidy process can consume 2–4 GB with the full check set enabled,
and `-j $(nproc)` on an 8-core machine easily exceeds available memory.

```sh
run-clang-tidy-17 -p out/build/dev -j 2        # whole compile DB
run-clang-tidy-17 -p out/build/dev -j 2 src/storage/sqlite/  # a subset
```

Only project sources are in `compile_commands.json` (Conan deps are prebuilt),
so no file filter is needed for a full sweep. Fall back to
`clang-tidy-17 -p out/build/dev <file>` only when the parallel runner is absent.

## Clang-tidy Strategy

**Hard rule: never exceed `-j 2` for `run-clang-tidy`.** This is not a
performance tuning knob — it is an OOM prevention measure. The rationale applies
equally to local machines, CI runners, and any future build infrastructure.

### Why `-j 2`

- The project's `.clang-tidy` enables six broad check categories:
  `bugprone-*`, `clang-analyzer-*` (path-sensitive analysis, the biggest memory
  consumer), `modernize-*`, `performance-*`, `portability-*`, and
  `readability-*`.
- A single `clang-tidy` process with this check set routinely consumes
  **2–4 GB** of resident memory.
- `-j $(nproc)` on an 8-core machine spawns 8 processes (peak **16–32 GB**);
  on a 4-core GitHub Actions runner it spawns 4 (peak **8–16 GB**). Both exceed
  typical available memory and trigger the OOM killer.
- The same constraint already caps `cmake --build` at `-j 2` in CI (see
  `.github/workflows/build.yml`, build step comment).

### What to do when clang-tidy is slow

- **Do not** increase `-j` beyond 2.
- If the CI step times out, split the run by subdirectory (each still at
  `-j 2`):
  ```sh
  run-clang-tidy-17 -p out/build/dev -j 2 src/core/
  run-clang-tidy-17 -p out/build/dev -j 2 src/storage/
  run-clang-tidy-17 -p out/build/dev -j 2 tests/
  ```
- For local iteration, run on a single file or subdirectory rather than the
  full tree.

### What to do if someone proposes raising `-j`

- Point them to this section.
- Ask: has the check set been reduced? Has memory profiling been done?
  If neither, the answer is no.

## Coding Style & Naming Conventions

Use C++20 for core services. Add
`// SPDX-License-Identifier: AGPL-3.0-or-later` to every new source file.
Keep domain code free of I/O and database dependencies. Use abstract interfaces
for storage, auth, KMS, and hardware integrations. Raw SQL belongs only inside
concrete `*Backend` implementations. Prefer descriptive type names such as
`IStorageBackend`, `SqliteBackend`, `LabId`, and `SampleStatus`. Run
clang-format and clang-tidy before opening a PR.

## Testing Guidelines

TDD is mandatory for implementation tasks: write or update failing tests before
production code. Planned frameworks include GoogleTest for unit and conformance
tests, plus property and fuzz tests where invariants matter. Backend
implementations must pass `tests/backend_conformance/`. Test names should state
the behavior under test, for example `SqliteBackendRejectsDuplicateBoxPosition`.

## Commit & Pull Request Guidelines

Recent history uses Conventional Commits (`docs: ...`, `ci: ...`). Keep that
style for new commits and sign every commit with `git commit -s` so it includes
`Signed-off-by: Your Name <email>`. PRs should target `main`, describe the
behavior change, link relevant TODO or issue context, and include the exact test
plan. For UI-facing changes, include screenshots. Never log PHI or include PHI
in fixtures, error messages, or PR examples.
