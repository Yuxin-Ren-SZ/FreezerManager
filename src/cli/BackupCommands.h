// SPDX-License-Identifier: AGPL-3.0-or-later

// `freezerctl backup` — encrypted SQLite backups, restore, and a restore-drill
// verify (PRD §14). Backups are an online hot copy of the live database
// (SqliteBackup::hot_copy) encrypted under a *separate* backup KEK
// (crypto::encrypt_file + kms::make_backup_kms), so losing the live master key
// alone cannot decrypt them (PRD §8). Engines are free functions writing to an
// injected std::ostream so they are unit-testable, mirroring KeyCommands.
#ifndef FMGR_CLI_BACKUPCOMMANDS_H
#define FMGR_CLI_BACKUPCOMMANDS_H

#include "core/ids.h"
#include "kms/IKmsProvider.h"
#include "storage/IStorageBackend.h"

#include <ostream>
#include <string>

namespace fmgr::cli {

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

  struct BackupVerifyReport {
    bool ok{};
    int schema_version{};
    std::string detail;
  };

  // Restore-drill: decrypt `in_path` to a scratch file, then check it decrypts
  // intact, passes `PRAGMA integrity_check`, and its audit hash chain verifies.
  // Never throws on a *bad backup* — it reports ok=false with a reason — so it is
  // safe to schedule. Throws only on a programmer/IO error it cannot classify.
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

} // namespace fmgr::cli

#endif // FMGR_CLI_BACKUPCOMMANDS_H
