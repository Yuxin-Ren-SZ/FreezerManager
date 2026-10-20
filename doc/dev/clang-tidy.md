# Clang-tidy

CI's `clang-tidy` job is the gate, and it cannot be reproduced on the owner's
Mac; a local run is advisory. The `clang-format` gate is reproducible. Both have
turned a PR red while every test passed, so budget a CI round-trip for
clang-tidy rather than assuming a local run settles it.

## The two CI gates

### `Check formatting` — reproducible

CI's `lint` job installs `clang-format-17` and runs it over every C, C++ and
proto file. The runner's `clang-format-17` is 17.0.6, and the `.venv` wheel
matches, so the same command reproduces the gate locally:

```sh
clang-format --dry-run --Werror \
  $(git ls-files '*.c' '*.cc' '*.cpp' '*.cxx' '*.h' '*.hh' '*.hpp' \
    '*.hxx' '*.ipp' '*.proto')
```

### `clang-tidy` — the gate is CI-only

CI runs `run-clang-tidy-17 -p out/build/dev -j 2` over the whole compile DB,
with warnings as errors (`.clang-tidy` sets `WarningsAsErrors: '*'`). That
runner is **not in the main checkout's `.venv/bin`**, which has only
`clang-tidy`, `clang-format` and `git-clang-format`, so you cannot reproduce the
gate. The runner ships with LLVM and is many times faster than a bare
`clang-tidy` loop. Only project sources are in `compile_commands.json` (Conan
deps are prebuilt), so no file filter is needed for a full sweep.

Never run it above `-j 2` — see "Clang-tidy Strategy" below.

## Running it locally

You can and should still run the bare clang-tidy 17 binary per file on your own
changed translation units (`.venv/bin/clang-tidy` is LLVM 17.0.1; on Ubuntu the
package is `clang-tidy-17`):

```sh
clang-tidy -p out/build/dev <file>     # one changed TU at a time
```

Filter the output: treat a finding as yours only if it **(a)** lands on a line
you added or changed **and (b)** does not also fire on the unmodified
`origin/main` version of the same file. This pays — a local run has already
found a genuine `bugprone-easily-swappable-parameters` before pushing. But the
run is **advisory, not clearance**.

### It is not a superset

The compile database drives AppleClang's `/usr/bin/c++`, while the local
clang-tidy is LLVM 17.0.1. This machine's SDK libc++ uses builtins a newer clang
provides (`__builtin_clzg`, `__countr_zero`, `__GCC_DESTRUCTIVE_SIZE`), and the
tool cannot parse `protobuf/arena_align.h`, so the frontend emits internal
`clang-diagnostic-error`s and stops with **"too many errors emitted"**. With the
AST broken, semantic checks go silent — `bugprone-unchecked-optional-access` is
*enabled* and listed by `--list-checks`, yet reports nothing because the optional
model never resolved — while name-lookup checks (like
`convert-member-functions-to-static`) still fire.

**That asymmetry is the thing to remember:** a local run's silence means
*nothing* for semantic checks, and a local run's noise means *nothing* for the
artifacts. It is not a weaker version of CI; it is a different measurement. It
reports artifacts in untouched files that CI does not, and it misses findings CI
catches: a local run on #106's file reported zero
`bugprone-easily-swappable-parameters` while CI reported exactly that check on
the same file and branch. Local clang-tidy is necessary but not sufficient.

On some translation units it aborts before reaching project code: 48
`clang-diagnostic-error`s from the macOS SDK's libc++ left the tool stuck in the
headers. The output is then advisory only — apply the A/B filter above, and say
in the PR that it aborted rather than implying it was clean. Whether it gets
through depends on which headers a TU pulls in, so it varies by file, not by
machine. With the builtins shimmed it reports only pre-existing findings in
files `main` passes (`-to-static` fixture false positives, `src/obs/Health.h`,
`src/rpc/RateLimiter.h`) — nothing points at new lines.

### Its silence is not evidence

The method has a false-negative mode: when a check **does not fire locally at
all**, filtering cannot tell "not a finding" from "not observed", the method
returns nothing, and the run reads as clean.
`bugprone-easily-swappable-parameters` never fires locally;
`performance-move-const-arg` did not either, and CI caught a `std::move` of a
trivially-copyable `std::optional<core::Volume>` that way after a local run had
found nothing on the author's lines.

**So: never write "clang-tidy clean" in a PR based on a local run.** Say what you
ran and what it reported, and let CI be the gate. Reporting a clean local run as
clearance is the one thing that turns a useful tool into a false assurance.

### The check to handle proactively

Whenever you add a helper with **two adjacent parameters of the same type** — two
`const std::string&`, two ids — apply the repo's existing convention
*proactively* rather than waiting for CI:
`// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)` with a one-line
justification (`src/kms/KmsFactory.cc:20`, `src/auth/Totp.cc:27`, and
`get_sample` in `tests/integration/sample_service_integration_test.cpp`), or put
a differently-typed parameter between them (as `dispatch_key_rotate` and
`import_csv(token, bool dry_run, csv, resp)` do). This one check has cost at
least four separate CI round-trips.

For a C++ PR, clang-tidy is therefore a CI-only gate. Most such PRs need at
least one round-trip for it (#27, #35, #38, #53); the fix is usually small —
cognitive complexity, an identifier under three characters, adjacent same-type
parameters.

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
per run: clang-17, the `dev` preset, `-j 2`. Since #61 it is its own job, named
`clang-tidy`, alongside the build matrix; before that it ran twice, inside both
`dev` jobs. It needs only a CMake configure and the `freezermanager_proto`
target (for the generated `fmgr/v1/*.pb.h`), so it starts without waiting for a
full build. A PR that changes only `src/web/**`, `doc/**`, `*.md`, `.nvmrc` or
issue templates skips it; anything unrecognised, and every push to `main`, runs
it.

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
