# Clang-tidy

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

### Several agents on one machine

The `-j 2` cap is per process tree. When several agents share a machine (see
`AGENTS.md` → Coordination), only one agent at a time should run a full-tree
sweep; everyone else lints only the files they changed.
