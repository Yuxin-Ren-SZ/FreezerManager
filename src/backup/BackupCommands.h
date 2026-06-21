// SPDX-License-Identifier: AGPL-3.0-or-later

// Encrypted SQLite backups, restore, and a restore-drill verify (PRD §14).
// Backups are an online hot copy of the live database (SqliteBackup::hot_copy)
// encrypted under a *separate* backup KEK (crypto::encrypt_file +
// kms::make_backup_kms), so losing the live master key alone cannot decrypt them
// (PRD §8). Engines are free functions writing to an injected std::ostream so
// they are unit-testable, mirroring KeyCommands.
#ifndef FMGR_BACKUP_BACKUPCOMMANDS_H
#define FMGR_BACKUP_BACKUPCOMMANDS_H

#include "core/ids.h"
#include "kms/IKmsProvider.h"
#include "storage/IStorageBackend.h"

#include <nlohmann/json.hpp>

#include <ostream>
#include <string>

namespace fmgr::backup {

  struct BackupCreateReport {
    std::string out_path;
    int schema_version{};
    std::string content_sha256;
  };

  // Hot-copy the live SQLite database at `sqlite_db_path`, encrypt it to
  // `out_path` under `backup_kms`, and append a `backup.create` audit event to
  // `backend`. Throws BackupError / CipherError / BackendError on failure.
  BackupCreateReport run_backup_create(storage::IStorageBackend& backend,
                                       const std::string& sqlite_db_path,
                                       const kms::IKmsProvider& backup_kms,
                                       const std::string& out_path, core::UserId actor,
                                       std::ostream& sink);

  // PostgreSQL counterpart: `pg_dump -Fc` the database at `conninfo`, encrypt the
  // dump to `out_path` under `backup_kms` (manifest tagged backend="postgres"), and
  // append a `backup.create` audit event to `backend`. Throws BackupError /
  // CipherError / BackendError on failure.
  BackupCreateReport run_backup_create_postgres(storage::IStorageBackend& backend,
                                                const std::string& conninfo,
                                                const kms::IKmsProvider& backup_kms,
                                                const std::string& out_path, core::UserId actor,
                                                std::ostream& sink);

  struct BackupVerifyReport {
    bool ok{};
    int schema_version{};
    std::string detail;
  };

  // Restore-drill: decrypt `in_path` to a scratch file, then verify it. The check
  // dispatches on the archive's manifest `backend` tag: a SQLite archive must
  // decrypt intact, pass `PRAGMA integrity_check`, and have a verifying audit hash
  // chain; a Postgres archive (a `pg_dump` custom-format file) must decrypt intact
  // and read cleanly under `pg_restore --list`. (A full restore-into-scratch-DB
  // Postgres drill is a documented follow-up.) Never throws on a *bad backup* — it
  // reports ok=false with a reason — so it is safe to schedule. Throws only on a
  // programmer/IO error it cannot classify.
  BackupVerifyReport run_backup_verify(const std::string& in_path,
                                       const kms::IKmsProvider& backup_kms, std::ostream& sink);

  struct BackupRestoreReport {
    std::string out_path;
    int schema_version{};
  };

  // Decrypt `in_path` to `out_path` (refusing to overwrite an existing file unless
  // `force`), then append a `backup.restore` event to the restored database's own
  // audit chain. Throws BackupError / CipherError on failure.
  BackupRestoreReport run_backup_restore(const std::string& in_path,
                                         const kms::IKmsProvider& backup_kms,
                                         const std::string& out_path, bool force,
                                         core::UserId actor, std::ostream& sink);

  // PostgreSQL counterpart: decrypt `in_path` to a scratch dump, `pg_restore` it
  // into the database at `conninfo` (--clean --if-exists), then append a
  // `backup.restore` event to the restored database's own audit chain. The target
  // database must already exist. Throws BackupError / CipherError on failure.
  BackupRestoreReport run_backup_restore_postgres(const std::string& in_path,
                                                  const kms::IKmsProvider& backup_kms,
                                                  const std::string& conninfo, core::UserId actor,
                                                  std::ostream& sink);

  // Append a `backup.*` event to a backend's audit chain (SQLite or Postgres). The
  // snapshot is server-derived metadata (paths/hash), never PHI. Shared by the
  // engines above and by the scheduler tick (BackupRunner).
  //
  // The append is best-effort and uses ReadCommitted isolation: an audit-only
  // append needs no Serializable guarantee, and under heavy concurrent write load
  // a Serializable append could conflict and throw, which must not abort the
  // backup (the backup file itself is already written). A failed append is logged
  // to `warn` and swallowed, not propagated (review F-4).
  void append_backup_event(storage::IStorageBackend& backend, const std::string& action,
                           const std::string& entity_id, const nlohmann::json& after,
                           core::UserId actor, std::ostream& warn);

} // namespace fmgr::backup

#endif // FMGR_BACKUP_BACKUPCOMMANDS_H
