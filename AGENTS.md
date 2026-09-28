# FreezerManager — Agent Guide

The single source of instructions for every coding agent working in this repo
(Claude Code loads it through `CLAUDE.md`; Codex, Hermes and others read it
directly). Humans: see `CONTRIBUTING.md`; everything here applies to you too.

FreezerManager is a self-hostable freezer / biospecimen manager: a C++20 server
(`freezerd`: gRPC + REST/SSE gateway, SQLite or PostgreSQL), a CLI
(`freezerctl`) and a Qt 6 desktop client. Product spec: `doc/PRD.md`.

## 1. Start of every session

1. **Know your role.** Your prompt names you `lead` or `worker-N` (N = 1–3). If
   it doesn't, ask the owner. Never pick a role or a task yourself.
2. **Read the coordination board:** `gh issue view 28 --comments`. If the owner
   has posted `STOP` there, do not start anything.
3. **Worker:** find your assignment with
   `gh issue list --label agent:worker-N --label status:in-progress`, then read it
   with `gh issue view <n> --comments`. No assignment means no work: say so and
   wait.
4. **Set up your shell** in your worktree: `AGENT_SLOT=N source scripts/agent/env.sh`
   (the lead uses slot 0 in the main checkout).

## 2. Project map

| Path | What lives there |
|---|---|
| `src/core/` | Pure domain types and validation. No I/O, no database. |
| `src/storage/` | `IStorageBackend`, typed query DSL, `sqlite/` and `postgres/` backends. Migrations are defined in `sqlite/SqliteBackend.cc` and `postgres/PostgresBackend.cc`. |
| `src/auth/`, `src/rpc/` | `IAuthProvider` + `LocalAuthProvider` (Argon2id, TOTP); `AuthMiddleware` (RBAC gate, RPC → permission registry, rate limiter). |
| `src/audit/` | Hash-chained audit log: canonical JSON, chain verifier. |
| `src/kms/`, `src/crypto/` | `IKmsProvider` + env/keyring KMS; PHI field cipher, backup file cipher. |
| `src/backup/` | Encrypted SQLite/Postgres backups, retention, restore drill. |
| `src/server/` | `freezerd`: gRPC service implementations, `FreezerServer`, backup scheduler. |
| `src/rest/` | Drogon REST/JSON gateway over the in-process gRPC channel, SSE bridge. |
| `src/obs/` | JSON logging, metrics, health. |
| `src/cli/` | `freezerctl` and the CSV import/export cores. |
| `src/qt/` | Qt 6 desktop client (Google style, own `.clang-format`/`.clang-tidy`). Built only if Qt6 is found. |
| `src/web/`, `src/py/` | Placeholders for the planned SPA and Python client. |
| `proto/fmgr/v1/` | gRPC API, the source of truth. Never edit generated code. |
| `tests/` | `unit/`, `property/`, `integration/` (label `grpc_integration`), `e2e/`, `fuzz/`, `backend_conformance/`. |
| `doc/` | `PRD.md` (spec), `SLO.md`, `UPGRADE.md`, reviews/audits, `handoffs/` (history), `dev/` (tooling notes). |
| `TODO.md` | Roadmap keyed by stable IDs (`F7`, `C-10`, …). Only the lead edits it. |
| `scripts/` | Demo seeding, `agent/` worktree + environment helpers. `tools/`: SPDX check, test runner, TSan suppressions. |

## 3. Coordination

### Roles

- **Owner** (the human): sets priorities, merges PRs, can pause everything by
  posting `**[owner] STOP**` on the board issue. Their word overrides this file.
- **Lead** (exactly one agent, `lead`): turns `TODO.md` items into issues, plans
  work in waves, assigns, answers questions, reviews PRs, and is the only writer
  of `TODO.md` and of the board issue body. The lead can take tasks itself only
  when no worker is available, and follows the worker rules while doing so.
