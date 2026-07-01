// SPDX-License-Identifier: AGPL-3.0-or-later

#include "backup/BackupCommands.h"
#include "backup/PostgresDump.h"
#include "backup/SqliteBackup.h"
#include "cli/AuditCommands.h"
#include "cli/AuditCsv.h"
#include "cli/BackendFactory.h"
#include "cli/BoxImport.h"
#include "cli/CliApp.h"
#include "cli/CreateCommands.h"
#include "cli/CsvImport.h"
#include "cli/CsvReader.h"
#include "cli/CsvWriter.h"
#include "cli/CustomFieldDefImport.h"
#include "cli/EntityRead.h"
#include "cli/ItemTypeImport.h"
#include "cli/KeyCommands.h"
#include "cli/LabCommands.h"
#include "cli/NounCommands.h"
#include "cli/SampleCommands.h"
#include "cli/SampleCsv.h"
#include "cli/SampleImport.h"
#include "cli/SampleQuery.h"
#include "cli/UserImport.h"

#include "core/audit_event.h"
#include "core/box.h"
#include "core/freezer.h"
#include "core/identity.h"
#include "core/item_type.h"
#include "core/role.h"
#include "core/sample.h"
#include "crypto/FieldCipher.h"
#include "kms/EnvVarKms.h"
#include "storage/AuditTraits.h"
#include "storage/BoxGeometryTraits.h"
#include "storage/FreezerTraits.h"
#include "storage/IdentityTraits.h"
#include "storage/ItemTypeTraits.h"
#include "storage/SampleTraits.h"

