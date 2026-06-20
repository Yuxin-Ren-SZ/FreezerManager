// SPDX-License-Identifier: AGPL-3.0-or-later

// PostgresDump — logical PostgreSQL backup via the standard `pg_dump` /
// `pg_restore` client tools (PRD §14). A custom-format dump (`pg_dump -Fc`) is the
// plaintext payload that crypto::encrypt_file then seals under the backup KEK,
// exactly mirroring the SQLite hot-copy → encrypt path.
//
// Tools are spawned with fork/execvp (never a shell) so a connection string can
// never be interpreted as a command. The password is stripped from the connection
// string and handed to the child only through the PGPASSWORD environment variable,
// so it never appears in the process argument list (visible via `ps`).
//
// Continuous-WAL / PITR (`pg_basebackup`) is intentionally out of scope here — it
// is a documented operator runbook, not code, for v1.
#ifndef FMGR_BACKUP_POSTGRESDUMP_H
#define FMGR_BACKUP_POSTGRESDUMP_H

#include <string>

namespace fmgr::backup {

  // Dump the database identified by `conninfo` (a libpq connection URI or
  // keyword/value string) to `out_path` as a custom-format archive
  // (`pg_dump -Fc`). Throws BackupError if pg_dump is missing or exits non-zero.
  void pg_dump_to_file(const std::string& conninfo, const std::string& out_path);

  // Restore a custom-format dump at `dump_path` into the database identified by
  // `conninfo` (`pg_restore --clean --if-exists --no-owner`). Throws BackupError if
  // pg_restore is missing or exits non-zero.
  void pg_restore_from_file(const std::string& conninfo, const std::string& dump_path);

  // Structural check used by the restore-drill: `pg_restore --list` over the dump.
  // Returns true if the archive's table of contents reads cleanly. On failure
  // returns false and writes the reason to `detail` — never throws on a bad/corrupt
  // dump, so it is schedule-safe (mirrors run_backup_verify's contract).
  [[nodiscard]] bool pg_restore_list_ok(const std::string& dump_path, std::string& detail);

} // namespace fmgr::backup

#endif // FMGR_BACKUP_POSTGRESDUMP_H