- **Workers** (`worker-1` … `worker-3`): do exactly the issue they are assigned.
  They never self-assign, never edit `TODO.md`, never merge, and never touch
  another agent's branch or worktree.

### The board: GitHub Issues

All agents share one GitHub account, so ownership is shown by labels and comment
signatures, not by the assignee field.

- **Board issue #28** (pinned, *Agent coordination board*): the body is kept up
  to date by the lead and lists the current lead, active assignments (issue →
  agent → branch → slot), held locks, and the next free migration version.
  Comments carry announcements such as "main changed X, rebase".
- **One issue per task**, created from `.github/ISSUE_TEMPLATE/agent-task.md`,
  titled `[<TODO-ID>] <summary>`.
- **Labels:**

| Label | Meaning |
|---|---|
| `status:ready` → `status:in-progress` → `status:in-review` | Lifecycle; `status:blocked` when waiting on someone. |
| `agent:lead`, `agent:worker-1..3` | Who owns the task. Exactly one per in-progress issue. |
| `area:<module>` | `core`, `storage`, `auth`, `kms`, `audit`, `backup`, `rpc`, `rest`, `server`, `cli`, `qt`, `ci`, `docs`. |
| `lock:migration`, `lock:proto`, `lock:deps`, `lock:ci` | Hot resource held (see below). |
| `priority:P0` / `P1` / `P2` | Set by the owner or lead. |

### Lifecycle of a task

1. **Lead** creates the issue (goal, TODO ID, acceptance criteria, likely files,
   locks, dependencies) and labels it `status:ready`.
2. **Lead assigns:** adds `agent:worker-N` + `status:in-progress`, removes
   `status:ready`, posts an `ASSIGN` comment, and updates the board.
3. **Worker** creates the worktree with `scripts/agent/worktree.sh <issue> <slug> [type]`,
   writes failing tests first, commits early, and opens a **draft PR** containing
   `Closes #<issue>`.
4. **Worker finishes:** rebases on `origin/main`, runs the full `ctest --preset dev`,
   adds a handoff note under `doc/handoffs/`, marks the PR ready, relabels the
   issue `status:in-review`, and posts `HANDOFF`.
5. **Lead reviews** against the acceptance criteria and posts `DECISION`
   (either "changes needed: …" or "ready for owner merge").
6. **Owner squash-merges.** The lead then ticks `TODO.md` (in a batched
   `docs(todo): …` PR), releases locks on the board, and tells affected workers
   to rebase. The worker removes its worktree:
   `git worktree remove .worktrees/<issue>-<slug> && git branch -D <branch>`.

**Lead planning rules:** at most 3 workers active at once. Don't run two tasks
in the same wave that are likely to edit the same files. Each `lock:*` label is
on at most one in-progress issue. Tasks whose dependencies haven't merged stay
`status:ready`.

**Reassigning:** if a worker is silent on its issue for a working day, the lead
may post `**[lead] DECISION** reassigning to worker-M`. A worker that sees its
task reassigned stops, pushes its branch as-is, and comments where it left off.

### Talking to each other

Every comment on an issue or PR starts with the author and a tag:

```
**[worker-2] STATUS** red test for sample-watch ordering pushed (a1b2c3d); implementing next.
**[lead] ASSIGN** worker-2 · branch feat/42-sse-sample-watch · worktree .worktrees/42-sse-sample-watch · slot 2 · locks: none
```

Tags: `ASSIGN`, `STATUS`, `QUESTION`, `BLOCKED`, `DECISION`, `HANDOFF`.

- **Workers read their issue comments** (`gh issue view <n> --comments`) before
  starting, after each commit, before opening or readying the PR, and whenever
  they are blocked. They read the board issue at the start and before readying
  the PR.
- **Ask, don't guess:** if the task is ambiguous, or doing it right needs a file
  outside the issue's scope, post a `QUESTION` and work on something unblocked
  meanwhile. Scope creep becomes a new issue (lead creates it), not extra
  commits.
