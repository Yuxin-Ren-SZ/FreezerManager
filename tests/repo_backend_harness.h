// SPDX-License-Identifier: AGPL-3.0-or-later

// Test-only harness that drives a domain-repository test suite over both the
// SQLite and PostgreSQL backends from a single parameterized fixture. Only the
// dedicated repository-test executable (which links both backends) includes this
// header. The Postgres parameter skips when FMGR_TEST_POSTGRES_URL is unset.
#ifndef FMGR_TESTS_REPO_BACKEND_HARNESS_H
#define FMGR_TESTS_REPO_BACKEND_HARNESS_H

#include "storage/postgres/PostgresBackend.h"
#include "storage/sqlite/SqliteBackend.h"

#include "test_helpers.h"

#include <gtest/gtest.h>
#include <pqxx/pqxx>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>

namespace fmgr::test {

  enum class BackendKind : std::uint8_t { Sqlite, Postgres };

  [[nodiscard]] inline std::string backend_kind_name(BackendKind kind) {
    return kind == BackendKind::Sqlite ? "Sqlite" : "Postgres";
  }

  // Make GoogleTest print "Sqlite"/"Postgres" for the parameter instead of the
  // default "1-byte object <00>" enum dump (keeps ctest/gtest names readable).
  inline void PrintTo(BackendKind kind, std::ostream* os) {
    *os << backend_kind_name(kind);
  }

  [[nodiscard]] inline std::optional<std::string> postgres_test_url() {
    const char* url = std::getenv("FMGR_TEST_POSTGRES_URL");
    if (url == nullptr || std::string_view(url).empty()) {
      return std::nullopt;
    }
    return std::string(url);
  }

  // Owns a freshly-migrated backend of the requested kind with a caller-supplied
  // set of repositories registered. SQLite uses a unique temp database file;
  // Postgres wipes and recreates the public schema for per-test isolation.
  class RepoBackendHarness {
  public:
    using SqliteRegistrar = std::function<void(storage::SqliteBackend&)>;
    using PostgresRegistrar = std::function<void(storage::PostgresBackend&)>;

    RepoBackendHarness(BackendKind kind, const SqliteRegistrar& register_sqlite,
                       const PostgresRegistrar& register_postgres)
        : kind_(kind) {
      if (kind_ == BackendKind::Sqlite) {
        db_path_ = unique_sqlite_path();
        remove_sqlite_files(db_path_);
        sqlite_.emplace(storage::SqliteBackendOptions{.database_path = db_path_.string()});
        register_sqlite(sqlite_.value());
        sqlite_->migrate_to_latest();
      } else {
        const auto url = postgres_test_url().value();
        {
          pqxx::connection setup_conn(url);
          pqxx::work txn(setup_conn);
          txn.exec("DROP SCHEMA public CASCADE");
          txn.exec("CREATE SCHEMA public");
          txn.commit();
        }
        postgres_.emplace(storage::PostgresBackendOptions{
            .connection_string = url,
            .pool_size = 4,
        });
        register_postgres(postgres_.value());
        postgres_->migrate_to_latest();
      }
    }

    ~RepoBackendHarness() {
      if (kind_ == BackendKind::Sqlite) {
        remove_sqlite_files(db_path_);
      }
    }

    RepoBackendHarness(const RepoBackendHarness&) = delete;
    RepoBackendHarness& operator=(const RepoBackendHarness&) = delete;
    RepoBackendHarness(RepoBackendHarness&&) = delete;
    RepoBackendHarness& operator=(RepoBackendHarness&&) = delete;

    // sqlite_/postgres_ engagement is determined by kind_ (exactly one is set);
    // clang-tidy can't see that cross-variable invariant.
    [[nodiscard]] storage::IStorageBackend& backend() {
      if (kind_ == BackendKind::Sqlite) {
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        return sqlite_.value();
      }
      // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
      return postgres_.value();
    }

    [[nodiscard]] std::size_t audit_event_count() {
      if (kind_ == BackendKind::Sqlite) {
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        return sqlite_->audit_event_count_for_tests();
      }
      // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
      return postgres_->audit_event_count_for_tests();
    }

  private:
    [[nodiscard]] static std::filesystem::path unique_sqlite_path() {
      static std::atomic<std::uint64_t> counter{0};
      const auto unique = std::to_string(counter.fetch_add(1)) + "-" +
                          std::to_string(static_cast<unsigned long long>(
                              ::testing::UnitTest::GetInstance()->random_seed()));
      return std::filesystem::temp_directory_path() / ("freezermanager-repo-" + unique + ".db");
    }

    BackendKind kind_;
    std::filesystem::path db_path_;
    std::optional<storage::SqliteBackend> sqlite_;
    std::optional<storage::PostgresBackend> postgres_;
  };

} // namespace fmgr::test

#endif // FMGR_TESTS_REPO_BACKEND_HARNESS_H
