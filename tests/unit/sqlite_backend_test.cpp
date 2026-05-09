// SPDX-License-Identifier: AGPL-3.0-or-later

#include "storage/sqlite/SqliteBackend.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace fmgr::storage {
  namespace {

    [[nodiscard]] std::filesystem::path sqlite_test_path(std::string_view suffix) {
      const auto unique = std::to_string(static_cast<unsigned long long>(
                              ::testing::UnitTest::GetInstance()->random_seed())) +
                          "-" + std::to_string(reinterpret_cast<std::uintptr_t>(&suffix));
      return std::filesystem::temp_directory_path() / (std::string("freezermanager-sqlite-unit-") +
                                                       unique + "-" + std::string(suffix) + ".db");
    }

    void remove_sqlite_files(const std::filesystem::path& path) {
      std::filesystem::remove(path);
      std::filesystem::remove(path.string() + "-wal");
      std::filesystem::remove(path.string() + "-shm");
    }

    TEST(SqliteBackend, InMemoryDatabaseMigratesAndReportsCapabilities) {
      SqliteBackend backend(SqliteBackendOptions{
          .database_path = ":memory:",
          .migrations =
              {
                  SqliteMigration{
                      .version = 1,
                      .name = "unit_schema",
                      .up_sql = "CREATE TABLE unit_schema (id INTEGER PRIMARY KEY);",
                  },
              },
      });

      backend.migrate_to_latest();

      EXPECT_EQ(backend.current_version(), SchemaVersion{1});
      const auto capabilities = backend.caps();
      EXPECT_TRUE(capabilities.json_path_equality);
      EXPECT_FALSE(capabilities.row_level_security);
      EXPECT_FALSE(capabilities.native_uuid);
    }

    TEST(SqliteBackend, ChangedAppliedMigrationChecksumIsRejected) {
      const auto path = sqlite_test_path("checksum");
      remove_sqlite_files(path);

      SqliteBackend initial_backend(SqliteBackendOptions{
          .database_path = path.string(),
          .migrations =
              {
                  SqliteMigration{
                      .version = 1,
                      .name = "unit_schema",
                      .up_sql = "CREATE TABLE unit_schema (id INTEGER PRIMARY KEY);",
                  },
              },
      });
      initial_backend.migrate_to_latest();

      SqliteBackend changed_backend(SqliteBackendOptions{
          .database_path = path.string(),
          .migrations =
              {
                  SqliteMigration{
                      .version = 1,
                      .name = "unit_schema",
                      .up_sql = "CREATE TABLE unit_schema_changed (id INTEGER PRIMARY KEY);",
                  },
              },
      });

      EXPECT_THROW(changed_backend.migrate_to_latest(), MigrationFailure);
      remove_sqlite_files(path);
    }

    TEST(SqliteBackend, DuplicateMigrationVersionIsRejected) {
      SqliteBackend backend(SqliteBackendOptions{
          .database_path = ":memory:",
          .migrations =
              {
                  SqliteMigration{
                      .version = 1,
                      .name = "first",
                      .up_sql = "CREATE TABLE first_table (id INTEGER PRIMARY KEY);",
                  },
                  SqliteMigration{
                      .version = 1,
                      .name = "duplicate",
                      .up_sql = "CREATE TABLE duplicate_table (id INTEGER PRIMARY KEY);",
                  },
              },
      });

      EXPECT_THROW(backend.migrate_to_latest(), MigrationFailure);
    }

  } // namespace
} // namespace fmgr::storage