- **The lead's loop:** `gh issue list --label status:blocked`, then
  `--label status:in-progress`, then `gh pr list`. Answer `QUESTION`/`BLOCKED`
  first, then review, then plan the next wave.
- Tool-native messaging (e.g. Claude Code `SendMessage`) is fine for a quick
  ping, but anything that decides something must also be on the issue. The
  issue is the record.

### Hot resources (locks)

These conflict even when two tasks touch different features. Hold the lock
label before changing them:

| Lock | Covers | Rule |
|---|---|---|
| `lock:migration` | New schema migrations in `SqliteBackend.cc` + `PostgresBackend.cc` | The lead reserves the version number in the issue (next free: see board). Both backends get the same number. |
| `lock:proto` | `proto/fmgr/v1/*.proto` | Breaking changes need a new package version (`v2`); additive changes still need the lock. |
| `lock:deps` | `conanfile.py`, `conan.lock` | Only the holder may run `conan install --build=missing` (it writes the shared Conan cache). |
| `lock:ci` | `.github/workflows/`, `CMakePresets.json`, top-level `CMakeLists.txt` | Keep build-graph changes serialized. |

`TODO.md`, `README.md` (roadmap table) and the board issue body belong to the
lead. Workers report changes to them in their `HANDOFF` comment.

### Running several agents on one machine

`scripts/agent/worktree.sh` and `scripts/agent/env.sh` implement these rules. Use
them rather than a tool-specific worktree feature, so every agent gets the same
layout.

- **One git worktree per task** under `.worktrees/<issue>-<slug>` (gitignored),
  branch `<type>/<issue>-<slug>`, cut from `origin/main`. Never work in the
  main checkout unless you are the lead doing lead chores.
- **Shared toolchain:** `env.sh` points `CONAN_HOME` at the main checkout's
  `.conan/` and puts its `.venv/bin` on `PATH`. The worktree script runs
  `conan install --build=never`, so dependencies are never compiled twice or
  concurrently. If a package is missing, ask the lead.
- **Memory:** builds default to `-j3` (`CMAKE_BUILD_PARALLEL_LEVEL`); don't pass
  a higher `-j`. At most one agent runs a full-tree clang-tidy sweep at a time
  (`doc/dev/clang-tidy.md`).
- **Tests:** `TMPDIR` is private to the worktree (`out/tmp`), because the
  integration tests create SQLite files with fixed-pattern names.
  `CTEST_PARALLEL_LEVEL=1` because they also collide inside one `ctest -j` run.
- **Ports and data:** `freezerd` started by hand uses `FMGR_LISTEN` /
  `FMGR_REST_LISTEN` on per-slot loopback ports (gRPC `50051+10·N`, REST
  `18080+10·N`) and `FMGR_DB_PATH` inside the worktree. Never bind `0.0.0.0` or
  use the default 8080/50051.

## 4. Build and test

Prerequisites: CMake ≥ 3.25, Conan 2, Ninja, a C++20 compiler (CI: GCC 13 and
Clang 17 on Ubuntu 24.04). On the owner's Mac these tools live in the main
checkout's `.venv/`, and the dependency cache in `.conan/`. `env.sh` wires both
up. First-time setup, main checkout only:

```sh
python3 -m venv .venv && .venv/bin/pip install conan cmake ninja
export PATH=$PWD/.venv/bin:$PATH CONAN_HOME=$PWD/.conan
conan profile detect --force
conan install . --lockfile=conan.lock --output-folder=out/conan/dev \
    --build=missing -s build_type=Debug -s compiler.cppstd=20
```

Everyday, in a worktree:

```sh
AGENT_SLOT=N source scripts/agent/env.sh
cmake --build --preset dev
ctest --preset dev                  # full suite, ~1400 tests, ~70 s
ctest --preset dev -R '^LabService' # focused
```

- **Sanitizers** (`asan`, `ubsan`, `tsan` presets) are required when touching
  memory, concurrency, storage or parser code. Each preset needs its own
  `conan install --output-folder=out/conan/<preset>`. Under asan/tsan, exclude
  `-LE 'grpc_integration|e2e'`, as CI does.
