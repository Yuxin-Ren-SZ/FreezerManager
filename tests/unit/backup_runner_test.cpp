// SPDX-License-Identifier: AGPL-3.0-or-later

#include "backup/BackupRunner.h"

#include "backup/BackupCommands.h"
#include "backup/BackupFilename.h"
#include "backup/RetentionPolicy.h"
#include "core/audit_event.h"
#include "core/ids.h"
#include "kms/EnvVarKms.h"
#include "storage/AuditTraits.h"
#include "storage/sqlite/AuditRepositories.h"
#include "storage/sqlite/SqliteBackend.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "test_helpers.h"
#include <gtest/gtest.h>

namespace fmgr::backup {
  namespace {

    using namespace fmgr::test;
    namespace fs = std::filesystem;

    constexpr const char* kBackupKekB64 = "ICEiIyQlJicoKSorLC0uLzAxMjM0NTY3ODk6Ozw9Pj8=";
    constexpr std::int64_t kMicros = 1'000'000;
    constexpr std::int64_t kDay = 86'400 * kMicros;

    fs::path unique_temp(const std::string& suffix) {
      static std::atomic<std::uint64_t> counter{0};
      return fs::temp_directory_path() /
             ("freezermanager-runner-" + std::to_string(counter.fetch_add(1)) + suffix);
    }

    // A live on-disk SQLite database with a working audit chain, plus a backup
    // directory. Mirrors the minimal surface run_backup_tick needs.
    struct RunnerFixture {
      fs::path db = unique_temp(".db");
      fs::path dir = unique_temp(".backups");
      storage::SqliteBackend backend{storage::SqliteBackendOptions{.database_path = db.string()}};

      RunnerFixture() {
        storage::register_audit_repositories(backend);
        backend.migrate_to_latest();
        fs::create_directories(dir);
      }
      ~RunnerFixture() {
        remove_sqlite_files(db);
        std::error_code error;
        fs::remove_all(dir, error);
      }
      RunnerFixture(const RunnerFixture&) = delete;
      RunnerFixture& operator=(const RunnerFixture&) = delete;

      [[nodiscard]] BackupScheduleConfig config() const {
        return BackupScheduleConfig{.sqlite_db_path = db.string(),
                                    .backup_dir = dir.string(),
                                    .retention = RetentionPolicy{30, 12, 7},
                                    .backup_interval_micros = kDay,
                                    .drill_interval_micros = 7 * kDay,
                                    .actor = id_from_low<core::UserId>(10)};
      }

      [[nodiscard]] std::vector<std::string> audit_actions() {
        auto txn = backend.begin(storage::IsolationLevel::Serializable);
        const auto events =
            txn->repo<core::AuditEvent>().query(storage::Query<core::AuditEvent>::all());
        txn->commit();
        std::vector<std::string> actions;
        for (const auto& event : events) {
          actions.push_back(event.action);
        }
        return actions;
      }

      [[nodiscard]] std::size_t backup_count() const {
        std::size_t n = 0;
        for (const auto& entry : fs::directory_iterator(dir)) {
          if (parse_backup_timestamp(entry.path().filename().string()).has_value()) {
            ++n;
          }
        }
        return n;
      }

      // Seed a dummy (non-decryptable) backup file with a valid name on a day.
      void seed_dummy(std::int64_t day) {
        const fs::path p = dir / make_backup_filename(day * kDay);
        std::ofstream(p) << "dummy";
      }
    };

    TEST(BackupRunnerTest, CreatesBackupWhenDue) {
      RunnerFixture fix;
      std::ostringstream log;
      const auto result = run_backup_tick(fix.backend, kms::EnvVarKms::from_base64(kBackupKekB64),
                                          fix.config(), 100 * kDay, 1234, log);
      EXPECT_TRUE(result.backup_made);
      EXPECT_TRUE(fs::exists(result.backup_path)) << log.str();
      EXPECT_EQ(fix.backup_count(), 1U);
    }

