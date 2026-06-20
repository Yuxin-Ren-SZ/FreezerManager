// SPDX-License-Identifier: AGPL-3.0-or-later

#include "server/BackupScheduler.h"

#include "backup/BackupFilename.h"
#include "backup/BackupRunner.h"
#include "backup/RetentionPolicy.h"
#include "core/ids.h"
#include "kms/EnvVarKms.h"
#include "storage/sqlite/AuditRepositories.h"
#include "storage/sqlite/SqliteBackend.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

#include "test_helpers.h"
#include <gtest/gtest.h>

namespace fmgr::server {
  namespace {

    using namespace fmgr::test;
    namespace fs = std::filesystem;

    constexpr const char* kBackupKekB64 = "ICEiIyQlJicoKSorLC0uLzAxMjM0NTY3ODk6Ozw9Pj8=";
    constexpr std::int64_t kMicros = 1'000'000;
    constexpr std::int64_t kDay = 86'400 * kMicros;

    fs::path unique_temp(const std::string& suffix) {
      static std::atomic<std::uint64_t> counter{0};
      return fs::temp_directory_path() /
             ("freezermanager-sched-" + std::to_string(counter.fetch_add(1)) + suffix);
    }

    std::unique_ptr<kms::IKmsProvider> backup_kms() {
      return std::make_unique<kms::EnvVarKms>(kms::EnvVarKms::from_base64(kBackupKekB64));
    }

    std::size_t backup_count(const fs::path& dir) {
      std::size_t n = 0;
      std::error_code error;
      for (const auto& entry : fs::directory_iterator(dir, error)) {
        if (backup::parse_backup_timestamp(entry.path().filename().string()).has_value()) {
          ++n;
        }
      }
      return n;
    }

    TEST(BackupSchedulerTest, CreatesBackupOnFirstTickAndStopsCleanly) {
      const fs::path db = unique_temp(".db");
      const fs::path dir = unique_temp(".backups");
      storage::SqliteBackend backend(storage::SqliteBackendOptions{.database_path = db.string()});
      storage::register_audit_repositories(backend);
      backend.migrate_to_latest();
      fs::create_directories(dir);

      backup::BackupScheduleConfig config{.sqlite_db_path = db.string(),
                                          .backup_dir = dir.string(),
                                          .retention = backup::RetentionPolicy{30, 12, 7},
                                          .backup_interval_micros = 1,           // always due
                                          .drill_interval_micros = 1'000 * kDay, // never drill
                                          .actor = id_from_low<core::UserId>(7)};

      BackupScheduler scheduler(backend, backup_kms(), std::move(config),
                                std::chrono::milliseconds(10));
      scheduler.start();

      // Wait (bounded) for the first scheduled backup to land.
      bool seen = false;
      for (int i = 0; i < 300 && !seen; ++i) {
        if (backup_count(dir) >= 1) {
          seen = true;
          break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
      scheduler.stop(); // must join without hanging or racing

      EXPECT_TRUE(seen);
      EXPECT_GE(backup_count(dir), 1U);

      remove_sqlite_files(db);
      std::error_code error;
      fs::remove_all(dir, error);
    }

    TEST(BackupSchedulerTest, StopBeforeStartIsSafe) {
      const fs::path db = unique_temp(".db");
      storage::SqliteBackend backend(storage::SqliteBackendOptions{.database_path = db.string()});
      backend.migrate_to_latest();
      backup::BackupScheduleConfig config{.sqlite_db_path = db.string(),
                                          .backup_dir = unique_temp(".backups").string(),
                                          .actor = id_from_low<core::UserId>(7)};
      BackupScheduler scheduler(backend, backup_kms(), std::move(config));
      scheduler.stop(); // never started — no-op, no crash
      remove_sqlite_files(db);
    }

  } // namespace
} // namespace fmgr::server
