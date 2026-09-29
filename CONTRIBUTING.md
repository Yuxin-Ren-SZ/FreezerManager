# Contributing to FreezerManager

FreezerManager is pre-alpha. Contributions should keep the repository ready for
test-driven C++20 implementation without adding production behavior ahead of the
design in `doc/PRD.md`.

Coding agents, and humans working alongside them, follow `AGENTS.md`: task
board, coordination protocol, and worktree rules.

## Workflow

- Open pull requests against `main`.
- Keep PRs small enough to review independently.
- Use Conventional Commits, for example `docs: add operator notes` or
  `ci: add clang-tidy check`.
- Sign every commit with `git commit -s`; PRs with commits missing a
  `Signed-off-by:` line cannot merge.
- Squash-merge is the expected merge strategy.

## Local Setup

Install CMake 3.25 or newer, Conan 2, Ninja, a C++20 compiler, clang-format,
and clang-tidy. Then run:

```sh
conan profile detect --force
conan install . --lockfile=conan.lock --output-folder=out/conan/dev --build=missing -s build_type=Debug -c tools.cmake.cmaketoolchain:user_presets=
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

Use sanitizer presets when touching memory, concurrency, storage, or parsing
code:

```sh
conan install . --lockfile=conan.lock --output-folder=out/conan/asan --build=missing -s build_type=Debug -c tools.cmake.cmaketoolchain:user_presets=
cmake --preset asan
conan install . --lockfile=conan.lock --output-folder=out/conan/ubsan --build=missing -s build_type=Debug -c tools.cmake.cmaketoolchain:user_presets=
cmake --preset ubsan
conan install . --lockfile=conan.lock --output-folder=out/conan/tsan --build=missing -s build_type=Debug -c tools.cmake.cmaketoolchain:user_presets=
cmake --preset tsan
```

Each `conan install` above needs a second output folder, which is why every
command ends with `-c tools.cmake.cmaketoolchain:user_presets=`: otherwise Conan
merges all the folders into one root `CMakeUserPresets.json`, every Debug folder
contributes a preset called `conan-debug`, and `cmake --preset` aborts with
`Duplicate preset: "conan-debug"`. That file is not used by this project —
`CMakePresets.json` points at the Conan toolchain directly — so the flag simply
stops Conan from writing it. Agents do not run these commands by hand; they use
`scripts/agent/conan-install.sh <preset>` (AGENTS.md §4).

## Tests

TDD is mandatory for implementation work. Add or update tests before production
code, link the relevant test files in the PR description, and include the exact
commands you ran. Backend implementations must pass `tests/backend_conformance/`
once that suite exists.

## Style and Safety

Run clang-format and clang-tidy before requesting review. Add
`// SPDX-License-Identifier: AGPL-3.0-or-later` to every new C++ source/header
file and `# SPDX-License-Identifier: AGPL-3.0-or-later` to every new Python
file. Do not log PHI or place PHI in fixtures, screenshots, sample data, error
messages, or PR examples.
