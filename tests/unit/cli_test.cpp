// SPDX-License-Identifier: AGPL-3.0-or-later

#include "cli/AuditCommands.h"
#include "cli/BackendFactory.h"
#include "cli/CliApp.h"
#include "cli/CsvWriter.h"
#include "cli/SampleCommands.h"
#include "cli/SampleCsv.h"
#include "cli/SampleQuery.h"

#include "core/identity.h"
#include "core/item_type.h"
#include "core/sample.h"
#include "storage/IdentityTraits.h"
#include "storage/ItemTypeTraits.h"
#include "storage/SampleTraits.h"

#include "repo_backend_harness.h"
#include "test_helpers.h"
#include <gtest/gtest.h>
#include <pqxx/pqxx>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fmgr::cli {
  namespace {
    using namespace fmgr::test;

    storage::MutationContext mutation_context() {
      return storage::MutationContext{
          .actor_user_id = id_from_low<core::UserId>(999),
          .actor_session_id = "cli-test-session",
          .request_id = "cli-test-request",
          .reason = "cli test",
      };
    }

    core::Lab make_lab(std::uint64_t low) {
      return core::Lab{
          .id = id_from_low<core::LabId>(low),
          .name = "Lab " + std::to_string(low),
          .contact = "lab@example.org",
          .created_at = ts(100 + static_cast<std::int64_t>(low)),
          .settings_json = nlohmann::json::object(),
      };
    }

    core::User make_user(std::uint64_t low, core::LabId lab_id) {
      return core::User{
          .id = id_from_low<core::UserId>(low),
          .primary_email = "user" + std::to_string(low) + "@example.org",
          .display_name = "Test User",
          .status = core::UserStatus::Active,
          .created_at = ts(200 + static_cast<std::int64_t>(low)),
          .auth_bindings = nlohmann::json::array({{{"provider", "local"}}}),
          .default_lab_id = lab_id,
      };
    }

    core::ItemType make_item_type(std::uint64_t low, core::LabId lab_id) {
      return core::ItemType{
          .id = id_from_low<core::ItemTypeId>(low),
          .lab_id = lab_id,
          .parent_id = std::nullopt,
          .name = "liquid",
          .created_at = ts(300 + static_cast<std::int64_t>(low)),
      };
    }

    core::Sample make_sample(std::uint64_t low, core::LabId lab_id, core::ItemTypeId item_type_id,
                             core::UserId user_id) {
      return core::Sample{
          .id = id_from_low<core::SampleId>(low),
          .lab_id = lab_id,
          .item_type_id = item_type_id,
          .name = "Sample " + std::to_string(low),
          .status = core::SampleStatus::Active,
          .created_by = user_id,
          .created_at = ts(800 + static_cast<std::int64_t>(low)),
          .last_modified_by = user_id,
          .last_modified_at = ts(800 + static_cast<std::int64_t>(low)),
      };
    }

    // A throwaway CLI environment (SQLite temp file or a freshly-reset Postgres
    // public schema) opened through the production open_backend() factory and
    // seeded with two labs: lab A has two samples, lab B has one.
    class CliFixture {
    public:
      explicit CliFixture(BackendKind kind) : kind_(kind) {
        backend_ = open_backend(make_options());

        lab_a_ = make_lab(1).id;
        lab_b_ = make_lab(2).id;
        const auto user = make_user(10, lab_a_);
        const auto item_type = make_item_type(20, lab_a_);
        const auto user_b = make_user(11, lab_b_);
        const auto item_type_b = make_item_type(21, lab_b_);

        // Prerequisites first: sample insert validates that item_type references a
        // live *committed* row, so labs/users/item-types must land before samples.
        {
          auto txn = backend_->begin(storage::IsolationLevel::Serializable);
          txn->repo<core::Lab>().insert(make_lab(1), mutation_context());
          txn->repo<core::Lab>().insert(make_lab(2), mutation_context());
          txn->repo<core::User>().insert(user, mutation_context());
          txn->repo<core::User>().insert(user_b, mutation_context());
          txn->repo<core::ItemType>().insert(item_type, mutation_context());
          txn->repo<core::ItemType>().insert(item_type_b, mutation_context());
          txn->commit();
        }
        {
          auto txn = backend_->begin(storage::IsolationLevel::Serializable);
          txn->repo<core::Sample>().insert(make_sample(100, lab_a_, item_type.id, user.id),
                                           mutation_context());
          txn->repo<core::Sample>().insert(make_sample(101, lab_a_, item_type.id, user.id),
                                           mutation_context());
          txn->repo<core::Sample>().insert(make_sample(200, lab_b_, item_type_b.id, user_b.id),
                                           mutation_context());
          txn->commit();
        }
      }

      ~CliFixture() {
        backend_.reset();
        if (kind_ == BackendKind::Sqlite) {
          remove_sqlite_files(db_path_);
        }
      }

      CliFixture(const CliFixture&) = delete;
      CliFixture& operator=(const CliFixture&) = delete;
      CliFixture(CliFixture&&) = delete;
      CliFixture& operator=(CliFixture&&) = delete;

      storage::IStorageBackend& backend() {
        return *backend_;
      }
      [[nodiscard]] core::LabId lab_a() const {
        return lab_a_;
      }
      [[nodiscard]] core::LabId lab_b() const {
        return lab_b_;
      }
      // Only meaningful for the SQLite backend (used by the argv-level test).
      [[nodiscard]] std::string db_path() const {
        return db_path_.string();
      }

    private:
      BackendOptions make_options() {
        if (kind_ == BackendKind::Sqlite) {
          db_path_ = unique_path();
          remove_sqlite_files(db_path_);
          return BackendOptions{.sqlite_path = db_path_.string()};
        }
        const auto url_opt = postgres_test_url();
        if (!url_opt.has_value()) {
          throw std::logic_error("postgres url required (SetUp must skip when unset)");
        }
        const auto& url = url_opt.value();
        // Per-test isolation: wipe the shared public schema so open_backend()
        // migrates from scratch (mirrors RepoBackendHarness).
        pqxx::connection setup_conn(url);
        pqxx::work txn(setup_conn);
        txn.exec("DROP SCHEMA public CASCADE");
        txn.exec("CREATE SCHEMA public");
        txn.commit();
        return BackendOptions{.postgres_url = url};
      }

      static std::filesystem::path unique_path() {
        static std::atomic<std::uint64_t> counter{0};
        return std::filesystem::temp_directory_path() /
               ("freezermanager-cli-" + std::to_string(counter.fetch_add(1)) + ".db");
      }

      BackendKind kind_;
      std::filesystem::path db_path_;
      std::unique_ptr<storage::IStorageBackend> backend_;
      core::LabId lab_a_;
      core::LabId lab_b_;
    };

    // ---- CsvWriter ----

    TEST(CsvWriterTest, LeavesPlainFieldUnquoted) {
      EXPECT_EQ(csv_escape_field("hello"), "hello");
      EXPECT_EQ(csv_escape_field(""), "");
    }

    TEST(CsvWriterTest, QuotesFieldsWithSpecialChars) {
      EXPECT_EQ(csv_escape_field("a,b"), "\"a,b\"");
      EXPECT_EQ(csv_escape_field("line\nbreak"), "\"line\nbreak\"");
      EXPECT_EQ(csv_escape_field("carriage\rreturn"), "\"carriage\rreturn\"");
    }

    TEST(CsvWriterTest, DoublesEmbeddedQuotes) {
      EXPECT_EQ(csv_escape_field("say \"hi\""), "\"say \"\"hi\"\"\"");
    }

    TEST(CsvWriterTest, WritesCommaSeparatedRowWithCrlf) {
      std::ostringstream out;
      write_csv_row(out, {"a", "b,c", "d"});
      EXPECT_EQ(out.str(), "a,\"b,c\",d\r\n");
    }

    // ---- SampleCsv ----

    TEST(SampleCsvTest, ColumnsExcludePhiAndMatchToJson) {
      const auto& columns = sample_csv_columns();
      EXPECT_EQ(columns.front(), "id");
      for (const auto& column : columns) {
        EXPECT_NE(column, "phi_fields_enc_json") << "PHI ciphertext must not be exported";
      }
    }

    TEST(SampleCsvTest, RowHasOneCellPerColumn) {
      const auto sample =
          make_sample(100, id_from_low<core::LabId>(1), id_from_low<core::ItemTypeId>(20),
                      id_from_low<core::UserId>(10));
      const auto row = sample_to_csv_row(sample);
      EXPECT_EQ(row.size(), sample_csv_columns().size());
      // Absent optional (barcode) renders empty; name renders verbatim.
      const auto barcode_index = static_cast<std::size_t>(std::distance(
          sample_csv_columns().begin(),
          std::find(sample_csv_columns().begin(), sample_csv_columns().end(), "barcode")));
      EXPECT_EQ(row.at(barcode_index), "");
    }

    TEST(SampleCsvTest, ExportHeaderHasChainOfCustodyLines) {
      std::ostringstream out;
      write_export_header(out, 13, "lab-123", "2026-06-07T00:00:00Z");
      const std::string text = out.str();
      EXPECT_NE(text.find("schema_version=13"), std::string::npos);
      EXPECT_NE(text.find("lab_id=lab-123"), std::string::npos);
      EXPECT_NE(text.find("exported_at=2026-06-07T00:00:00Z"), std::string::npos);
      EXPECT_NE(text.find("signature=UNSIGNED"), std::string::npos);
    }

    // ---- BackendFactory ----

    TEST(BackendFactoryTest, RejectsNeitherTarget) {
      EXPECT_THROW(static_cast<void>(open_backend(BackendOptions{})), BackendOptionError);
    }

    TEST(BackendFactoryTest, RejectsBothTargets) {
      EXPECT_THROW(static_cast<void>(open_backend(BackendOptions{
                       .sqlite_path = "/tmp/x.db", .postgres_url = "postgresql://localhost/x"})),
                   BackendOptionError);
    }

    // ---- Backend-parameterized: SampleQuery + run_sample_export ----
    // Runs against SQLite and (when FMGR_TEST_POSTGRES_URL is set) Postgres, so
    // the RLS session-var injection in query_samples is exercised on a real DB.

    class CliBackendTest : public ::testing::TestWithParam<BackendKind> {
    protected:
      void SetUp() override {
        if (GetParam() == BackendKind::Postgres && !postgres_test_url().has_value()) {
          GTEST_SKIP() << "FMGR_TEST_POSTGRES_URL not set; skipping Postgres CLI tests";
        }
        fixture_ = std::make_unique<CliFixture>(GetParam());
      }

      std::unique_ptr<CliFixture> fixture_;
    };

    TEST_P(CliBackendTest, FiltersByLab) {
      const auto samples =
          query_samples(fixture_->backend(), SampleQueryOptions{.lab_id = fixture_->lab_a()});
      EXPECT_EQ(samples.size(), 2U);
      for (const auto& sample : samples) {
        EXPECT_EQ(sample.lab_id, fixture_->lab_a());
      }
    }

    TEST_P(CliBackendTest, RespectsLimit) {
      const auto samples = query_samples(
          fixture_->backend(), SampleQueryOptions{.lab_id = fixture_->lab_a(), .limit = 1U});
      EXPECT_EQ(samples.size(), 1U);
    }

    TEST_P(CliBackendTest, EmptyForLabWithNoSamples) {
      const auto samples = query_samples(
          fixture_->backend(), SampleQueryOptions{.lab_id = id_from_low<core::LabId>(999)});
      EXPECT_TRUE(samples.empty());
    }

    TEST_P(CliBackendTest, ExportEmitsHeaderAndRowPerSample) {
      std::ostringstream out;
      run_sample_export(fixture_->backend(), SampleQueryOptions{.lab_id = fixture_->lab_a()}, out);
      const std::string text = out.str();
      EXPECT_NE(text.find("# freezermanager-export"), std::string::npos);
      EXPECT_NE(text.find(fixture_->lab_a().to_string()), std::string::npos);
      // 4 comment lines + 1 csv header + 2 data rows = 7 CRLF-terminated lines.
      std::size_t line_count = 0;
      for (std::size_t pos = text.find("\r\n"); pos != std::string::npos;
           pos = text.find("\r\n", pos + 2)) {
        ++line_count;
      }
      EXPECT_EQ(line_count, 7U);
    }

    TEST_P(CliBackendTest, AuditVerifyPassesOnSeededChain) {
      // The fixture's seed inserts (labs/users/item-types/samples) each wrote an
      // audit row at commit; the chain the backend built must verify intact.
      std::ostringstream out;
      const int code = run_audit_verify(fixture_->backend(), AuditVerifyOptions{}, out);
      EXPECT_EQ(code, 0) << out.str();
      EXPECT_NE(out.str().find("OK:"), std::string::npos);
    }

    INSTANTIATE_TEST_SUITE_P(Backends, CliBackendTest,
                             ::testing::Values(BackendKind::Sqlite, BackendKind::Postgres),
                             [](const ::testing::TestParamInfo<BackendKind>& info) {
                               return backend_kind_name(info.param);
                             });

    // ---- run_cli end-to-end (SQLite, argv-level) ----

    TEST(CliAppTest, SampleExportToStdout) {
      CliFixture fixture(BackendKind::Sqlite);
      std::ostringstream out;
      std::ostringstream err;
      // Hold the argument strings alive; CLI11 reads the char* in place.
      const std::string sqlite_db = fixture.db_path();
      const std::string lab = fixture.lab_a().to_string();
      const std::vector<const char*> args = {"freezerctl",      "sample", "export",   "--sqlite",
                                             sqlite_db.c_str(), "--lab",  lab.c_str()};
      const int code = run_cli(static_cast<int>(args.size()), args.data(), out, err);
      EXPECT_EQ(code, 0) << err.str();
      EXPECT_NE(out.str().find("# freezermanager-export"), std::string::npos);
    }

    TEST(CliAppTest, MissingLabIsUsageError) {
      std::ostringstream out;
      std::ostringstream err;
      const std::array<const char*, 5> args = {"freezerctl", "sample", "export", "--sqlite",
                                               "/tmp/whatever.db"};
      const int code = run_cli(static_cast<int>(args.size()), args.data(), out, err);
      EXPECT_NE(code, 0);
    }

    TEST(CliAppTest, UnknownSubcommandIsError) {
      std::ostringstream out;
      std::ostringstream err;
      const std::array<const char*, 2> args = {"freezerctl", "bogus"};
      const int code = run_cli(static_cast<int>(args.size()), args.data(), out, err);
      EXPECT_NE(code, 0);
    }

  } // namespace
} // namespace fmgr::cli
