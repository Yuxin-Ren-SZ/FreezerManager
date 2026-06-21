// SPDX-License-Identifier: AGPL-3.0-or-later

#include "backup/SqliteBackup.h"

#include <sqlite3.h>

#include <string>

namespace fmgr::backup {

  namespace {

    // RAII for a sqlite3 connection so every early return closes it.
    struct Connection {
      sqlite3* handle{nullptr};
      ~Connection() {
        if (handle != nullptr) {
          sqlite3_close(handle);
        }
      }
      Connection() = default;
      Connection(const Connection&) = delete;
      Connection& operator=(const Connection&) = delete;
      Connection(Connection&&) = delete;
      Connection& operator=(Connection&&) = delete;
    };

  } // namespace

  void hot_copy(const std::string& src_db_path, const std::string& dst_db_path) {
    Connection src;
    if (sqlite3_open_v2(src_db_path.c_str(), &src.handle, SQLITE_OPEN_READONLY, nullptr) !=
        SQLITE_OK) {
      throw BackupError("cannot open source database: " + src_db_path);
    }

    Connection dst;
    if (sqlite3_open_v2(dst_db_path.c_str(), &dst.handle,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK) {
      throw BackupError("cannot open destination database: " + dst_db_path);
    }

    sqlite3_backup* backup = sqlite3_backup_init(dst.handle, "main", src.handle, "main");
    if (backup == nullptr) {
      throw BackupError(std::string("backup init failed: ") + sqlite3_errmsg(dst.handle));
    }

    // -1 copies all remaining pages in one step.
    const int step_rc = sqlite3_backup_step(backup, -1);
    const int finish_rc = sqlite3_backup_finish(backup);
    if (step_rc != SQLITE_DONE) {
      throw BackupError(std::string("backup step failed: ") + sqlite3_errstr(step_rc));
    }
    if (finish_rc != SQLITE_OK) {
      throw BackupError(std::string("backup finish failed: ") + sqlite3_errstr(finish_rc));
    }
  }

} // namespace fmgr::backup
