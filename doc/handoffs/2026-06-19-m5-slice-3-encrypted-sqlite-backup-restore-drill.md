# Handoff note — 2026-06-19, M5 slice 3 (encrypted SQLite backup + restore-drill)

First Backup/DR slice (PRD §14). SQLite path end-to-end: an online hot copy,
stream-encrypted under a **separate** backup KEK (PRD §8), with restore and a
schedule-safe restore-drill verify. CLI-driven (mirrors `key rotate`); Postgres
backup and the in-server scheduler are explicit follow-ups.

**Separate backup key (`src/kms/`):**
- `EnvVarKms::from_env(active_var, prev_var)` and
  `OsKeyringKms::from_credentials_dir(dir, basename)` /
  `from_systemd_credentials(basename)` overloads let an independent key load
  through the same keyring loaders. The no-arg/default forms still read
  `FMGR_MASTER_KEK` / `master_kek`.
- `kms::make_backup_kms()` (`KmsFactory`): OS keyring `backup_kek` →
  `FMGR_BACKUP_KEK` → nullptr (backups off). `make_default_kms`/`make_backup_kms`
  now share an internal `make_kms(basename, env_active, env_previous)` resolver.

**Whole-file cipher (`src/crypto/FileCipher.{h,cc}`):**
- `encrypt_file`/`decrypt_file` over libsodium `crypto_secretstream` (chunked,
  bounded memory). Fresh per-archive DEK wrapped by the injected KMS. On-disk =
  one JSON manifest line (`magic`, `v`, `kek_id`, wrapped `dek`, `ss_header`,
  `schema_version`, `backend`, `created_at_micros`, `content_sha256`) then
  length-prefixed ciphertext chunks; the last carries the FINAL tag.
  `content_sha256` (plaintext SHA-256) is verified on decrypt. Throws
  `crypto::CipherError` on wrong key / tamper / hash mismatch / malformed.

**SQLite hot copy (`src/cli/SqliteBackup.{h,cc}`):**
- `hot_copy(src, dst)` via `sqlite3_backup_init/step/finish` on a fresh READONLY
  source connection — safe online copy under WAL with live writers.

**CLI (`src/cli/BackupCommands.{h,cc}`, wired in `CliApp.cc`):**
- `freezerctl backup create --sqlite <db> --out <file> --actor <uuid>`: hot-copy
  → read schema version → `encrypt_file` → append a `backup.create` audit event
  (entity_kind `backup`, after-image = path/schema/hash; uses a `dynamic_cast` to
  `SqliteTransaction::note_mutation` since backup is not a domain entity).
- `freezerctl backup verify --in <file>` (= restore drill): decrypt to a scratch
  file, `PRAGMA integrity_check`, audit-chain verify. Reports PASS/FAIL; **never
  throws on a bad backup** (catches `CipherError`) so it is schedule-safe.
- `freezerctl backup restore --in <file> --out <db> [--force] --actor <uuid>`:
  decrypt to temp → rename into place (refuses to clobber without `--force`) →
  append `backup.restore` to the restored DB's own chain.
- `--postgres` on `backup create` returns a clear "not yet supported" error.

**Tests:** `file_cipher_test.cpp` (round-trip 0/small/multi-chunk, wrong key,
tamper, content-hash mismatch, plaintext-absent, malformed); `kms_test.cpp` /
`os_keyring_test.cpp` (named env/credential load, backup key distinct from master,
cross-unwrap fails, null when unconfigured); `cli_test.cpp` BackupTest
(create→verify→restore round-trip, `backup.create` audited, tamper→verify FAIL,
restore refuses overwrite without `--force`) + argv create/verify.

**Known limitations / follow-ups:**
- SQLite only. Postgres backup (encrypted `pg_dump` logical + documented
  `pg_basebackup`/WAL/PITR runbook) is slice 2.
- No scheduler yet: in-server nightly runner, retention rotation (30 daily / 12
  monthly / 7 yearly), and a weekly automated restore-drill are slice 3 (no
  scheduler exists; `SseBridge` uses `trantor::Timer`, reusable). A Prometheus
  backup-status metric and an optional `BackupService` RPC land with it.
- `backup create` audits via a `dynamic_cast` to `SqliteTransaction`; generalize
  when Postgres backup arrives (or add `note_mutation` to `ITransaction`).
- Whole-file streaming is memory-bounded but reads the input twice (hash pass +
  encrypt pass); fine for a local DB file.

Verification: `cmake --build --preset dev` clean; `ctest --preset dev -j1` — new
file-cipher / kms-backup / backup tests green; only the pre-existing baseline
failures remain. Manual E2E: create → verify PASS → tamper → verify FAIL →
restore → `audit verify` clean. `run-clang-tidy-17` on new/changed sources clean;
`clang-format-17` clean; `tools/check-spdx-headers.sh` clean; `git diff --check`
clean.