    TEST(BackupRunnerTest, SkipsCreateWhenNotDue) {
      RunnerFixture fix;
      std::ostringstream log;
      const auto first = run_backup_tick(fix.backend, kms::EnvVarKms::from_base64(kBackupKekB64),
                                         fix.config(), 100 * kDay, 1234, log);
      EXPECT_TRUE(first.backup_made);
      // A second tick only an hour later must not create another backup.
      const auto second = run_backup_tick(fix.backend, kms::EnvVarKms::from_base64(kBackupKekB64),
                                          fix.config(), 100 * kDay + 3'600 * kMicros, 1234, log);
      EXPECT_FALSE(second.backup_made);
      EXPECT_EQ(fix.backup_count(), 1U);
    }

    TEST(BackupRunnerTest, PrunesPerRetentionAndAudits) {
      RunnerFixture fix;
      for (std::int64_t day = 0; day < 5; ++day) {
        fix.seed_dummy(day);
      }
      auto config = fix.config();
      config.retention = RetentionPolicy{1, 0, 0};   // keep only newest day
      config.backup_interval_micros = 10'000 * kDay; // never create
      config.drill_interval_micros = 10'000 * kDay;  // never drill
      // Mark a fresh drill so the drill cadence is not due.
      std::ofstream(fix.dir / ".last_drill") << (5 * kDay) << '\n';

      std::ostringstream log;
      const auto result = run_backup_tick(fix.backend, kms::EnvVarKms::from_base64(kBackupKekB64),
                                          config, 5 * kDay, 1234, log);
      EXPECT_FALSE(result.backup_made);
      EXPECT_EQ(result.pruned, 4) << log.str();
      EXPECT_EQ(fix.backup_count(), 1U); // only the newest day survives

      const auto actions = fix.audit_actions();
      EXPECT_EQ(std::count(actions.begin(), actions.end(), "backup.prune"), 4);
    }

    TEST(BackupRunnerTest, RunsDrillOnGoodBackup) {
      RunnerFixture fix;
      auto config = fix.config();
      config.backup_interval_micros = 10'000 * kDay; // don't auto-create in the drill tick
      // First make one real, valid backup.
      std::ostringstream sink;
      run_backup_create(fix.backend, fix.db.string(), kms::EnvVarKms::from_base64(kBackupKekB64),
                        (fix.dir / make_backup_filename(100 * kDay)).string(),
                        id_from_low<core::UserId>(10), sink);

      std::ostringstream log;
      const auto result = run_backup_tick(fix.backend, kms::EnvVarKms::from_base64(kBackupKekB64),
                                          config, 100 * kDay, 1234, log);
      EXPECT_TRUE(result.drill_ran);
      EXPECT_TRUE(result.drill_ok) << log.str();

      const auto actions = fix.audit_actions();
      EXPECT_EQ(std::count(actions.begin(), actions.end(), "backup.drill"), 1);
    }

    TEST(BackupRunnerTest, DrillDetectsCorruptBackup) {
      RunnerFixture fix;
      auto config = fix.config();
      config.backup_interval_micros = 10'000 * kDay;
      const fs::path backup = fix.dir / make_backup_filename(100 * kDay);
      std::ostringstream sink;
      run_backup_create(fix.backend, fix.db.string(), kms::EnvVarKms::from_base64(kBackupKekB64),
                        backup.string(), id_from_low<core::UserId>(10), sink);

      // Corrupt the ciphertext tail.
      std::vector<char> bytes;
      {
        std::ifstream in(backup, std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
      }
      ASSERT_GT(bytes.size(), 16U);
      bytes[bytes.size() - 4] ^= 0x01;
      {
        std::ofstream out(backup, std::ios::binary | std::ios::trunc);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
      }

      std::ostringstream log;
      const auto result = run_backup_tick(fix.backend, kms::EnvVarKms::from_base64(kBackupKekB64),
                                          config, 100 * kDay, 1234, log);
      EXPECT_TRUE(result.drill_ran);
      EXPECT_FALSE(result.drill_ok);
      EXPECT_NE(log.str().find("DRILL FAILED"), std::string::npos);
    }

    TEST(BackupRunnerTest, SkipsDrillWhenMarkerFresh) {
      RunnerFixture fix;
      auto config = fix.config();
      config.backup_interval_micros = 10'000 * kDay;
      std::ostringstream sink;
      run_backup_create(fix.backend, fix.db.string(), kms::EnvVarKms::from_base64(kBackupKekB64),
                        (fix.dir / make_backup_filename(100 * kDay)).string(),
                        id_from_low<core::UserId>(10), sink);
      std::ofstream(fix.dir / ".last_drill") << (100 * kDay) << '\n'; // drilled "now"

      std::ostringstream log;
      const auto result = run_backup_tick(fix.backend, kms::EnvVarKms::from_base64(kBackupKekB64),
                                          config, 100 * kDay, 1234, log);
      EXPECT_FALSE(result.drill_ran);
    }

    TEST(BackupRunnerTest, ListReportsNewestFirst) {
      RunnerFixture fix;
      fix.seed_dummy(1);
      fix.seed_dummy(3);
      fix.seed_dummy(2);
      std::ostringstream out;
      EXPECT_EQ(run_backup_list(fix.dir.string(), out), 0);
      const std::string text = out.str();
      const auto p3 = text.find(make_backup_filename(3 * kDay));
      const auto p2 = text.find(make_backup_filename(2 * kDay));
      const auto p1 = text.find(make_backup_filename(1 * kDay));
      ASSERT_NE(p3, std::string::npos);
      EXPECT_LT(p3, p2);
      EXPECT_LT(p2, p1);
    }

  } // namespace
} // namespace fmgr::backup
