// SPDX-License-Identifier: AGPL-3.0-or-later

// SqliteBackup — online hot copy of a SQLite database (PRD §14).
//
// Uses the sqlite3 online-backup API (sqlite3_backup_init/step/finish) against a
// freshly opened source connection, so the copy is transactionally consistent
// even while the live server has the database open in WAL mode with concurrent
// writers. The result is a standalone, fully-checkpointed database file ready to
// be encrypted by crypto::encrypt_file.
#ifndef FMGR_CLI_SQLITEBACKUP_H
#define FMGR_CLI_SQLITEBACKUP_H

#include <stdexcept>
#include <string>

namespace fmgr::cli {

  // Raised on any SQLite-level backup failure.
  class BackupError : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
  };

  // Copy the database at `src_db_path` to a new file at `dst_db_path` (overwriting
  // it). Throws BackupError on any open/copy failure.
  void hot_copy(const std::string& src_db_path, const std::string& dst_db_path);

} // namespace fmgr::cli

#endif // FMGR_CLI_SQLITEBACKUP_H
