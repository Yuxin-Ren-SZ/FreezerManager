// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef FMGR_TEST_SUPPORT_TEMPSQLITEDB_H
#define FMGR_TEST_SUPPORT_TEMPSQLITEDB_H

#include "core/uuid.h"

#include <unistd.h>

#include <filesystem>
#include <string>
#include <string_view>

namespace fmgr::test {

  class TempSqliteDb {
  public:
    explicit TempSqliteDb(std::string_view prefix)
        : path_(std::filesystem::temp_directory_path() /
                (std::string(prefix) + "-pid" + std::to_string(::getpid()) + "-" +
                 core::generate_uuid_v4() + ".db")) {
      cleanup();
    }

    ~TempSqliteDb() noexcept {
      cleanup();
    }

    TempSqliteDb(const TempSqliteDb&) = delete;
    TempSqliteDb& operator=(const TempSqliteDb&) = delete;
    TempSqliteDb(TempSqliteDb&&) = delete;
    TempSqliteDb& operator=(TempSqliteDb&&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const {
      return path_;
    }

    [[nodiscard]] std::string string() const {
      return path_.string();
    }

    void cleanup() const noexcept {
      std::error_code error;
      std::filesystem::remove(path_, error);
      std::filesystem::remove(std::filesystem::path(path_.string() + "-wal"), error);
      std::filesystem::remove(std::filesystem::path(path_.string() + "-shm"), error);
    }

  private:
    std::filesystem::path path_;
  };

} // namespace fmgr::test

#endif // FMGR_TEST_SUPPORT_TEMPSQLITEDB_H
