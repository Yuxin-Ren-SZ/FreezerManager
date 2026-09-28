# Handoff note — 2026-06-13, M1 sample CSV import (transactional + dry-run)

Implemented `freezerctl sample import`, closing the CSV-import half of M1 for the
Sample entity (PRD §13). Export already existed; this adds the read path.

**New files:**
- `src/cli/CsvReader.{h,cc}` — RFC 4180 reader (the counterpart to `CsvWriter`).
  Honours double-quoted fields, doubled embedded quotes, embedded commas/CR/LF,
  CRLF or bare-LF separators; skips `#`-prefixed comment lines so a file produced
  by `sample export` (chain-of-custody header block) round-trips unchanged. No
  spurious trailing empty record. `CsvParseError` on an unterminated quote. Kept
  domain-free for fuzzing (PRD §15 lists the CSV importer as a fuzz target).
- `src/cli/SampleImport.{h,cc}` — pure CSV-row → `core::Sample` mapping and
  validation (`build_import`). No I/O, no DB, clock-injected: required fields,
  UUID/enum parse, box/position pairing (mirrors the samples CHECK), well-formed
  `custom_fields_json`, volume/mass value+unit pairing, and intra-file duplicate
  `(box_id, position_label)` detection. Server-managed columns (id, lab_id,
  status, created_*, last_modified_*, phi_fields_enc_json) are ignored if present,
  so a file cannot forge ownership/authorship or smuggle a row into another lab.
  Emits a per-row `ImportReport`.

**Changed:**
- `src/cli/SampleCommands.{h,cc}` — `run_sample_import()`. Structural gate first
  (any row error ⇒ nothing written, exit 1). `--dry-run`: each row inserted in
  its own transaction that is rolled back (never committed), so DB-level checks
  (item-type liveness, box existence, size-class) report per-row without a poison
  cascade. Normal mode: all rows inserted in a single transaction and committed
  all-or-nothing. RLS `current_lab_ids` injected on every transaction.
- `src/cli/CliApp.cc` — `sample import [--dry-run] --lab <uuid> --actor <uuid>
  <file|->` subcommand (reads stdin on `-`). `--actor` is the recorded importer.
- `src/cli/CMakeLists.txt` — adds `CsvReader.cc`, `SampleImport.cc`.

**Tests (`tests/unit/cli_test.cpp`, all in the existing cli suite):**
- CsvReader: simple rows, bare-LF / no-trailing-newline, comment-line skip,
  quoted comma/newline/doubled-quote, no trailing empty record, unterminated-quote
  throw.
- SampleImport (pure): valid mapping with server-controlled fields, ignores
  server-managed columns, missing-name / bad-UUID / box-without-position /
  intra-file-duplicate-position / bad-custom-JSON row errors, missing-required-
  header and empty-document header errors.
- run_sample_import (SQLite + Postgres-parameterized): persists transactionally,
  dry-run writes nothing, rejects unknown item-type at the DB layer. Plus an
  argv-level `sample import` end-to-end test reading from a file.

Verification:
- `cmake --build --preset dev` — clean.
- `ctest --preset dev -j1` — 889/889 passed (Postgres cli/conformance tests skip
  without `FMGR_TEST_POSTGRES_URL`). NOTE: under `-j$(nproc)` the pre-existing
  `grpc_integration` tests collide on fixed ports/paths and report failures; they
  pass in isolation and serially. Unrelated to this slice.
- `clang-format --dry-run --Werror` on all new/changed files — clean.
- `run-clang-tidy-17 -p out/build/dev` on the new/changed `.cc` — clean.
- `tools/check-spdx-headers.sh` — clean. `git diff --check` — clean.

Handoff notes:
- Dry-run cannot detect a `(box_id, position_label)` collision against *already
  committed* rows (the partial unique index fires at commit, which dry-run never
  reaches); intra-file collisions are caught structurally. Real-mode import
  surfaces a committed-row collision as a `UniqueViolation` that aborts the whole
  batch. Documented limitation, acceptable for v1.
- Import currently covers Sample only. PRD §13 also lists boxes, item types,
  custom-field definitions, and (admin) users — each is a follow-up that can reuse
  `CsvReader` and the `build_import`/report pattern.
- Remaining M1 CLI nouns (freezer/box/item-type create/list/inspect) are still
  outstanding; see Section F6 / L9 for the command-tree conventions.
