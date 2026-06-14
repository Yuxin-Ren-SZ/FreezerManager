// SPDX-License-Identifier: AGPL-3.0-or-later

#include "cli/AuditCommands.h"
#include "cli/BackendFactory.h"
#include "cli/CliApp.h"
#include "cli/CsvReader.h"
#include "cli/CsvWriter.h"
#include "cli/NounCommands.h"
#include "cli/SampleCommands.h"
#include "cli/SampleCsv.h"
#include "cli/SampleImport.h"
#include "cli/SampleQuery.h"

#include "core/box.h"
#include "core/freezer.h"
#include "core/identity.h"
#include "core/item_type.h"
#include "core/sample.h"
#include "storage/BoxGeometryTraits.h"
#include "storage/FreezerTraits.h"
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
#include <fstream>
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

    core::StorageContainer make_container(std::uint64_t low, core::LabId lab_id) {
      return core::StorageContainer{
          .id = id_from_low<core::StorageContainerId>(low),
          .lab_id = lab_id,
          .parent_id = std::nullopt,
          .kind = core::ContainerKind::Shelf,
          .name = "Container " + std::to_string(low),
          .ordering_index = static_cast<std::int32_t>(low),
          .created_at = ts(300 + static_cast<std::int64_t>(low)),
      };
    }

    core::Freezer make_freezer(std::uint64_t low, core::LabId lab_id,
                               core::StorageContainerId layout_root_id) {
      return core::Freezer{
          .id = id_from_low<core::FreezerId>(low),
          .lab_id = lab_id,
          .name = "Freezer " + std::to_string(low),
          .location = "Room 101",
          .model = "ULT-80",
          .temp_target_c = -80,
          .layout_root_id = layout_root_id,
          .created_at = ts(400 + static_cast<std::int64_t>(low)),
      };
    }

    core::ContainerType make_container_type(std::uint64_t low, core::LabId lab_id) {
      return core::ContainerType{
          .id = id_from_low<core::ContainerTypeId>(low),
          .lab_id = lab_id,
          .name = "Container " + std::to_string(low),
          .size_class = "cryovial_2ml",
          .material = "polypropylene",
          .supplier_sku = "SKU-" + std::to_string(low),
          .created_at = ts(500 + static_cast<std::int64_t>(low)),
      };
    }

    core::BoxType make_box_type(std::uint64_t low, core::LabId lab_id) {
      return core::BoxType{
          .id = id_from_low<core::BoxTypeId>(low),
          .lab_id = lab_id,
          .name = "BoxType " + std::to_string(low),
          .manufacturer = "Generic",
          .sku = "BT-" + std::to_string(low),
          .positions = {core::Position{
              .label = "A1", .row = 0, .col = 0, .accepts = {"cryovial_2ml"}}},
          .created_at = ts(600 + static_cast<std::int64_t>(low)),
      };
    }

    core::Box make_box(std::uint64_t low, core::LabId lab_id, core::BoxTypeId box_type_id,
                       core::StorageContainerId storage_container_id) {
      return core::Box{
          .id = id_from_low<core::BoxId>(low),
          .lab_id = lab_id,
          .box_type_id = box_type_id,
          .storage_container_id = storage_container_id,
          .label = "Box " + std::to_string(low),
          .created_at = ts(700 + static_cast<std::int64_t>(low)),
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
          // Layout prerequisites: a container-type (for box-type size_class) and
          // a root storage container per lab (referenced by freezers and boxes).
          txn->repo<core::ContainerType>().insert(make_container_type(32, lab_a_),
                                                  mutation_context());
          txn->repo<core::StorageContainer>().insert(make_container(30, lab_a_),
                                                     mutation_context());
          txn->repo<core::StorageContainer>().insert(make_container(40, lab_b_),
                                                     mutation_context());
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
          // Box-type (validates container-type) and one freezer per lab (each
          // validates its committed root container). Lab A gets a box too.
          txn->repo<core::BoxType>().insert(make_box_type(33, lab_a_), mutation_context());
          txn->repo<core::Freezer>().insert(
              make_freezer(31, lab_a_, id_from_low<core::StorageContainerId>(30)),
              mutation_context());
          txn->repo<core::Freezer>().insert(
              make_freezer(41, lab_b_, id_from_low<core::StorageContainerId>(40)),
              mutation_context());
          txn->commit();
        }
        {
          auto txn = backend_->begin(storage::IsolationLevel::Serializable);
          txn->repo<core::Box>().insert(make_box(34, lab_a_, id_from_low<core::BoxTypeId>(33),
                                                 id_from_low<core::StorageContainerId>(30)),
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

    // ---- CsvReader ----

    std::vector<std::vector<std::string>> parse(const std::string& text) {
      std::istringstream in(text);
      return parse_csv(in);
    }

    TEST(CsvReaderTest, ParsesSimpleRows) {
      const auto rows = parse("a,b,c\r\n1,2,3\r\n");
      ASSERT_EQ(rows.size(), 2U);
      EXPECT_EQ(rows.at(0), (std::vector<std::string>{"a", "b", "c"}));
      EXPECT_EQ(rows.at(1), (std::vector<std::string>{"1", "2", "3"}));
    }

    TEST(CsvReaderTest, AcceptsBareLfAndNoTrailingNewline) {
      const auto rows = parse("a,b\n1,2");
      ASSERT_EQ(rows.size(), 2U);
      EXPECT_EQ(rows.at(1), (std::vector<std::string>{"1", "2"}));
    }

    TEST(CsvReaderTest, SkipsCommentLines) {
      // Mirrors the export header block that `sample export` prepends.
      const auto rows = parse("# freezermanager-export schema_version=13\r\n"
                              "# lab_id=abc\r\n"
                              "name,item_type_id\r\n"
                              "S1,uuid\r\n");
      ASSERT_EQ(rows.size(), 2U);
      EXPECT_EQ(rows.at(0), (std::vector<std::string>{"name", "item_type_id"}));
    }

    TEST(CsvReaderTest, HandlesQuotedFieldsWithCommaNewlineAndQuotes) {
      const auto rows = parse("\"a,b\",\"line\r\nbreak\",\"say \"\"hi\"\"\"\r\n");
      ASSERT_EQ(rows.size(), 1U);
      EXPECT_EQ(rows.at(0), (std::vector<std::string>{"a,b", "line\r\nbreak", "say \"hi\""}));
    }

    TEST(CsvReaderTest, NoTrailingEmptyRecord) {
      EXPECT_EQ(parse("a\r\n").size(), 1U);
      EXPECT_EQ(parse("a\n").size(), 1U);
    }

    TEST(CsvReaderTest, ThrowsOnUnterminatedQuote) {
      EXPECT_THROW(parse("\"unterminated"), CsvParseError);
    }

    // ---- SampleImport (pure mapping/validation) ----

    ImportContext import_ctx() {
      return ImportContext{.lab_id = id_from_low<core::LabId>(1),
                           .actor = id_from_low<core::UserId>(10),
                           .now = ts(900)};
    }

    std::string item_type_uuid() {
      return id_from_low<core::ItemTypeId>(20).to_string();
    }

    TEST(SampleImportTest, BuildsValidRowWithServerControlledFields) {
      const std::vector<std::vector<std::string>> records = {
          {"name", "item_type_id"},
          {"Imported S1", item_type_uuid()},
      };
      const auto report = build_import(records, import_ctx());
      ASSERT_FALSE(report.has_errors()) << report.rows.at(0).error;
      ASSERT_EQ(report.rows.size(), 1U);
      const auto& sample = report.rows.at(0).sample.value();
      EXPECT_EQ(sample.name, "Imported S1");
      EXPECT_EQ(sample.lab_id, id_from_low<core::LabId>(1)); // from context, not file
      EXPECT_EQ(sample.created_by, id_from_low<core::UserId>(10));
      EXPECT_EQ(sample.status, core::SampleStatus::Active);
      EXPECT_EQ(sample.created_at, ts(900));
    }

    TEST(SampleImportTest, IgnoresServerManagedColumns) {
      // id/lab_id/status from the file must not override server-controlled values.
      const std::vector<std::vector<std::string>> records = {
          {"id", "lab_id", "status", "name", "item_type_id"},
          {id_from_low<core::SampleId>(7).to_string(), id_from_low<core::LabId>(999).to_string(),
           "tombstoned", "S1", item_type_uuid()},
      };
      const auto report = build_import(records, import_ctx());
      ASSERT_FALSE(report.has_errors());
      const auto& sample = report.rows.at(0).sample.value();
      EXPECT_EQ(sample.lab_id, id_from_low<core::LabId>(1));
      EXPECT_NE(sample.id, id_from_low<core::SampleId>(7)); // freshly minted
      EXPECT_EQ(sample.status, core::SampleStatus::Active);
    }

    TEST(SampleImportTest, MissingNameIsRowError) {
      const std::vector<std::vector<std::string>> records = {{"name", "item_type_id"},
                                                             {"", item_type_uuid()}};
      const auto report = build_import(records, import_ctx());
      EXPECT_TRUE(report.has_errors());
      EXPECT_FALSE(report.rows.at(0).ok);
      EXPECT_NE(report.rows.at(0).error.find("name"), std::string::npos);
    }

    TEST(SampleImportTest, BadItemTypeUuidIsRowError) {
      const std::vector<std::vector<std::string>> records = {{"name", "item_type_id"},
                                                             {"S1", "not-a-uuid"}};
      const auto report = build_import(records, import_ctx());
      EXPECT_TRUE(report.rows.at(0).error.find("item_type_id") != std::string::npos);
    }

    TEST(SampleImportTest, BoxWithoutPositionIsRowError) {
      const std::vector<std::vector<std::string>> records = {
          {"name", "item_type_id", "box_id", "position_label"},
          {"S1", item_type_uuid(), id_from_low<core::BoxId>(5).to_string(), ""}};
      const auto report = build_import(records, import_ctx());
      EXPECT_FALSE(report.rows.at(0).ok);
      EXPECT_NE(report.rows.at(0).error.find("position_label"), std::string::npos);
    }

    TEST(SampleImportTest, IntraFileDuplicatePositionIsRowError) {
      const std::string box = id_from_low<core::BoxId>(5).to_string();
      const std::vector<std::vector<std::string>> records = {
          {"name", "item_type_id", "box_id", "position_label"},
          {"S1", item_type_uuid(), box, "A1"},
          {"S2", item_type_uuid(), box, "A1"}};
      const auto report = build_import(records, import_ctx());
      EXPECT_TRUE(report.rows.at(0).ok);
      EXPECT_FALSE(report.rows.at(1).ok);
      EXPECT_NE(report.rows.at(1).error.find("duplicate position"), std::string::npos);
    }

    TEST(SampleImportTest, BadCustomFieldsJsonIsRowError) {
      const std::vector<std::vector<std::string>> records = {
          {"name", "item_type_id", "custom_fields_json"}, {"S1", item_type_uuid(), "not json"}};
      const auto report = build_import(records, import_ctx());
      EXPECT_NE(report.rows.at(0).error.find("custom_fields_json"), std::string::npos);
    }

    TEST(SampleImportTest, MissingRequiredHeaderColumnIsHeaderError) {
      const std::vector<std::vector<std::string>> records = {{"name", "barcode"}, {"S1", "BC1"}};
      const auto report = build_import(records, import_ctx());
      EXPECT_FALSE(report.header_error.empty());
      EXPECT_TRUE(report.rows.empty());
    }

    TEST(SampleImportTest, EmptyDocumentIsHeaderError) {
      const auto report = build_import({}, import_ctx());
      EXPECT_FALSE(report.header_error.empty());
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

    // The fixture seeds lab A with item-type id_from_low(20) and user
    // id_from_low(10); imports reference those so repository validation passes.
    SampleImportOptions import_options(core::LabId lab) {
      return SampleImportOptions{.lab_id = lab, .actor = id_from_low<core::UserId>(10)};
    }

    std::string two_row_csv(core::ItemTypeId item_type) {
      return "name,item_type_id\r\n"
             "Imported A," +
             item_type.to_string() + "\r\nImported B," + item_type.to_string() + "\r\n";
    }

    TEST_P(CliBackendTest, ImportPersistsRowsTransactionally) {
      const auto lab = fixture_->lab_a();
      const auto before = query_samples(fixture_->backend(), SampleQueryOptions{.lab_id = lab});
      std::istringstream in(two_row_csv(id_from_low<core::ItemTypeId>(20)));
      std::ostringstream out;
      const int code = run_sample_import(fixture_->backend(), import_options(lab), in, out);
      EXPECT_EQ(code, 0) << out.str();
      EXPECT_NE(out.str().find("imported 2 sample(s)"), std::string::npos);
      const auto after = query_samples(fixture_->backend(), SampleQueryOptions{.lab_id = lab});
      EXPECT_EQ(after.size(), before.size() + 2U);
    }

    TEST_P(CliBackendTest, ImportDryRunWritesNothing) {
      const auto lab = fixture_->lab_a();
      const auto before = query_samples(fixture_->backend(), SampleQueryOptions{.lab_id = lab});
      std::istringstream in(two_row_csv(id_from_low<core::ItemTypeId>(20)));
      std::ostringstream out;
      auto opts = import_options(lab);
      opts.dry_run = true;
      const int code = run_sample_import(fixture_->backend(), opts, in, out);
      EXPECT_EQ(code, 0) << out.str();
      EXPECT_NE(out.str().find("dry-run OK"), std::string::npos);
      const auto after = query_samples(fixture_->backend(), SampleQueryOptions{.lab_id = lab});
      EXPECT_EQ(after.size(), before.size());
    }

    TEST_P(CliBackendTest, ImportRejectsUnknownItemTypeAtDbLayer) {
      const auto lab = fixture_->lab_a();
      const auto before = query_samples(fixture_->backend(), SampleQueryOptions{.lab_id = lab});
      // Structurally valid UUID, but not a live item type in this lab.
      std::istringstream in("name,item_type_id\r\nBad," +
                            id_from_low<core::ItemTypeId>(777).to_string() + "\r\n");
      std::ostringstream out;
      auto opts = import_options(lab);
      opts.dry_run = true;
      const int code = run_sample_import(fixture_->backend(), opts, in, out);
      EXPECT_EQ(code, 1) << out.str();
      const auto after = query_samples(fixture_->backend(), SampleQueryOptions{.lab_id = lab});
      EXPECT_EQ(after.size(), before.size());
    }

    // ---- Read nouns: freezer / box / item-type list + inspect ----

    TEST_P(CliBackendTest, FreezerListReturnsLabRowsOnly) {
      std::ostringstream out;
      run_freezer_list(fixture_->backend(), NounListOptions{.lab_id = fixture_->lab_a()}, out);
      const std::string text = out.str();
      EXPECT_NE(text.find(id_from_low<core::FreezerId>(31).to_string()), std::string::npos);
      EXPECT_NE(text.find("1 freezer(s)"), std::string::npos);
      // Lab B's freezer must not leak into lab A's listing.
      EXPECT_EQ(text.find(id_from_low<core::FreezerId>(41).to_string()), std::string::npos);
    }

    TEST_P(CliBackendTest, BoxListReturnsLabRows) {
      std::ostringstream out;
      run_box_list(fixture_->backend(), NounListOptions{.lab_id = fixture_->lab_a()}, out);
      const std::string text = out.str();
      EXPECT_NE(text.find(id_from_low<core::BoxId>(34).to_string()), std::string::npos);
      EXPECT_NE(text.find("1 box(es)"), std::string::npos);
    }

    TEST_P(CliBackendTest, ItemTypeListReturnsLabRows) {
      std::ostringstream out;
      run_item_type_list(fixture_->backend(), NounListOptions{.lab_id = fixture_->lab_a()}, out);
      const std::string text = out.str();
      EXPECT_NE(text.find(id_from_low<core::ItemTypeId>(20).to_string()), std::string::npos);
      EXPECT_NE(text.find("1 item-type(s)"), std::string::npos);
    }

    TEST_P(CliBackendTest, BoxListExcludesTombstonedUnlessRequested) {
      // Tombstone the seeded box, then confirm default listing hides it and the
      // include-tombstoned flag surfaces it again.
      {
        auto txn = fixture_->backend().begin(storage::IsolationLevel::Serializable);
        txn->set_session_var("current_lab_ids", fixture_->lab_a().to_string());
        txn->repo<core::Box>().soft_delete(id_from_low<core::BoxId>(34), mutation_context());
        txn->commit();
      }
      std::ostringstream hidden;
      run_box_list(fixture_->backend(), NounListOptions{.lab_id = fixture_->lab_a()}, hidden);
      EXPECT_NE(hidden.str().find("0 box(es)"), std::string::npos);

      std::ostringstream shown;
      run_box_list(fixture_->backend(),
                   NounListOptions{.lab_id = fixture_->lab_a(), .include_tombstoned = true}, shown);
      EXPECT_NE(shown.str().find("1 box(es)"), std::string::npos);
    }

    TEST_P(CliBackendTest, FreezerInspectPrintsDetail) {
      std::ostringstream out;
      const int code = run_freezer_inspect(fixture_->backend(), fixture_->lab_a(),
                                           id_from_low<core::FreezerId>(31), out);
      EXPECT_EQ(code, 0) << out.str();
      EXPECT_NE(out.str().find("Freezer 31"), std::string::npos);
      EXPECT_NE(out.str().find("ULT-80"), std::string::npos);
    }

    TEST_P(CliBackendTest, FreezerInspectUnknownIdIsNotFound) {
      std::ostringstream out;
      const int code = run_freezer_inspect(fixture_->backend(), fixture_->lab_a(),
                                           id_from_low<core::FreezerId>(888), out);
      EXPECT_EQ(code, 1);
      EXPECT_NE(out.str().find("not found"), std::string::npos);
    }

    TEST_P(CliBackendTest, FreezerInspectCrossLabIsNotFound) {
      // Freezer 41 belongs to lab B; inspecting it under lab A must report
      // not-found rather than disclose another lab's row.
      std::ostringstream out;
      const int code = run_freezer_inspect(fixture_->backend(), fixture_->lab_a(),
                                           id_from_low<core::FreezerId>(41), out);
      EXPECT_EQ(code, 1);
      EXPECT_NE(out.str().find("not found"), std::string::npos);
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

    TEST(CliAppTest, SampleImportFromFile) {
      CliFixture fixture(BackendKind::Sqlite);
      const auto item_type = id_from_low<core::ItemTypeId>(20).to_string();
      const auto csv_path =
          std::filesystem::temp_directory_path() / "freezermanager-import-test.csv";
      {
        std::ofstream csv(csv_path, std::ios::binary | std::ios::trunc);
        csv << "name,item_type_id\r\nFromFile," << item_type << "\r\n";
      }
      std::ostringstream out;
      std::ostringstream err;
      const std::string sqlite_db = fixture.db_path();
      const std::string lab = fixture.lab_a().to_string();
      const std::string actor = id_from_low<core::UserId>(10).to_string();
      const std::string file = csv_path.string();
      const std::vector<const char*> args = {
          "freezerctl", "sample",    "import",  "--sqlite",    sqlite_db.c_str(),
          "--lab",      lab.c_str(), "--actor", actor.c_str(), file.c_str()};
      const int code = run_cli(static_cast<int>(args.size()), args.data(), out, err);
      std::filesystem::remove(csv_path);
      EXPECT_EQ(code, 0) << err.str() << out.str();
      EXPECT_NE(out.str().find("imported 1 sample(s)"), std::string::npos);
    }

    TEST(CliAppTest, FreezerListToStdout) {
      CliFixture fixture(BackendKind::Sqlite);
      std::ostringstream out;
      std::ostringstream err;
      const std::string sqlite_db = fixture.db_path();
      const std::string lab = fixture.lab_a().to_string();
      const std::vector<const char*> args = {"freezerctl",      "freezer", "list",     "--sqlite",
                                             sqlite_db.c_str(), "--lab",   lab.c_str()};
      const int code = run_cli(static_cast<int>(args.size()), args.data(), out, err);
      EXPECT_EQ(code, 0) << err.str();
      EXPECT_NE(out.str().find(id_from_low<core::FreezerId>(31).to_string()), std::string::npos);
    }

    TEST(CliAppTest, BoxInspectByIdFromArgv) {
      CliFixture fixture(BackendKind::Sqlite);
      std::ostringstream out;
      std::ostringstream err;
      const std::string sqlite_db = fixture.db_path();
      const std::string lab = fixture.lab_a().to_string();
      const std::string box_id = id_from_low<core::BoxId>(34).to_string();
      const std::vector<const char*> args = {"freezerctl",      "box",   "inspect",   "--sqlite",
                                             sqlite_db.c_str(), "--lab", lab.c_str(), "--id",
                                             box_id.c_str()};
      const int code = run_cli(static_cast<int>(args.size()), args.data(), out, err);
      EXPECT_EQ(code, 0) << err.str() << out.str();
      EXPECT_NE(out.str().find("Box 34"), std::string::npos);
    }

    TEST(CliAppTest, ItemTypeInspectUnknownReturnsOne) {
      CliFixture fixture(BackendKind::Sqlite);
      std::ostringstream out;
      std::ostringstream err;
      const std::string sqlite_db = fixture.db_path();
      const std::string lab = fixture.lab_a().to_string();
      const std::string absent = id_from_low<core::ItemTypeId>(909).to_string();
      const std::vector<const char*> args = {
          "freezerctl", "item-type", "inspect", "--sqlite",    sqlite_db.c_str(),
          "--lab",      lab.c_str(), "--id",    absent.c_str()};
      const int code = run_cli(static_cast<int>(args.size()), args.data(), out, err);
      EXPECT_EQ(code, 1);
      EXPECT_NE(out.str().find("not found"), std::string::npos);
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
