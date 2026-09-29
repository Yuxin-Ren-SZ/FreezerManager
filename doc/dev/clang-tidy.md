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
- In CI, the result cache (see "In CI" below) is what keeps the job fast. If it
  still times out, split the run by subdirectory (each still at `-j 2`):
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

## In CI

The `clang-tidy` job in `.github/workflows/build.yml` lints the whole tree once
per run: clang-17, the `dev` preset, `-j 2`. Before #56 it ran inside both `dev`
jobs, after their build and tests. It needs only a CMake configure and the
`freezermanager_proto` target (for the generated `fmgr/v1/*.pb.h`), so it runs
alongside the build matrix. A PR that changes only `src/web/**`, `doc/**`,
Markdown or issue templates skips it.

**Result cache.** The job wraps clang-tidy in
[ctcache](https://github.com/matus-chochlik/ctcache), pinned by commit and
SHA-256 in the workflow. For each translation unit, ctcache hashes:

- the preprocessed source, **comments included** (`CTCACHE_KEEP_COMMENTS=1`), so
  a change to any header the TU includes, or the removal of a `NOLINT`,
  invalidates the entry;
- the `--dump-config` output for that file, so a `.clang-tidy` change
  invalidates it too;
- the clang-tidy arguments.

Conan names every package build folder uniquely (`~/.conan2/p/b/grpc<hash>/p`),
and that path appears in the `-isystem` flags and the preprocessor's line
markers. `CTCACHE_STRIP` and `CTCACHE_STRIP_SRC` remove that one segment, so
rebuilding identical dependencies leaves the hashes unchanged, while a header
whose content changed still invalidates every TU that includes it.

It skips clang-tidy only when that hash is already stored, and it stores a hash
only when clang-tidy exited 0 with no output. A file with findings is therefore
re-checked on every run and can never be replayed as clean.

The cache directory is saved on every run (it is a few bytes per clean TU) and
restored from the newest entry for the same clang-tidy version; a new
clang-tidy starts from an empty cache. The `clang-tidy cache statistics` step
prints the hit and miss counts.

If you ever suspect a stale result, set `CTCACHE_DISABLE: "1"` in the job's
`env` for one run: ctcache then calls clang-tidy directly.

**Trying ctcache locally:** the `clang-tidy` wheel in `.venv` is a Python shim
that prints `Resource filename: …` to stdout. ctcache treats any stdout as a
finding and never caches, so point the wrapper at the real binary under
`.venv/lib/python3*/site-packages/clang_tidy/data/bin/clang-tidy`.