- **PostgreSQL tests** skip unless `FMGR_TEST_POSTGRES_URL` is set (CI uses a
  `postgres:16` service). Each test process gets its own schema, so a shared
  server is safe.
- **Qt client** builds only when Qt6 is found; it isn't installed on the
  owner's Mac.
- **Formatting / lint:** CI enforces `clang-format-17 --dry-run --Werror`, the
  SPDX check (`tools/check-spdx-headers.sh`) and `run-clang-tidy-17`. **Never
  run clang-tidy above `-j 2`** (it OOMs; see `doc/dev/clang-tidy.md`). If a
  tool isn't installed locally, say so in the PR rather than skipping silently.

## 5. Engineering rules

- **Stay behind interfaces:** higher-level code must not depend on a concrete
  backend, auth provider, KMS or hardware device (PRD §5, §7, §8, §12).
- **Raw SQL only inside a `*Backend`** implementation. Everything else uses the
  typed query DSL.
- **Every mutation writes its audit row in the same transaction.** A new
  mutating RPC without audit coverage is a blocking review comment.
- **Every new RPC is registered** in the `AuthMiddleware` RPC → permission
  registry. `ServerIntegrationTest.RpcRegistryCoversAllExpectedMethods`
  enforces this.
- **PHI never appears** in logs, error messages, fixtures, screenshots, PR text
  or unencrypted backups.
- **SQLite is single-writer:** return `Unavailable` on contention and let
  callers retry. Don't serialize writers behind an app-level mutex.
- **Keep domain code pure:** `src/core/` has no I/O or database dependencies.
  Prefer descriptive type names (`IStorageBackend`, `LabId`, `SampleStatus`).
- Every new C++ or Python file starts with
  `// SPDX-License-Identifier: AGPL-3.0-or-later` (`#` for Python and shell).

## 6. Testing rules

- **TDD is mandatory:** write the failing test first, and link the test files in
  the PR.
- Test names state the behavior under test, e.g.
  `SqliteBackendRejectsDuplicateBoxPosition`.
- Backends must pass `tests/backend_conformance/`.
- **Fakes must be able to fail:** every fake gRPC service supports per-RPC
  error injection (`grpc::StatusCode fail_<method> = grpc::StatusCode::OK`). A
  fake that only returns `OK` hides every error branch (see
  `doc/TEST_COVERAGE_AUDIT_2026-07-01.md`).
- Report results faithfully: paste the exact commands and pass/fail counts into
  the PR. Never mark a failing or skipped check as done.

## 7. Branches, commits and PRs

- Branch: `<type>/<issue>-<slug>`, from `origin/main`. PRs target `main`.
- Commits: Conventional Commits (`feat(rest): …`, `fix: …`, `docs: …`), each
  signed off with `git commit -s`. **Do not add `Co-Authored-By:` trailers.**
  Commits are authored by the human committer only.
- PR body contains `Closes #<issue>`, what changed and why, the exact test
  commands with results, and one line noting AI assistance and which agent did
  it (e.g. "Implemented by worker-2 (Claude Code)").
- Keep PRs to one issue. Rebase onto `origin/main` before marking ready. Only
  force-push your own branch, and only with `--force-with-lease`.
- **Never:** push to `main`, merge a PR, rewrite someone else's branch, delete
  issues, or change labels on issues you don't own (the lead may).

## 8. Where things live

| Need | Look in |
|---|---|
| What to build, and why | `doc/PRD.md`, then the item in `TODO.md` |
| Who is doing what, right now | Board issue #28, and issues labelled `status:in-progress` |
| How a past slice was done | `doc/handoffs/` (one file per slice) |
| Known security/quality findings | `TODO.md` top sections, `doc/review-*.md`, `doc/security-audit-*.md` |
| Operations | `doc/SLO.md`, `doc/UPGRADE.md` |