#include <nlohmann/json.hpp>

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
    using namespace fmgr::backup; // run_backup_*, BackupError moved to the backup library

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
        backend_.reset(); // close pool before dropping the schema
        if (kind_ == BackendKind::Sqlite) {
          remove_sqlite_files(db_path_);
          return;
        }
        if (schema_name_.empty()) {
          return;
        }
        try {
          pqxx::connection conn(postgres_test_url().value());
          pqxx::work txn(conn);
          txn.exec("DROP SCHEMA IF EXISTS " + txn.quote_name(schema_name_) + " CASCADE");
          txn.commit();
        } catch (...) { // NOLINT(bugprone-empty-catch): best-effort cleanup in dtor
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
      // Postgres schema this fixture migrated into. Empty for SQLite. Callers that
      // reopen the backend by raw URL must pin the search_path to it.
      [[nodiscard]] std::string postgres_schema() const {
        return schema_name_;
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
        // Per-process isolation: give this fixture its own schema (unique across
        // parallel ctest processes) and pin the backend's search_path to it, so
        // open_backend() migrates a private schema instead of racing on the
        // shared `public` one (mirrors RepoBackendHarness).
        schema_name_ = unique_postgres_schema("cli_test");
        pqxx::connection setup_conn(url);
        pqxx::work txn(setup_conn);
        txn.exec("DROP SCHEMA IF EXISTS " + txn.quote_name(schema_name_) + " CASCADE");
        txn.exec("CREATE SCHEMA " + txn.quote_name(schema_name_));
        txn.commit();
        return BackendOptions{.postgres_url = postgres_url_with_schema(url, schema_name_)};
      }

      static std::filesystem::path unique_path() {
        static std::atomic<std::uint64_t> counter{0};
        return std::filesystem::temp_directory_path() /
               ("freezermanager-cli-" + std::to_string(counter.fetch_add(1)) + ".db");
      }

      BackendKind kind_;
      std::filesystem::path db_path_;
      std::string schema_name_;
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

    // ---- CsvReader resource limits (review F-8) ----

    TEST(CsvReaderTest, RejectsOverlongField) {
      CsvLimits limits;
      limits.max_field_length = 8;
      std::istringstream in(std::string(100, 'x') + "\n");
      EXPECT_THROW((void)parse_csv(in, limits), CsvParseError);
    }

    TEST(CsvReaderTest, RejectsTooManyFieldsPerRow) {
      CsvLimits limits;
      limits.max_fields_per_row = 3;
      std::istringstream in("a,b,c,d\n");
      EXPECT_THROW((void)parse_csv(in, limits), CsvParseError);
    }

    TEST(CsvReaderTest, RejectsTooManyRows) {
      CsvLimits limits;
      limits.max_rows = 2;
      std::istringstream in("1\n2\n3\n");
      EXPECT_THROW((void)parse_csv(in, limits), CsvParseError);
    }

    TEST(CsvReaderTest, AcceptsInputWithinLimits) {
      CsvLimits limits;
      limits.max_field_length = 16;
      limits.max_fields_per_row = 4;
      limits.max_rows = 4;
      std::istringstream in("a,b\n1,2\n");
      EXPECT_EQ(parse_csv(in, limits).size(), 2U);
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

    // ---- ItemTypeImport / BoxImport (pure mappers) ----

    TEST(ItemTypeImportTest, MapsValidRow) {
      const auto records = parse("name\r\nblood\r\n");
      const auto report = build_item_type_import(records, id_from_low<core::LabId>(1), ts(1));
      ASSERT_EQ(report.rows.size(), 1U);
      ASSERT_TRUE(report.rows.at(0).ok);
      ASSERT_TRUE(report.rows.at(0).entity.has_value());
      EXPECT_EQ(report.rows.at(0).entity->name, "blood");
      EXPECT_EQ(report.rows.at(0).entity->lab_id, id_from_low<core::LabId>(1));
    }

    TEST(ItemTypeImportTest, MissingNameColumnIsHeaderError) {
      const auto report =
          build_item_type_import(parse("foo\r\nbar\r\n"), id_from_low<core::LabId>(1), ts(1));
      EXPECT_FALSE(report.header_error.empty());
      EXPECT_TRUE(report.rows.empty());
    }

    TEST(ItemTypeImportTest, EmptyNameIsRowError) {
      const auto report = build_item_type_import(parse("name,parent_id\r\n,\r\n"),
                                                 id_from_low<core::LabId>(1), ts(1));
      ASSERT_EQ(report.rows.size(), 1U);
      EXPECT_FALSE(report.rows.at(0).ok);
    }

    TEST(ItemTypeImportTest, BadParentUuidIsRowError) {
      const auto report = build_item_type_import(parse("name,parent_id\r\nblood,not-a-uuid\r\n"),
                                                 id_from_low<core::LabId>(1), ts(1));
      ASSERT_EQ(report.rows.size(), 1U);
      EXPECT_FALSE(report.rows.at(0).ok);
      EXPECT_NE(report.rows.at(0).error.find("parent_id"), std::string::npos);
    }

    TEST(BoxImportTest, MapsValidRow) {
      const std::string box_type = id_from_low<core::BoxTypeId>(5).to_string();
      const std::string container = id_from_low<core::StorageContainerId>(6).to_string();
      const auto report = build_box_import(parse("label,box_type_id,storage_container_id\r\nB1," +
                                                 box_type + "," + container + "\r\n"),
                                           id_from_low<core::LabId>(1), ts(1));
      ASSERT_EQ(report.rows.size(), 1U);
      ASSERT_TRUE(report.rows.at(0).ok);
      ASSERT_TRUE(report.rows.at(0).entity.has_value());
      EXPECT_EQ(report.rows.at(0).entity->label, "B1");
    }

    TEST(BoxImportTest, MissingRequiredColumnIsHeaderError) {
      const auto report =
          build_box_import(parse("label\r\nB1\r\n"), id_from_low<core::LabId>(1), ts(1));
      EXPECT_FALSE(report.header_error.empty());
    }

    TEST(BoxImportTest, BadBoxTypeUuidIsRowError) {
      const std::string container = id_from_low<core::StorageContainerId>(6).to_string();
      const auto report = build_box_import(
          parse("label,box_type_id,storage_container_id\r\nB1,not-a-uuid," + container + "\r\n"),
          id_from_low<core::LabId>(1), ts(1));
      ASSERT_EQ(report.rows.size(), 1U);
      EXPECT_FALSE(report.rows.at(0).ok);
      EXPECT_NE(report.rows.at(0).error.find("box_type_id"), std::string::npos);
    }

    // ---- CustomFieldDefImport / UserImport (pure mappers) ----

    TEST(CustomFieldDefImportTest, MapsValidRow) {
      const auto report = build_custom_field_def_import(
          parse("scope_kind,key,label,data_type\r\nsample,patient_id,Patient ID,string\r\n"),
          id_from_low<core::LabId>(1), ts(1));
      ASSERT_EQ(report.rows.size(), 1U);
      ASSERT_TRUE(report.rows.at(0).ok) << report.rows.at(0).error;
      ASSERT_TRUE(report.rows.at(0).entity.has_value());
      EXPECT_EQ(report.rows.at(0).entity->key, "patient_id");
      EXPECT_EQ(report.rows.at(0).entity->data_type, core::FieldDataType::String);
    }

    TEST(CustomFieldDefImportTest, MissingDataTypeColumnIsHeaderError) {
      const auto report = build_custom_field_def_import(
          parse("scope_kind,key,label\r\nsample,k,L\r\n"), id_from_low<core::LabId>(1), ts(1));
      EXPECT_FALSE(report.header_error.empty());
    }

    TEST(CustomFieldDefImportTest, BadDataTypeIsRowError) {
      const auto report = build_custom_field_def_import(
          parse("scope_kind,key,label,data_type\r\nsample,k,L,bogus\r\n"),
          id_from_low<core::LabId>(1), ts(1));
      ASSERT_EQ(report.rows.size(), 1U);
      EXPECT_FALSE(report.rows.at(0).ok);
      EXPECT_NE(report.rows.at(0).error.find("data_type"), std::string::npos);
    }

    TEST(CustomFieldDefImportTest, PhiIndexedConflictIsRowError) {
      const auto report =
          build_custom_field_def_import(parse("scope_kind,key,label,data_type,is_phi,indexed\r\n"
                                              "sample,k,L,string,true,true\r\n"),
                                        id_from_low<core::LabId>(1), ts(1));
      ASSERT_EQ(report.rows.size(), 1U);
      EXPECT_FALSE(report.rows.at(0).ok);
      EXPECT_NE(report.rows.at(0).error.find("indexed"), std::string::npos);
    }

    TEST(UserImportTest, MapsValidRow) {
      const auto report =
          build_user_import(parse("primary_email,display_name\r\na@b.edu,Alice\r\n"),
                            id_from_low<core::LabId>(1), ts(1));
      ASSERT_EQ(report.rows.size(), 1U);
      ASSERT_TRUE(report.rows.at(0).ok);
      ASSERT_TRUE(report.rows.at(0).entity.has_value());
      EXPECT_EQ(report.rows.at(0).entity->primary_email, "a@b.edu");
      EXPECT_EQ(report.rows.at(0).entity->status, core::UserStatus::Active);
    }

    TEST(UserImportTest, MissingDisplayNameColumnIsHeaderError) {
      const auto report = build_user_import(parse("primary_email\r\na@b.edu\r\n"),
                                            id_from_low<core::LabId>(1), ts(1));
      EXPECT_FALSE(report.header_error.empty());
    }

    TEST(UserImportTest, EmptyEmailIsRowError) {
      const auto report = build_user_import(parse("primary_email,display_name\r\n,Bob\r\n"),
                                            id_from_low<core::LabId>(1), ts(1));
      ASSERT_EQ(report.rows.size(), 1U);
      EXPECT_FALSE(report.rows.at(0).ok);
    }

    TEST(BackendFactoryTest, RejectsNeitherTarget) {
      EXPECT_THROW(static_cast<void>(open_backend(BackendOptions{})), BackendOptionError);
    }

    TEST(BackendFactoryTest, RejectsBothTargets) {
      EXPECT_THROW(static_cast<void>(open_backend(BackendOptions{
                       .sqlite_path = "/tmp/x.db", .postgres_url = "postgresql://localhost/x"})),
                   BackendOptionError);
    }

    TEST(BackendFactoryTest, RejectsSqlitePathEscapingDataDir) {
      // A `..` traversal out of the configured data directory is rejected (F-10).
      EXPECT_THROW(static_cast<void>(open_backend(BackendOptions{
                       .sqlite_path = "/tmp/data/../../etc/evil.db", .data_dir = "/tmp/data"})),
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

    // ---- audit export ----

    // Read every audit event from the backend, ordered as the verifier expects.
    std::vector<core::AuditEvent> read_audit_events(storage::IStorageBackend& backend) {
      auto txn = backend.begin(storage::IsolationLevel::Serializable);
      auto events = txn->repo<core::AuditEvent>().query(
          storage::Query<core::AuditEvent>::all()
              .order_by(
                  storage::field<core::AuditEvent, core::Timestamp>(core::AuditEvent::Field::At),
                  storage::SortDirection::Ascending)
              .order_by(storage::field<core::AuditEvent, std::string>(core::AuditEvent::Field::Id),
                        storage::SortDirection::Ascending));
      txn->commit();
      return events;
    }

    // Count the lines terminated by CRLF in a CSV export body.
    std::size_t crlf_line_count(const std::string& text) {
      std::size_t count = 0;
      for (std::size_t pos = text.find("\r\n"); pos != std::string::npos;
           pos = text.find("\r\n", pos + 2)) {
        ++count;
      }
      return count;
    }

    TEST_P(CliBackendTest, AuditExportEmitsHeaderAndRowPerEvent) {
      const std::size_t seeded = read_audit_events(fixture_->backend()).size();
      ASSERT_GT(seeded, 0U);

      std::ostringstream out;
      const int code = run_audit_export(
          fixture_->backend(), AuditExportOptions{.actor = id_from_low<core::UserId>(10)}, out);
      EXPECT_EQ(code, 0);

      const std::string text = out.str();
      EXPECT_NE(text.find("# freezermanager-audit-export"), std::string::npos);
      EXPECT_NE(text.find("event_count=" + std::to_string(seeded)), std::string::npos);
      // 4 comment lines + 1 csv header + one row per seeded event.
      EXPECT_EQ(crlf_line_count(text), 4U + 1U + seeded);
    }

    TEST_P(CliBackendTest, AuditExportIsItselfAudited) {
      const std::size_t before = read_audit_events(fixture_->backend()).size();

      std::ostringstream out;
      ASSERT_EQ(run_audit_export(fixture_->backend(),
                                 AuditExportOptions{.actor = id_from_low<core::UserId>(10)}, out),
                0);

      const auto after = read_audit_events(fixture_->backend());
      ASSERT_EQ(after.size(), before + 1U);
      EXPECT_EQ(after.back().action, "audit.export");
      EXPECT_EQ(after.back().entity_kind, "audit");
    }

    TEST_P(CliBackendTest, AuditExportLeavesChainVerifiable) {
      std::ostringstream sink;
      ASSERT_EQ(run_audit_export(fixture_->backend(),
                                 AuditExportOptions{.actor = id_from_low<core::UserId>(10)}, sink),
                0);
      std::ostringstream verify_out;
      EXPECT_EQ(run_audit_verify(fixture_->backend(), AuditVerifyOptions{}, verify_out), 0)
          << verify_out.str();
    }

    TEST_P(CliBackendTest, AuditExportFiltersByLab) {
      std::ostringstream out;
      ASSERT_EQ(run_audit_export(fixture_->backend(),
                                 AuditExportOptions{.lab_id = fixture_->lab_a(),
                                                    .actor = id_from_low<core::UserId>(10)},
                                 out),
                0);
      const std::string text = out.str();
      // A lab-scoped export must not leak another lab's events.
      EXPECT_EQ(text.find(fixture_->lab_b().to_string()), std::string::npos);
      EXPECT_NE(text.find(fixture_->lab_a().to_string()), std::string::npos);
      EXPECT_NE(text.find("lab_filter=" + fixture_->lab_a().to_string()), std::string::npos);
    }

    // ---- AuditCsv pure mapping ----

    TEST(AuditCsvTest, ColumnsIncludeHashChain) {
      const auto& columns = audit_csv_columns();
      EXPECT_EQ(columns.front(), "id");
      EXPECT_NE(std::find(columns.begin(), columns.end(), "prev_hash"), columns.end());
      EXPECT_NE(std::find(columns.begin(), columns.end(), "this_hash"), columns.end());
    }

    TEST(AuditCsvTest, RowMatchesColumnArityAndRendersScalars) {
      core::AuditEvent event;
      event.id = id_from_low<core::AuditEventId>(7);
      event.at = core::Timestamp::from_unix_micros(1234567);
      event.actor_user_id = id_from_low<core::UserId>(10);
      event.actor_session_id = "sess";
      event.lab_id = std::nullopt;
      event.action = "insert";
      event.entity_kind = "lab";
      event.entity_id = std::nullopt;
      event.request_id = "req";
      event.prev_hash = std::string(64, '0');
      event.this_hash = std::string(64, 'a');

      const auto row = audit_event_to_csv_row(event);
      ASSERT_EQ(row.size(), audit_csv_columns().size());
      // `at` renders as raw micros; absent optionals render empty.
      const auto at_index = static_cast<std::size_t>(
          std::find(audit_csv_columns().begin(), audit_csv_columns().end(), "at") -
          audit_csv_columns().begin());
      EXPECT_EQ(row.at(at_index), "1234567");
      const auto lab_index = static_cast<std::size_t>(
          std::find(audit_csv_columns().begin(), audit_csv_columns().end(), "lab_id") -
          audit_csv_columns().begin());
      EXPECT_EQ(row.at(lab_index), "");
    }

    TEST(AuditCsvTest, WriteCsvEmitsHeaderBlockThenRows) {
      core::AuditEvent event;
      event.id = id_from_low<core::AuditEventId>(1);
      event.prev_hash = std::string(64, '0');
      event.this_hash = std::string(64, 'b');
      std::ostringstream out;
      write_audit_csv(out, {event}, 13, "all", "2026-06-21T00:00:00Z");
      const std::string text = out.str();
      EXPECT_NE(text.find("# freezermanager-audit-export schema_version=13"), std::string::npos);
      EXPECT_NE(text.find("lab_filter=all"), std::string::npos);
      EXPECT_NE(text.find("event_count=1"), std::string::npos);
      EXPECT_NE(text.find("signature=UNSIGNED"), std::string::npos);
    }

    // ---- key rotate ----

    constexpr const char* kOldKekB64 = "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8=";
    constexpr const char* kNewKekB64 = "QEFCQ0RFRkdISUpLTE1OT1BRUlNUVVZXWFlaW1xdXl8=";

    std::vector<std::uint8_t> kek_bytes(std::uint8_t base) {
      std::vector<std::uint8_t> kek(32);
      for (std::size_t i = 0; i < kek.size(); ++i) {
        kek[i] = static_cast<std::uint8_t>(base + i);
      }
      return kek;
    }
    std::vector<std::uint8_t> old_kek() {
      return kek_bytes(0x00);
    } // == kKekB64
    std::vector<std::uint8_t> new_kek() {
      return kek_bytes(0x40);
    } // == kNewKekB64

    // Insert an active sample whose PHI envelope is wrapped under `kms`.
    core::SampleId insert_phi_sample(storage::IStorageBackend& backend, core::LabId lab,
                                     const fmgr::kms::IKmsProvider& kms, std::uint64_t low) {
      core::Sample sample =
          make_sample(low, lab, id_from_low<core::ItemTypeId>(20), id_from_low<core::UserId>(10));
      sample.phi_fields_enc_json =
          crypto::encrypt(crypto::PhiFields{{"mrn", "MRN-" + std::to_string(low)}}, kms);
      auto txn = backend.begin(storage::IsolationLevel::Serializable);
      txn->set_session_var("current_lab_ids", lab.to_string());
      txn->repo<core::Sample>().insert(sample, mutation_context());
      txn->commit();
      return sample.id;
    }

    TEST_P(CliBackendTest, KeyRotateRewrapsPhiToActiveKekAndAudits) {
      auto& backend = fixture_->backend();
      const auto lab = fixture_->lab_a();
      const fmgr::kms::EnvVarKms old_kms(old_kek());
      const auto sid = insert_phi_sample(backend, lab, old_kms, 555);

      // Active KEK is NEW; OLD retained as retired so the existing DEK can unwrap.
      const fmgr::kms::EnvVarKms new_kms(new_kek(), {old_kek()});
      std::ostringstream sink;
      const auto report =
          rotate_phi_keys(backend, new_kms, lab, id_from_low<core::UserId>(10), sink);
      EXPECT_EQ(report.scanned, 1U); // the two non-PHI seed samples are not scanned
      EXPECT_EQ(report.rewrapped, 1U);
      EXPECT_EQ(report.current, 0U);
      EXPECT_EQ(report.failed, 0U);

      auto txn = backend.begin(storage::IsolationLevel::ReadCommitted);
      txn->set_session_var("current_lab_ids", lab.to_string());
      const auto row = txn->repo<core::Sample>().find_by_id(sid);
      const auto events =
          txn->repo<core::AuditEvent>().query(storage::Query<core::AuditEvent>::all());
      txn->commit();

      ASSERT_TRUE(row.has_value());
      const auto envelope = nlohmann::json::parse(row->phi_fields_enc_json);
      EXPECT_EQ(envelope.at("kek_id").get<std::string>(), new_kms.key_id());
      EXPECT_EQ(crypto::decrypt(row->phi_fields_enc_json, new_kms).at("mrn"), "MRN-555");

      // Rotation is audited as a Sample update; its after-image records the
      // re-wrapped envelope (new kek_id) and never the plaintext PHI value.
      int rotate_events = 0;
      for (const auto& event : events) {
        if (event.entity_kind == "sample" && event.entity_id == sid.to_string() &&
            event.action == "update") {
          ++rotate_events;
          EXPECT_NE(event.after_json.find(new_kms.key_id()), std::string::npos);
          EXPECT_EQ(event.after_json.find("MRN-555"), std::string::npos);
        }
      }
      EXPECT_EQ(rotate_events, 1);

      // A second rotation under the same active KEK is a no-op.
      std::ostringstream sink2;
      const auto again =
          rotate_phi_keys(backend, new_kms, lab, id_from_low<core::UserId>(10), sink2);
      EXPECT_EQ(again.rewrapped, 0U);
      EXPECT_EQ(again.current, 1U);
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

    // ---- non-sample CSV import (item-type / box) ----

    TEST_P(CliBackendTest, ItemTypeImportPersistsTransactionally) {
      const auto lab = fixture_->lab_a();
      const auto before = query_in_lab<core::ItemType>(fixture_->backend(), lab);
      std::istringstream in("name\r\nblood\r\ncsf\r\n");
      std::ostringstream out;
      const EntityImportOptions opts{.lab_id = lab, .actor = id_from_low<core::UserId>(10)};
      const int code = run_entity_import<core::ItemType>(
          fixture_->backend(), opts, build_item_type_import, in, out, "item-type(s)");
      EXPECT_EQ(code, 0) << out.str();
      EXPECT_NE(out.str().find("imported 2 item-type(s)"), std::string::npos);
      const auto after = query_in_lab<core::ItemType>(fixture_->backend(), lab);
      EXPECT_EQ(after.size(), before.size() + 2U);
    }

    TEST_P(CliBackendTest, BoxImportPersistsWithSeededRefs) {
      const auto lab = fixture_->lab_a();
      const auto before = query_in_lab<core::Box>(fixture_->backend(), lab);
      const std::string box_type = id_from_low<core::BoxTypeId>(33).to_string();
      const std::string container = id_from_low<core::StorageContainerId>(30).to_string();
      std::istringstream in("label,box_type_id,storage_container_id\r\nB-imp," + box_type + "," +
                            container + "\r\n");
      std::ostringstream out;
      const EntityImportOptions opts{.lab_id = lab, .actor = id_from_low<core::UserId>(10)};
      const int code = run_entity_import<core::Box>(fixture_->backend(), opts, build_box_import, in,
                                                    out, "box(es)");
      EXPECT_EQ(code, 0) << out.str();
      EXPECT_NE(out.str().find("imported 1 box(es)"), std::string::npos);
      const auto after = query_in_lab<core::Box>(fixture_->backend(), lab);
      EXPECT_EQ(after.size(), before.size() + 1U);
    }

    TEST_P(CliBackendTest, BoxImportRejectsUnknownBoxTypeAtDbLayer) {
      const auto lab = fixture_->lab_a();
      const std::string bad_box_type = id_from_low<core::BoxTypeId>(777).to_string();
      const std::string container = id_from_low<core::StorageContainerId>(30).to_string();
      std::istringstream in("label,box_type_id,storage_container_id\r\nBad," + bad_box_type + "," +
                            container + "\r\n");
      std::ostringstream out;
      const EntityImportOptions opts{
          .lab_id = lab, .actor = id_from_low<core::UserId>(10), .dry_run = true};
      const int code = run_entity_import<core::Box>(fixture_->backend(), opts, build_box_import, in,
                                                    out, "box(es)");
      EXPECT_EQ(code, 1) << out.str();
    }

    TEST_P(CliBackendTest, CustomFieldDefImportPersists) {
      const auto lab = fixture_->lab_a();
      const auto before = query_in_lab<core::CustomFieldDefinition>(fixture_->backend(), lab);
      std::istringstream in("scope_kind,key,label,data_type\r\nsample,donor_age,Donor Age,int\r\n");
      std::ostringstream out;
      const EntityImportOptions opts{.lab_id = lab, .actor = id_from_low<core::UserId>(10)};
      const int code = run_entity_import<core::CustomFieldDefinition>(
          fixture_->backend(), opts, build_custom_field_def_import, in, out, "custom-field-def(s)");
      EXPECT_EQ(code, 0) << out.str();
      const auto after = query_in_lab<core::CustomFieldDefinition>(fixture_->backend(), lab);
      EXPECT_EQ(after.size(), before.size() + 1U);
    }

    TEST_P(CliBackendTest, UserImportPersists) {
      const auto lab = fixture_->lab_a();
      std::istringstream in("primary_email,display_name\r\nimported@example.edu,Imported\r\n");
      std::ostringstream out;
      const EntityImportOptions opts{.lab_id = lab, .actor = id_from_low<core::UserId>(10)};
      const int code = run_entity_import<core::User>(fixture_->backend(), opts, build_user_import,
                                                     in, out, "user(s)");
      EXPECT_EQ(code, 0) << out.str();
      EXPECT_NE(out.str().find("imported 1 user(s)"), std::string::npos);
      // Users are global: verify by email lookup (no lab scope on the User row).
      auto txn = fixture_->backend().begin(storage::IsolationLevel::Serializable);
      const auto found = txn->repo<core::User>().query(storage::Query<core::User>::where(
          storage::field<core::User, std::string>(core::User::Field::PrimaryEmail) ==
          std::string("imported@example.edu")));
      txn->commit();
      EXPECT_EQ(found.size(), 1U);
    }

    // ---- create nouns (write half of the M1 CLI nouns) ----

    CreateCommon create_common(core::LabId lab) {
      return CreateCommon{.lab_id = lab, .actor = id_from_low<core::UserId>(10)};
    }

    TEST_P(CliBackendTest, ItemTypeCreatePersistsRootNode) {
      std::ostringstream out;
      const ItemTypeCreateOptions opts{.common = create_common(fixture_->lab_a()),
                                       .name = "tissue"};
      const auto id = run_item_type_create(fixture_->backend(), opts, out);
      EXPECT_NE(out.str().find("created item-type " + id.to_string()), std::string::npos);
      const auto found = find_in_lab<core::ItemType>(fixture_->backend(), fixture_->lab_a(), id);
      ASSERT_TRUE(found.has_value());
      EXPECT_EQ(found->name, "tissue");
      EXPECT_FALSE(found->parent_id.has_value());
    }

    TEST_P(CliBackendTest, ContainerTypeCreatePersists) {
      std::ostringstream out;
      const ContainerTypeCreateOptions opts{.common = create_common(fixture_->lab_a()),
                                            .name = "15mL Falcon",
                                            .size_class = "tube_15ml"};
      const auto id = run_container_type_create(fixture_->backend(), opts, out);
      const auto found =
          find_in_lab<core::ContainerType>(fixture_->backend(), fixture_->lab_a(), id);
      ASSERT_TRUE(found.has_value());
      EXPECT_EQ(found->size_class, "tube_15ml");
    }

    TEST_P(CliBackendTest, StorageContainerCreatePersists) {
      std::ostringstream out;
      const StorageContainerCreateOptions opts{.common = create_common(fixture_->lab_a()),
                                               .name = "Shelf 2",
                                               .kind = core::ContainerKind::Shelf};
      const auto id = run_storage_container_create(fixture_->backend(), opts, out);
      const auto found =
          find_in_lab<core::StorageContainer>(fixture_->backend(), fixture_->lab_a(), id);
      ASSERT_TRUE(found.has_value());
      EXPECT_EQ(found->kind, core::ContainerKind::Shelf);
    }

    TEST_P(CliBackendTest, FreezerCreatePersistsWithSeededLayoutRoot) {
      std::ostringstream out;
      // Container 30 is the seeded root storage container in lab A.
      const FreezerCreateOptions opts{.common = create_common(fixture_->lab_a()),
                                      .name = "Cryo-1",
                                      .model = "ULT-86",
                                      .layout_root_id = id_from_low<core::StorageContainerId>(30)};
      const auto id = run_freezer_create(fixture_->backend(), opts, out);
      const auto found = find_in_lab<core::Freezer>(fixture_->backend(), fixture_->lab_a(), id);
      ASSERT_TRUE(found.has_value());
      EXPECT_EQ(found->name, "Cryo-1");
    }

    TEST_P(CliBackendTest, BoxTypeCreateGeneratesUniformGrid) {
      std::ostringstream out;
      const BoxTypeCreateOptions opts{.common = create_common(fixture_->lab_a()),
                                      .name = "2x3 rack",
                                      .rows = 2,
                                      .cols = 3,
                                      .accepts_size_class = "cryovial_2ml"};
      const auto id = run_box_type_create(fixture_->backend(), opts, out);
      const auto found = find_in_lab<core::BoxType>(fixture_->backend(), fixture_->lab_a(), id);
      ASSERT_TRUE(found.has_value());
      EXPECT_EQ(found->positions.size(), 6U);
      EXPECT_EQ(found->positions.front().label, "A1");
      EXPECT_EQ(found->positions.back().label, "B3");
    }

    TEST_P(CliBackendTest, BoxTypeCreateRejectsZeroRows) {
      std::ostringstream out;
      const BoxTypeCreateOptions opts{.common = create_common(fixture_->lab_a()),
                                      .name = "bad",
                                      .rows = 0,
                                      .cols = 3,
                                      .accepts_size_class = "cryovial_2ml"};
      EXPECT_THROW(run_box_type_create(fixture_->backend(), opts, out), std::invalid_argument);
    }

    TEST_P(CliBackendTest, BoxCreatePersistsWithSeededRefs) {
      std::ostringstream out;
      // BoxType 33 and storage container 30 are both seeded in lab A.
      const BoxCreateOptions opts{.common = create_common(fixture_->lab_a()),
                                  .label = "Box-new",
                                  .box_type_id = id_from_low<core::BoxTypeId>(33),
                                  .storage_container_id =
                                      id_from_low<core::StorageContainerId>(30)};
      const auto id = run_box_create(fixture_->backend(), opts, out);
      const auto found = find_in_lab<core::Box>(fixture_->backend(), fixture_->lab_a(), id);
      ASSERT_TRUE(found.has_value());
      EXPECT_EQ(found->label, "Box-new");
    }

    // ---- lab create (first-run bootstrap) ----

    TEST_P(CliBackendTest, LabCreatePersistsLabUserAndSystemAdminMembership) {
      std::ostringstream out;
      const LabCreateOptions opts{.name = "Genomics Core",
                                  .contact = "core@example.edu",
                                  .admin_email = "boss@example.edu",
                                  .admin_display_name = "Dr. Boss"};
      const auto result = run_lab_create(fixture_->backend(), opts, out);
      EXPECT_NE(out.str().find("created lab " + result.lab_id.to_string()), std::string::npos);
      EXPECT_NE(out.str().find("created system admin " + result.admin_user_id.to_string()),
                std::string::npos);

      auto txn = fixture_->backend().begin(storage::IsolationLevel::Serializable);
      const auto lab = txn->repo<core::Lab>().find_by_id(result.lab_id);
      ASSERT_TRUE(lab.has_value());
      EXPECT_EQ(lab->name, "Genomics Core");

      const auto user = txn->repo<core::User>().find_by_id(result.admin_user_id);
      ASSERT_TRUE(user.has_value());
      EXPECT_EQ(user->primary_email, "boss@example.edu");
      EXPECT_EQ(user->default_lab_id, result.lab_id);

      const auto membership = txn->repo<core::LabMembership>().find_by_id(
          core::LabMembershipId{.user_id = result.admin_user_id, .lab_id = result.lab_id});
      ASSERT_TRUE(membership.has_value());
      EXPECT_EQ(membership->role_id, core::builtin_role_id(core::RoleKind::SystemAdmin));
      txn->commit();
    }

    TEST_P(CliBackendTest, LabCreateRejectsEmptyName) {
      std::ostringstream out;
      const LabCreateOptions opts{.name = "", .admin_email = "boss@example.edu"};
      EXPECT_THROW(run_lab_create(fixture_->backend(), opts, out), std::invalid_argument);
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

    TEST(CliAppTest, AuditExportToStdout) {
      CliFixture fixture(BackendKind::Sqlite);
      std::ostringstream out;
      std::ostringstream err;
      const std::string sqlite_db = fixture.db_path();
      const std::string actor = id_from_low<core::UserId>(10).to_string();
      const std::vector<const char*> args = {"freezerctl",      "audit",   "export",     "--sqlite",
                                             sqlite_db.c_str(), "--actor", actor.c_str()};
      const int code = run_cli(static_cast<int>(args.size()), args.data(), out, err);
      EXPECT_EQ(code, 0) << err.str();
      EXPECT_NE(out.str().find("# freezermanager-audit-export"), std::string::npos);
    }

    TEST(CliAppTest, AuditExportToFile) {
      CliFixture fixture(BackendKind::Sqlite);
      const auto csv_path =
          std::filesystem::temp_directory_path() / "freezermanager-audit-export-test.csv";
      std::filesystem::remove(csv_path);
      std::ostringstream out;
      std::ostringstream err;
      const std::string sqlite_db = fixture.db_path();
      const std::string actor = id_from_low<core::UserId>(10).to_string();
      const std::string file = csv_path.string();
      const std::vector<const char*> args = {"freezerctl",  "audit",           "export",
                                             "--sqlite",    sqlite_db.c_str(), "--actor",
                                             actor.c_str(), "--out",           file.c_str()};
      const int code = run_cli(static_cast<int>(args.size()), args.data(), out, err);
      EXPECT_EQ(code, 0) << err.str();
      ASSERT_TRUE(std::filesystem::exists(csv_path));
      std::ifstream in(csv_path, std::ios::binary);
      const std::string text((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());
      std::filesystem::remove(csv_path);
      EXPECT_NE(text.find("# freezermanager-audit-export"), std::string::npos);
    }

    TEST(CliAppTest, AuditExportLabScopedViaArgv) {
      CliFixture fixture(BackendKind::Sqlite);
      std::ostringstream out;
      std::ostringstream err;
      const std::string sqlite_db = fixture.db_path();
      const std::string lab = fixture.lab_a().to_string();
      const std::string actor = id_from_low<core::UserId>(10).to_string();
      const std::vector<const char*> args = {"freezerctl", "audit",           "export",
                                             "--sqlite",   sqlite_db.c_str(), "--lab",
                                             lab.c_str(),  "--actor",         actor.c_str()};
      const int code = run_cli(static_cast<int>(args.size()), args.data(), out, err);
      EXPECT_EQ(code, 0) << err.str();
      const std::string text = out.str();
      EXPECT_NE(text.find("lab_filter=" + lab), std::string::npos);
      // A lab-scoped export must not leak lab B's events.
      EXPECT_EQ(text.find(fixture.lab_b().to_string()), std::string::npos);
    }

    TEST(CliAppTest, AuditVerifyViaArgv) {
      CliFixture fixture(BackendKind::Sqlite);
      std::ostringstream out;
      std::ostringstream err;
      const std::string sqlite_db = fixture.db_path();
      const std::vector<const char*> args = {"freezerctl", "audit", "verify", "--sqlite",
                                             sqlite_db.c_str()};
      const int code = run_cli(static_cast<int>(args.size()), args.data(), out, err);
      EXPECT_EQ(code, 0) << err.str();
      EXPECT_NE(out.str().find("OK:"), std::string::npos);
    }

    TEST(CliAppTest, KeyRotateViaEnv) {
      CliFixture fixture(BackendKind::Sqlite);
      const fmgr::kms::EnvVarKms old_kms(old_kek());
      insert_phi_sample(fixture.backend(), fixture.lab_a(), old_kms, 556);

      ::setenv("FMGR_MASTER_KEK", kNewKekB64, 1);          // NOLINT(concurrency-mt-unsafe)
      ::setenv("FMGR_MASTER_KEK_PREVIOUS", kOldKekB64, 1); // NOLINT(concurrency-mt-unsafe)

      std::ostringstream out;
      std::ostringstream err;
      const std::string sqlite_db = fixture.db_path();
      const std::string actor = id_from_low<core::UserId>(10).to_string();
      const std::vector<const char*> args = {"freezerctl",      "key",     "rotate",     "--sqlite",
                                             sqlite_db.c_str(), "--actor", actor.c_str()};
      const int code = run_cli(static_cast<int>(args.size()), args.data(), out, err);

      ::unsetenv("FMGR_MASTER_KEK");          // NOLINT(concurrency-mt-unsafe)
      ::unsetenv("FMGR_MASTER_KEK_PREVIOUS"); // NOLINT(concurrency-mt-unsafe)

      EXPECT_EQ(code, 0) << err.str();
      EXPECT_NE(out.str().find("rotated 1 of 1"), std::string::npos) << out.str();
    }

    // ---- backup (encrypted SQLite hot-copy + restore-drill) ----

    constexpr const char* kBackupKekB64 = "ICEiIyQlJicoKSorLC0uLzAxMjM0NTY3ODk6Ozw9Pj8=";

    std::filesystem::path backup_temp(const std::string& suffix) {
      static std::atomic<std::uint64_t> counter{0};
      return std::filesystem::temp_directory_path() /
             ("freezermanager-backup-" + std::to_string(counter.fetch_add(1)) + suffix);
    }

    TEST(BackupTest, CreateVerifyRestoreRoundTrip) {
      CliFixture fixture(BackendKind::Sqlite);
      const auto backup_kms = fmgr::kms::EnvVarKms::from_base64(kBackupKekB64);
      const auto out = backup_temp(".fmgrbak");
      const auto actor = id_from_low<core::UserId>(10);
      std::ostringstream sink;

      const auto report = run_backup_create(fixture.backend(), fixture.db_path(), backup_kms,
                                            out.string(), actor, sink);
      EXPECT_TRUE(std::filesystem::exists(out));
      EXPECT_FALSE(report.content_sha256.empty());

      std::ostringstream vsink;
      EXPECT_TRUE(run_backup_verify(out.string(), backup_kms, vsink).ok) << vsink.str();

      const auto restored = backup_temp(".restored.db");
      std::ostringstream rsink;
      run_backup_restore(out.string(), backup_kms, restored.string(), false, actor, rsink);

      // The restored database has lab A and a chain that still verifies.
      auto backend = open_backend(BackendOptions{.sqlite_path = restored.string()});
      auto txn = backend->begin(storage::IsolationLevel::Serializable);
      const auto labs = txn->repo<core::Lab>().query(storage::Query<core::Lab>::all());
      txn->commit();
      EXPECT_TRUE(std::any_of(labs.begin(), labs.end(),
                              [&](const core::Lab& lab) { return lab.id == fixture.lab_a(); }));
      std::ostringstream avsink;
      EXPECT_EQ(run_audit_verify(*backend, AuditVerifyOptions{}, avsink), 0) << avsink.str();
      backend.reset();

      std::filesystem::remove(out);
      remove_sqlite_files(restored);
    }

    TEST(BackupTest, CreateAppendsAuditEvent) {
      CliFixture fixture(BackendKind::Sqlite);
      const auto backup_kms = fmgr::kms::EnvVarKms::from_base64(kBackupKekB64);
      const auto out = backup_temp(".fmgrbak");
      std::ostringstream sink;
      run_backup_create(fixture.backend(), fixture.db_path(), backup_kms, out.string(),
                        id_from_low<core::UserId>(10), sink);

      auto txn = fixture.backend().begin(storage::IsolationLevel::Serializable);
      const auto events =
          txn->repo<core::AuditEvent>().query(storage::Query<core::AuditEvent>::all());
      txn->commit();
      EXPECT_TRUE(std::any_of(events.begin(), events.end(), [](const core::AuditEvent& event) {
        return event.action == "backup.create";
      }));
      std::filesystem::remove(out);
    }

    TEST(BackupTest, TamperedBackupFailsVerify) {
      CliFixture fixture(BackendKind::Sqlite);
      const auto backup_kms = fmgr::kms::EnvVarKms::from_base64(kBackupKekB64);
      const auto out = backup_temp(".fmgrbak");
      std::ostringstream sink;
      run_backup_create(fixture.backend(), fixture.db_path(), backup_kms, out.string(),
                        id_from_low<core::UserId>(10), sink);

      // Flip a byte in the ciphertext region (well past the manifest line).
      std::vector<char> bytes;
      {
        std::ifstream in(out, std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
      }
      ASSERT_GT(bytes.size(), 100U);
      bytes[bytes.size() - 5] ^= 0x01;
      {
        std::ofstream rewrite(out, std::ios::binary | std::ios::trunc);
        rewrite.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
      }

      std::ostringstream vsink;
      EXPECT_FALSE(run_backup_verify(out.string(), backup_kms, vsink).ok);
      EXPECT_NE(vsink.str().find("FAIL"), std::string::npos);
      std::filesystem::remove(out);
    }

    TEST(BackupTest, RestoreRefusesOverwriteWithoutForce) {
      CliFixture fixture(BackendKind::Sqlite);
      const auto backup_kms = fmgr::kms::EnvVarKms::from_base64(kBackupKekB64);
      const auto out = backup_temp(".fmgrbak");
      const auto actor = id_from_low<core::UserId>(10);
      std::ostringstream sink;
      run_backup_create(fixture.backend(), fixture.db_path(), backup_kms, out.string(), actor,
                        sink);

      const auto dest = backup_temp(".restored.db");
      { std::ofstream(dest) << "existing"; } // occupy the destination

      std::ostringstream rsink;
      EXPECT_THROW(run_backup_restore(out.string(), backup_kms, dest.string(), false, actor, rsink),
                   BackupError);
      EXPECT_NO_THROW(
          run_backup_restore(out.string(), backup_kms, dest.string(), true, actor, rsink));

      std::filesystem::remove(out);
      remove_sqlite_files(dest);
    }

    // ---- PostgreSQL backup (pg_dump/pg_restore) ----

    // pg_restore --list over a non-archive must report failure, never throw, so the
    // schedule-safe drill contract holds for Postgres archives too.
    TEST(PostgresDumpTest, ListRejectsGarbageFile) {
      const auto garbage = backup_temp(".notadump");
      { std::ofstream(garbage) << "this is not a pg_dump archive\n"; }
      std::string detail;
      EXPECT_FALSE(fmgr::backup::pg_restore_list_ok(garbage.string(), detail));
      EXPECT_FALSE(detail.empty());
      std::filesystem::remove(garbage);
    }

    // A malformed connection string is rejected by libpq parsing before any tool runs.
    TEST(PostgresDumpTest, InvalidConninfoThrows) {
      EXPECT_THROW(fmgr::backup::pg_dump_to_file("definitely_not_a_keyword=1", "/tmp/never.dump"),
                   BackupError);
    }

    // Full create -> verify -> restore round-trip against a live Postgres. Restores
    // back into the same database (pg_restore --clean), then asserts the seeded lab
    // survives and the audit chain still verifies. Skipped without FMGR_TEST_POSTGRES_URL.
    TEST(PostgresBackupTest, CreateVerifyRestoreRoundTrip) {
      if (!postgres_test_url().has_value()) {
        GTEST_SKIP() << "FMGR_TEST_POSTGRES_URL not set; skipping Postgres backup test";
      }
      CliFixture fixture(BackendKind::Postgres);
      const std::string url = postgres_test_url().value();
      const auto backup_kms = fmgr::kms::EnvVarKms::from_base64(kBackupKekB64);
      const auto out = backup_temp(".pg.fmgrbak");
      const auto actor = id_from_low<core::UserId>(10);
      std::ostringstream sink;

      const auto report =
          run_backup_create_postgres(fixture.backend(), url, backup_kms, out.string(), actor, sink);
      EXPECT_TRUE(std::filesystem::exists(out));
      EXPECT_FALSE(report.content_sha256.empty());

      std::ostringstream vsink;
      EXPECT_TRUE(run_backup_verify(out.string(), backup_kms, vsink).ok) << vsink.str();
      EXPECT_NE(vsink.str().find("postgres"), std::string::npos) << vsink.str();

      std::ostringstream rsink;
      run_backup_restore_postgres(out.string(), backup_kms, url, actor, rsink);

      // A fresh backend on the restored database sees the seeded lab and a chain
      // that still verifies. (fixture.backend()'s pooled connections may hold state
      // invalidated by pg_restore --clean, so do not reuse it here.) The fixture's
      // data lives in its private schema, so pin the reopen's search_path to it.
      auto backend = open_backend(
          BackendOptions{.postgres_url = postgres_url_with_schema(url, fixture.postgres_schema())});
      auto txn = backend->begin(storage::IsolationLevel::Serializable);
      const auto labs = txn->repo<core::Lab>().query(storage::Query<core::Lab>::all());
      txn->commit();
      EXPECT_TRUE(std::any_of(labs.begin(), labs.end(),
                              [&](const core::Lab& lab) { return lab.id == fixture.lab_a(); }));
      std::ostringstream avsink;
      EXPECT_EQ(run_audit_verify(*backend, AuditVerifyOptions{}, avsink), 0) << avsink.str();

      std::filesystem::remove(out);
    }

    // backup.create is appended to the live Postgres audit chain — exercising
    // ITransaction::note_mutation on the Postgres transaction (no SQLite dynamic_cast).
    TEST(PostgresBackupTest, CreateAppendsAuditEvent) {
      if (!postgres_test_url().has_value()) {
        GTEST_SKIP() << "FMGR_TEST_POSTGRES_URL not set; skipping Postgres backup test";
      }
      CliFixture fixture(BackendKind::Postgres);
      const std::string url = postgres_test_url().value();
      const auto backup_kms = fmgr::kms::EnvVarKms::from_base64(kBackupKekB64);
      const auto out = backup_temp(".pg.fmgrbak");
      std::ostringstream sink;
      run_backup_create_postgres(fixture.backend(), url, backup_kms, out.string(),
                                 id_from_low<core::UserId>(10), sink);

      auto txn = fixture.backend().begin(storage::IsolationLevel::Serializable);
      const auto events =
          txn->repo<core::AuditEvent>().query(storage::Query<core::AuditEvent>::all());
      txn->commit();
      EXPECT_TRUE(std::any_of(events.begin(), events.end(), [](const core::AuditEvent& event) {
        return event.action == "backup.create";
      }));
      std::filesystem::remove(out);
    }

    TEST(CliAppTest, BackupCreateAndVerifyViaArgv) {
      CliFixture fixture(BackendKind::Sqlite);
      ::setenv("FMGR_BACKUP_KEK", kBackupKekB64, 1); // NOLINT(concurrency-mt-unsafe)
      ::unsetenv("FMGR_BACKUP_KEK_PREVIOUS");        // NOLINT(concurrency-mt-unsafe)

      const std::string sqlite_db = fixture.db_path();
      const auto out_path = backup_temp(".fmgrbak");
      const std::string out = out_path.string();
      const std::string actor = id_from_low<core::UserId>(10).to_string();

      std::ostringstream o1;
      std::ostringstream e1;
      const std::vector<const char*> create_args = {"freezerctl", "backup",          "create",
                                                    "--sqlite",   sqlite_db.c_str(), "--out",
                                                    out.c_str(),  "--actor",         actor.c_str()};
      const int create_code =
          run_cli(static_cast<int>(create_args.size()), create_args.data(), o1, e1);
      EXPECT_EQ(create_code, 0) << e1.str();

      std::ostringstream o2;
      std::ostringstream e2;
      const std::vector<const char*> verify_args = {"freezerctl", "backup", "verify", "--in",
                                                    out.c_str()};
      const int verify_code =
          run_cli(static_cast<int>(verify_args.size()), verify_args.data(), o2, e2);
      EXPECT_EQ(verify_code, 0) << e2.str();
      EXPECT_NE(o2.str().find("PASS"), std::string::npos);

      ::unsetenv("FMGR_BACKUP_KEK"); // NOLINT(concurrency-mt-unsafe)
      std::filesystem::remove(out_path);
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

    TEST(CliAppTest, ItemTypeImportFromFile) {
      CliFixture fixture(BackendKind::Sqlite);
      const auto csv_path =
          std::filesystem::temp_directory_path() / "freezermanager-itemtype-import-test.csv";
      {
        std::ofstream csv(csv_path, std::ios::binary | std::ios::trunc);
        csv << "name\r\nblood\r\ncsf\r\n";
      }
      std::ostringstream out;
      std::ostringstream err;
      const std::string sqlite_db = fixture.db_path();
      const std::string lab = fixture.lab_a().to_string();
      const std::string actor = id_from_low<core::UserId>(10).to_string();
      const std::string file = csv_path.string();
      const std::vector<const char*> args = {
          "freezerctl", "item-type", "import",  "--sqlite",    sqlite_db.c_str(),
          "--lab",      lab.c_str(), "--actor", actor.c_str(), file.c_str()};
      const int code = run_cli(static_cast<int>(args.size()), args.data(), out, err);
      std::filesystem::remove(csv_path);
      EXPECT_EQ(code, 0) << err.str() << out.str();
      EXPECT_NE(out.str().find("imported 2 item-type(s)"), std::string::npos);
    }

    TEST(CliAppTest, UserImportFromFile) {
      CliFixture fixture(BackendKind::Sqlite);
      const auto csv_path =
          std::filesystem::temp_directory_path() / "freezermanager-user-import-test.csv";
      {
        std::ofstream csv(csv_path, std::ios::binary | std::ios::trunc);
        csv << "primary_email,display_name\r\nbulk1@example.edu,Bulk One\r\n";
      }
      std::ostringstream out;
      std::ostringstream err;
      const std::string sqlite_db = fixture.db_path();
      const std::string lab = fixture.lab_a().to_string();
      const std::string actor = id_from_low<core::UserId>(10).to_string();
      const std::string file = csv_path.string();
      const std::vector<const char*> args = {
          "freezerctl", "user",      "import",  "--sqlite",    sqlite_db.c_str(),
          "--lab",      lab.c_str(), "--actor", actor.c_str(), file.c_str()};
      const int code = run_cli(static_cast<int>(args.size()), args.data(), out, err);
      std::filesystem::remove(csv_path);
      EXPECT_EQ(code, 0) << err.str() << out.str();
      EXPECT_NE(out.str().find("imported 1 user(s)"), std::string::npos);
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

    TEST(CliAppTest, LabCreateFromArgv) {
      CliFixture fixture(BackendKind::Sqlite);
      std::ostringstream out;
      std::ostringstream err;
      const std::string sqlite_db = fixture.db_path();
      const std::vector<const char*> args = {
          "freezerctl", "lab",      "create",        "--sqlite",         sqlite_db.c_str(),
          "--name",     "Argv Lab", "--admin-email", "admin@example.edu"};
      const int code = run_cli(static_cast<int>(args.size()), args.data(), out, err);
      EXPECT_EQ(code, 0) << err.str() << out.str();
      EXPECT_NE(out.str().find("created lab "), std::string::npos);
      EXPECT_NE(out.str().find("created system admin "), std::string::npos);
    }

    TEST(CliAppTest, ItemTypeCreateFromArgv) {
      // `create` attaches to the same root as the read-noun list/inspect.
      CliFixture fixture(BackendKind::Sqlite);
      std::ostringstream out;
      std::ostringstream err;
      const std::string sqlite_db = fixture.db_path();
      const std::string lab = fixture.lab_a().to_string();
      const std::string actor = id_from_low<core::UserId>(10).to_string();
      const std::vector<const char*> args = {
          "freezerctl", "item-type", "create",      "--sqlite", sqlite_db.c_str(), "--lab",
          lab.c_str(),  "--actor",   actor.c_str(), "--name",   "plasmid"};
      const int code = run_cli(static_cast<int>(args.size()), args.data(), out, err);
      EXPECT_EQ(code, 0) << err.str() << out.str();
      EXPECT_NE(out.str().find("created item-type "), std::string::npos);
    }

    TEST(CliAppTest, BoxTypeCreateFromArgv) {
      // box-type lives on its own root (no read noun yet); --rows/--cols build a grid.
      CliFixture fixture(BackendKind::Sqlite);
      std::ostringstream out;
      std::ostringstream err;
      const std::string sqlite_db = fixture.db_path();
      const std::string lab = fixture.lab_a().to_string();
      const std::string actor = id_from_low<core::UserId>(10).to_string();
      const std::vector<const char*> args = {
          "freezerctl", "box-type", "create",      "--sqlite",  sqlite_db.c_str(), "--lab",
          lab.c_str(),  "--actor",  actor.c_str(), "--name",    "9x9 cryobox",     "--rows",
          "9",          "--cols",   "9",           "--accepts", "cryovial_2ml"};
      const int code = run_cli(static_cast<int>(args.size()), args.data(), out, err);
      EXPECT_EQ(code, 0) << err.str() << out.str();
      EXPECT_NE(out.str().find("created box-type "), std::string::npos);
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

    // ---- Break-it: CSV parsing edge cases ----

    TEST(CsvReaderTest, EmptyInputProducesNoRows) {
      const auto rows = parse("");
      EXPECT_TRUE(rows.empty());
    }

    TEST(CsvReaderTest, WhitespaceOnlyLines) {
      const auto rows = parse("   \r\n\t\r\n");
      // Whitespace-only lines may be treated as empty rows or skipped.
      // Must not crash; behavior for empty rows is implementation-defined.
    }

    TEST(CsvReaderTest, VeryLongFieldDoesNotCrash) {
      std::string long_field(100'000, 'x');
      const auto rows = parse("\"" + long_field + "\"\r\n");
      ASSERT_EQ(rows.size(), 1U);
      ASSERT_EQ(rows.at(0).size(), 1U);
      EXPECT_EQ(rows.at(0).at(0), long_field);
    }

    TEST(CsvReaderTest, VeryWideRowDoesNotCrash) {
      std::string header;
      for (int i = 0; i < 500; ++i) {
        if (i > 0) {
          header += ",";
        }
        header += "col_" + std::to_string(i);
      }
      const auto rows = parse(header + "\r\n");
      ASSERT_EQ(rows.size(), 1U);
      EXPECT_EQ(rows.at(0).size(), 500U);
    }

    TEST(CsvReaderTest, DeeplyNestedQuotes) {
      // "say \"\"\"hi\"\"\"" → say """hi"""
      const auto rows = parse("\"say \"\"\"\"\"hi\"\"\"\"\"\"\r\n");
      ASSERT_EQ(rows.size(), 1U);
    }

    TEST(CsvReaderTest, SingleCharacterInputs) {
      EXPECT_EQ(parse("x").size(), 1U);
      EXPECT_EQ(parse(",").size(), 1U);
      // A lone double-quote is an unterminated quoted field — must throw.
      EXPECT_THROW(parse("\""), CsvParseError);
    }

    TEST(CsvWriterTest, EmptyFieldList) {
      std::ostringstream out;
      write_csv_row(out, {});
      EXPECT_EQ(out.str(), "\r\n");
    }

    // ---- Break-it: sample import empty/boundary ----

    TEST(SampleImportTest, SampleWithEmptyCustomFieldsParsesAsEmptyObject) {
      const std::vector<std::vector<std::string>> records = {
          {"name", "item_type_id", "custom_fields_json"}, {"S1", item_type_uuid(), "{}"}};
      const auto report = build_import(records, import_ctx());
      ASSERT_FALSE(report.has_errors());
      ASSERT_FALSE(report.rows.empty());
      EXPECT_TRUE(report.rows.at(0).ok);
    }

    TEST(SampleImportTest, SampleWithNestedCustomFieldsJson) {
      const std::vector<std::vector<std::string>> records = {
          {"name", "item_type_id", "custom_fields_json"},
          {"S1", item_type_uuid(), R"({"nested":{"key":"value"}})"}};
      const auto report = build_import(records, import_ctx());
      ASSERT_FALSE(report.has_errors());
      EXPECT_TRUE(report.rows.at(0).ok);
    }

    // ---- Break-it: BackendFactory edge cases ----

    TEST(BackendFactoryTest, EmptySqlitePathOption) {
      // An empty SQLite path — the factory treats it as an implicit in-memory
      // database or rejects it. Either outcome must not crash.
      try {
        (void)open_backend(BackendOptions{.sqlite_path = ""});
        // If accepted, it opened successfully (likely as :memory:).
      } catch (const BackendOptionError&) {
        SUCCEED() << "rejecting an empty SQLite path is acceptable";
      }
    }

    // ==== Aggressive: BOM, formula injection, mixed newlines ====

    TEST(CsvReaderTest, BOMAtStartOfInput) {
      // UTF-8 BOM (0xEF 0xBB 0xBF) at the very beginning.
      const std::string bom = "\xEF\xBB\xBF";
      const auto rows = parse(bom + "name\r\nvalue\r\n");
      ASSERT_GE(rows.size(), 1U);
      // The BOM may be silently stripped or treated as part of the first field name.
    }

    TEST(CsvReaderTest, CsvFormulaInjectionIsNotExecuted) {
      // Fields starting with =, +, -, @ must not be interpreted as formulas.
      const auto rows = parse("=SUM(A1:A10),+123,@HYPERLINK,-45\r\n");
      ASSERT_EQ(rows.size(), 1U);
    }

    TEST(CsvReaderTest, MixedLineEndings) {
      // A file mixing CR, LF, and CRLF within the same document.
      const auto rows = parse("a\rb\nc\r\nd");
      ASSERT_GE(rows.size(), 1U);
    }

    TEST(CsvReaderTest, EmptyQuotedField) {
      const auto rows = parse("\"\"\r\n");
      ASSERT_EQ(rows.size(), 1U);
      ASSERT_EQ(rows.at(0).size(), 1U);
      EXPECT_EQ(rows.at(0).at(0), "");
    }

    TEST(CsvReaderTest, FieldWithOnlyQuotes) {
      // """" → a single literal double-quote character.
      const auto rows = parse("\"\"\"\"\r\n");
      ASSERT_EQ(rows.size(), 1U);
      ASSERT_EQ(rows.at(0).size(), 1U);
      EXPECT_EQ(rows.at(0).at(0), "\"");
    }

    TEST(CsvReaderTest, TrailingSpacesAfterQuotedField) {
      // Spaces after closing quote but before comma — typically invalid per RFC 4180.
      const auto rows = parse("\"hello\" ,world\r\n");
      ASSERT_GE(rows.size(), 1U);
    }

    TEST(CsvReaderTest, ControlCharactersExceptCrLf) {
      // ASCII control characters (0x01-0x08, 0x0B-0x0C, 0x0E-0x1F) in unquoted fields.
      const auto rows = parse("a\x01\x02b,c\r\n");
      ASSERT_GE(rows.size(), 1U);
    }

  } // namespace
} // namespace fmgr::cli
