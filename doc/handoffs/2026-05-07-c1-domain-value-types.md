# Handoff note — 2026-05-07, C1 domain value types

Implemented the Section C1 core value-type slice in `src/core/` with focused
unit coverage in `tests/unit/core_value_types_test.cpp`.

Delivered:

- `Uuid` parsing, canonical formatting, comparison, and JSON conversion.
- Strong typed IDs backed by `Uuid`, including `LabId`, `UserId`, `SampleId`,
  `BoxId`, and the other planned domain ID aliases.
- `Volume` and `Mass` value types with unit enums and same-unit arithmetic
  checks.
- UTC microsecond `Timestamp`.
- Domain enums for sample status, checkout action, role kind, and container
  kind with string and JSON conversion.
- CMake wiring for the core library and unit test executable.

Verification completed locally:

- `conan install . --lockfile=conan.lock --output-folder=out/conan/dev --build=missing -s build_type=Debug -s compiler.cppstd=20`
- `cmake --preset dev`
- `cmake --build --preset dev`
- `ctest --preset dev` — 8/8 tests passed.
- `clang-format --dry-run --Werror` on the new core/test files.
- `clang-tidy -p out/build/dev tests/unit/core_value_types_test.cpp`.
- `tools/check-spdx-headers.sh`.
- `git diff --check`.

Handoff notes:

- Conan's detected default profile used `gnu17`; keep passing
  `-s compiler.cppstd=20` or update the default profile before dependency
  installation because `libpqxx` requires C++20.
- `clang-tidy` emits many suppressed third-party warnings from Conan-provided
  dependencies; project code is clean with the repository header filter.
- The next implementation slice should start at C2: define the storage
  abstraction interfaces and typed query DSL before any backend-specific code.
