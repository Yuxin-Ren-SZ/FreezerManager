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
`clang-tidy` loop — it is many times faster across the tree:

```sh
run-clang-tidy-17 -p out/build/dev -j "$(nproc)"        # whole compile DB
run-clang-tidy-17 -p out/build/dev -j "$(nproc)" src/storage/sqlite/  # a subset
```

Only project sources are in `compile_commands.json` (Conan deps are prebuilt),
so no file filter is needed for a full sweep. Fall back to
`clang-tidy-17 -p out/build/dev <file>` only when the parallel runner is absent.

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
