// SPDX-License-Identifier: AGPL-3.0-or-later

#include "storage/sqlite/SqliteBackend.h"

#include "core/enums.h"
#include "core/ids.h"
#include "core/item_type.h"
#include "core/timestamp.h"
#include "storage/CustomFieldResolver.h"
#include "storage/IdentityTraits.h"
#include "storage/ItemTypeTraits.h"
#include "storage/detail/QuerySqlBuilder.h"
#include "storage/sqlite/IdentityRepositories.h"
#include "storage/sqlite/ItemTypeRepositories.h"

#include "test_helpers.h"
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace fmgr::storage {
  namespace {
    using namespace fmgr::test;

    struct SqliteConformanceSample {
      using Id = core::SampleId;

      enum class Field : std::uint8_t {
        Id,
        LabId,
        Name,
        Status,
        BoxId,
        PositionLabel,
        CustomFields,
        CreatedAt
      };

      Id id;
      core::LabId lab_id;
      std::string name;
      core::SampleStatus status{core::SampleStatus::Active};
      std::optional<core::BoxId> box_id;
      std::optional<std::string> position_label;
      nlohmann::json custom_fields = nlohmann::json::object();
      core::Timestamp created_at;

      friend bool operator==(const SqliteConformanceSample&,
                             const SqliteConformanceSample&) = default;
    };

    struct UnsupportedSqliteConformanceEntity {
      using Id = core::BoxId;

      enum class Field : std::uint8_t { Id };
    };

  } // namespace

  template <> struct EntityTraits<SqliteConformanceSample> {
    using Id = SqliteConformanceSample::Id;
    using Field = SqliteConformanceSample::Field;

    [[nodiscard]] static constexpr std::string_view entity_name() {
      return "sqlite_conformance_sample";
    }

    [[nodiscard]] static constexpr Field tombstone_field() {
      return Field::Status;
    }
  };

  template <> struct EntityTraits<UnsupportedSqliteConformanceEntity> {
    using Id = UnsupportedSqliteConformanceEntity::Id;
    using Field = UnsupportedSqliteConformanceEntity::Field;

    [[nodiscard]] static constexpr std::string_view entity_name() {
      return "unsupported_sqlite_conformance_entity";
    }

    [[nodiscard]] static constexpr Field tombstone_field() {
      return Field::Id;
    }
  };

  namespace {

    [[nodiscard]] SqliteConformanceSample sample(std::uint64_t id_low_bits, std::string name,
                                                 core::Timestamp created_at) {
      return SqliteConformanceSample{
          .id = id_from_low<core::SampleId>(id_low_bits),
          .lab_id = id_from_low<core::LabId>(1),
          .name = std::move(name),
          .status = core::SampleStatus::Active,
          .box_id = id_from_low<core::BoxId>(100),
          .position_label = std::string("A") + std::to_string(id_low_bits),
          .custom_fields = nlohmann::json{{"project", "alpha"}},
          .created_at = created_at,
      };
    }

    [[nodiscard]] MutationContext mutation_context() {
      return MutationContext{
          .actor_user_id = id_from_low<core::UserId>(500),
          .actor_session_id = "sqlite-conformance-session",
          .request_id = "sqlite-conformance-request",
          .reason = "sqlite backend conformance test",
      };
    }

    // A sample-scoped definition with a caller-chosen label, so a test can tell
    // the two candidates of one key apart by reading back what survived.
    [[nodiscard]] core::CustomFieldDefinition
    make_conformance_cfd(std::uint64_t id_low_bits, core::LabId lab_id,
                         std::optional<core::ItemTypeId> item_type_id, std::string key,
                         std::string label, bool required) {
      return core::CustomFieldDefinition{
          .id = id_from_low<core::CustomFieldDefinitionId>(id_low_bits),
          .lab_id = lab_id,
          .scope_kind = core::ScopeKind::Sample,
          .item_type_id = item_type_id,
          .key = std::move(key),
          .label = std::move(label),
          .data_type = core::FieldDataType::String,
          .required = required,
          .validation_json = "{}",
          .indexed = false,
          .is_phi = false,
          .created_at =
              core::Timestamp::from_unix_micros(300 + static_cast<std::int64_t>(id_low_bits)),
      };
    }

    void bind_text(sqlite3_stmt* statement, int index, const std::string& value) {
      const auto result = sqlite3_bind_text(statement, index, value.c_str(),
                                            static_cast<int>(value.size()), SQLITE_TRANSIENT);
      if (result != SQLITE_OK) {
        throw ConstraintViolation("failed to bind sqlite text parameter");
      }
    }

    void bind_int64(sqlite3_stmt* statement, int index, std::int64_t value) {
      const auto result = sqlite3_bind_int64(statement, index, value);
      if (result != SQLITE_OK) {
        throw ConstraintViolation("failed to bind sqlite integer parameter");
      }
    }

    void bind_null(sqlite3_stmt* statement, int index) {
      const auto result = sqlite3_bind_null(statement, index);
      if (result != SQLITE_OK) {
        throw ConstraintViolation("failed to bind sqlite null parameter");
      }
    }

    [[nodiscard]] std::string sqlite_error(sqlite3* handle, std::string_view action) {
      return std::string(action) + ": " + sqlite3_errmsg(handle);
    }

    [[noreturn]] void throw_sqlite_error(int code, sqlite3* handle, std::string_view action) {
      const auto extended_code = sqlite3_extended_errcode(handle);
      const auto effective_code = extended_code == SQLITE_OK ? code : extended_code;
      switch (effective_code) {
      case SQLITE_CONSTRAINT_UNIQUE:
      case SQLITE_CONSTRAINT_PRIMARYKEY:
        throw UniqueViolation(sqlite_error(handle, action));
      case SQLITE_BUSY:
      case SQLITE_LOCKED:
        throw Unavailable(sqlite_error(handle, action));
      default:
        throw ConstraintViolation(sqlite_error(handle, action));
      }
    }

    class Statement {
    public:
      Statement(sqlite3* handle, const std::string& sql) : handle_(handle) {
        const auto result = sqlite3_prepare_v2(handle_, sql.c_str(), -1, &statement_, nullptr);
        if (result != SQLITE_OK) {
          throw_sqlite_error(result, handle_, "prepare statement");
        }
      }

      ~Statement() {
        sqlite3_finalize(statement_);
      }

      Statement(const Statement&) = delete;
      Statement& operator=(const Statement&) = delete;

      [[nodiscard]] sqlite3_stmt* get() const {
        return statement_;
      }

      [[nodiscard]] bool step_row() const {
        const auto result = sqlite3_step(statement_);
        if (result == SQLITE_ROW) {
          return true;
        }
        if (result == SQLITE_DONE) {
          return false;
        }
        throw_sqlite_error(result, handle_, "step statement");
      }

      void step_done() const {
        const auto result = sqlite3_step(statement_);
        if (result != SQLITE_DONE) {
          throw_sqlite_error(result, handle_, "execute statement");
        }
      }

    private:
      sqlite3* handle_;
      sqlite3_stmt* statement_{nullptr};
    };

    [[nodiscard]] std::string column_text(sqlite3_stmt* statement, int column) {
      const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(statement, column));
      return text == nullptr ? std::string() : std::string(text);
    }

    [[nodiscard]] bool is_active_occupant(core::SampleStatus status) {
      return status == core::SampleStatus::Active || status == core::SampleStatus::CheckedOut;
    }

    [[nodiscard]] std::string column_name(SqliteConformanceSample::Field field) {
      switch (field) {
      case SqliteConformanceSample::Field::Id:
        return "id";
      case SqliteConformanceSample::Field::LabId:
        return "lab_id";
      case SqliteConformanceSample::Field::Name:
        return "name";
      case SqliteConformanceSample::Field::Status:
        return "status";
      case SqliteConformanceSample::Field::BoxId:
        return "box_id";
      case SqliteConformanceSample::Field::PositionLabel:
        return "position_label";
      case SqliteConformanceSample::Field::CustomFields:
        return "custom_fields_json";
      case SqliteConformanceSample::Field::CreatedAt:
        return "created_at_micros";
      }
      throw ConstraintViolation("unknown sqlite conformance field");
    }

    struct StoredSample {
      SqliteConformanceSample entity;
      std::uint64_t version{0};
    };

    [[nodiscard]] StoredSample read_sample(sqlite3_stmt* statement) {
      auto box_id = std::optional<core::BoxId>{};
      if (sqlite3_column_type(statement, 4) != SQLITE_NULL) {
        box_id = core::BoxId::parse(column_text(statement, 4));
      }

      auto position_label = std::optional<std::string>{};
      if (sqlite3_column_type(statement, 5) != SQLITE_NULL) {
        position_label = column_text(statement, 5);
      }

      return StoredSample{
          .entity =
              SqliteConformanceSample{
                  .id = core::SampleId::parse(column_text(statement, 0)),
                  .lab_id = core::LabId::parse(column_text(statement, 1)),
                  .name = column_text(statement, 2),
                  .status = core::parse_sample_status(column_text(statement, 3)),
                  .box_id = box_id,
                  .position_label = position_label,
                  .custom_fields = nlohmann::json::parse(column_text(statement, 6)),
                  .created_at =
                      core::Timestamp::from_unix_micros(sqlite3_column_int64(statement, 7)),
              },
          .version = static_cast<std::uint64_t>(sqlite3_column_int64(statement, 8)),
      };
    }

    class SqliteConformanceSampleRepository final : public IRepository<SqliteConformanceSample> {
    public:
      explicit SqliteConformanceSampleRepository(SqliteTransaction& transaction)
          : transaction_(transaction) {
        transaction_.add_commit_hook([this](sqlite3* handle) { flush(handle); });
      }

      [[nodiscard]] std::optional<SqliteConformanceSample>
      find_by_id(const SqliteConformanceSample::Id& entity_id) override {
        if (const auto iterator = pending_.find(entity_id); iterator != pending_.end()) {
          return iterator->second.entity;
        }
        const auto stored = load(entity_id);
        if (!stored.has_value()) {
          return std::nullopt;
        }
        observed_versions_.insert_or_assign(entity_id, stored->version);
        return stored->entity;
      }

      // NOLINTBEGIN(readability-function-cognitive-complexity)
      [[nodiscard]] std::vector<SqliteConformanceSample>
      query(const Query<SqliteConformanceSample>& query_spec) override {
        std::string sql =
            "SELECT id, lab_id, name, status, box_id, position_label, custom_fields_json, "
            "created_at_micros, version FROM fmgr_sqlite_conformance_sample";
        std::vector<nlohmann::json> parameters;
        std::vector<std::string> predicates;

        if (!query_spec.includes_tombstoned()) {
          predicates.emplace_back("status != 'tombstoned'");
        }

        for (const auto& predicate : query_spec.predicates()) {
          const auto column = column_name(predicate.field);
          switch (predicate.op) {
          case PredicateOperator::Equal:
            predicates.push_back(column + " = ?");
            parameters.push_back(predicate.value);
            break;
          case PredicateOperator::GreaterThanOrEqual:
            predicates.push_back(column + " >= ?");
            parameters.push_back(predicate.value);
            break;
          case PredicateOperator::LessThanOrEqual:
            predicates.push_back(column + " <= ?");
            parameters.push_back(predicate.value);
            break;
          case PredicateOperator::Between:
            predicates.push_back(column + " BETWEEN ? AND ?");
            parameters.push_back(predicate.lower);
            parameters.push_back(predicate.upper);
            break;
          case PredicateOperator::In: {
            std::string clause = column + " IN (";
            for (std::size_t index = 0; index < predicate.values.size(); ++index) {
              if (index != 0) {
                clause += ", ";
              }
              clause += "?";
              parameters.push_back(predicate.values.at(index));
            }
            clause += ")";
            predicates.push_back(std::move(clause));
            break;
          }
          case PredicateOperator::JsonPathEqual:
            predicates.push_back("json_extract(" + column + ", ?) = ?");
            parameters.emplace_back(json_path(predicate.json_path));
            parameters.push_back(predicate.value);
            break;
          case PredicateOperator::ContainsCi: {
            // OR over every listed field, wildcards escaped. SQLite's default
            // LIKE folds ASCII case only (no ICU in the pinned build).
            std::string clause;
            for (std::size_t index = 0; index < predicate.fields.size(); ++index) {
              if (index != 0) {
                clause += " OR ";
              }
              clause += column_name(predicate.fields.at(index)) + " LIKE ? ESCAPE '\\'";
              parameters.emplace_back(
                  detail::like_contains_pattern(predicate.value.get<std::string>()));
            }
            predicates.push_back("(" + clause + ")");
            break;
          }
          }
        }

        if (!predicates.empty()) {
          sql += " WHERE ";
          for (std::size_t index = 0; index < predicates.size(); ++index) {
            if (index != 0) {
              sql += " AND ";
            }
            sql += predicates.at(index);
          }
        }

        if (!query_spec.sorts().empty()) {
          sql += " ORDER BY ";
          for (std::size_t index = 0; index < query_spec.sorts().size(); ++index) {
            if (index != 0) {
              sql += ", ";
            }
            const auto sort = query_spec.sorts().at(index);
            sql += column_name(sort.field);
            sql += sort.direction == SortDirection::Ascending ? " ASC" : " DESC";
          }
        }

        const auto limit = query_spec.limit_count();
        const auto offset = query_spec.offset_count();
        if (limit.has_value()) {
          sql += " LIMIT ?";
          parameters.emplace_back(static_cast<std::int64_t>(limit.value()));
        }
        if (offset.has_value()) {
          if (!limit.has_value()) {
            sql += " LIMIT -1";
          }
          sql += " OFFSET ?";
          parameters.emplace_back(static_cast<std::int64_t>(offset.value()));
        }

        Statement statement(transaction_.handle(), sql);
        bind_parameters(statement.get(), parameters);

        std::vector<SqliteConformanceSample> results;
        while (statement.step_row()) {
          const auto stored = read_sample(statement.get());
          observed_versions_.insert_or_assign(stored.entity.id, stored.version);
          results.push_back(stored.entity);
        }
        return results;
      }
      // NOLINTEND(readability-function-cognitive-complexity)

      void insert(const SqliteConformanceSample& entity, const MutationContext& context) override {
        if (pending_.contains(entity.id) || load(entity.id).has_value()) {
          throw UniqueViolation("sqlite conformance sample id already exists");
        }
        pending_.insert_or_assign(entity.id, PendingSample{.entity = entity, .is_insert = true});
        validate_active_positions();
        transaction_.note_mutation(
            std::string(EntityTraits<SqliteConformanceSample>::entity_name()),
            entity.id.to_string(), context);
      }

      void update(const SqliteConformanceSample& entity, const MutationContext& context) override {
        auto original_version = std::optional<std::uint64_t>{};
        bool is_insert = false;
        if (const auto iterator = pending_.find(entity.id); iterator != pending_.end()) {
          original_version = iterator->second.original_version;
          is_insert = iterator->second.is_insert;
        } else {
          const auto stored = load(entity.id);
          if (!stored.has_value()) {
            throw NotFound("sqlite conformance sample not found");
          }
          original_version = stored->version;
        }

        pending_.insert_or_assign(entity.id, PendingSample{.entity = entity,
                                                           .original_version = original_version,
                                                           .is_insert = is_insert});
        validate_active_positions();
        transaction_.note_mutation(
            std::string(EntityTraits<SqliteConformanceSample>::entity_name()),
            entity.id.to_string(), context);
      }

      void soft_delete(const SqliteConformanceSample::Id& entity_id,
                       const MutationContext& context) override {
        auto entity = find_by_id(entity_id);
        if (!entity.has_value()) {
          throw NotFound("sqlite conformance sample not found");
        }
        entity->status = core::SampleStatus::Tombstoned;
        update(*entity, context);
      }

    private:
      struct PendingSample {
        SqliteConformanceSample entity;
        std::optional<std::uint64_t> original_version;
        bool is_insert{false};
      };

      [[nodiscard]] static std::string json_path(const std::vector<std::string>& segments) {
        std::string path = "$";
        for (const auto& segment : segments) {
          path += ".";
          path += segment;
        }
        return path;
      }

      static void bind_json(sqlite3_stmt* statement, int index, const nlohmann::json& value) {
        if (value.is_null()) {
          bind_null(statement, index);
          return;
        }
        if (value.is_number_integer()) {
          bind_int64(statement, index, value.get<std::int64_t>());
          return;
        }
        if (value.is_number_unsigned()) {
          bind_int64(statement, index, static_cast<std::int64_t>(value.get<std::uint64_t>()));
          return;
        }
        if (value.is_boolean()) {
          bind_int64(statement, index, value.get<bool>() ? 1 : 0);
          return;
        }
        if (value.is_string()) {
          bind_text(statement, index, value.get<std::string>());
          return;
        }
        bind_text(statement, index, value.dump());
      }

      static void bind_parameters(sqlite3_stmt* statement,
                                  const std::vector<nlohmann::json>& parameters) {
        int index = 1;
        for (const auto& parameter : parameters) {
          bind_json(statement, index, parameter);
          ++index;
        }
      }

      [[nodiscard]] std::optional<StoredSample>
      load(const SqliteConformanceSample::Id& entity_id) const {
        Statement statement(
            transaction_.handle(),
            "SELECT id, lab_id, name, status, box_id, position_label, custom_fields_json, "
            "created_at_micros, version FROM fmgr_sqlite_conformance_sample WHERE id = ?");
        bind_text(statement.get(), 1, entity_id.to_string());
        if (!statement.step_row()) {
          return std::nullopt;
        }
        return read_sample(statement.get());
      }

      void validate_active_positions() const {
        std::set<std::pair<core::BoxId, std::string>> staged_positions;
        for (const auto& [unused_id, pending] : pending_) {
          (void)unused_id;
          const auto& entity = pending.entity;
          if (!is_active_occupant(entity.status) || !entity.box_id.has_value() ||
              !entity.position_label.has_value()) {
            continue;
          }
          const auto position =
              std::make_pair(entity.box_id.value(), entity.position_label.value());
          if (!staged_positions.insert(position).second) {
            throw UniqueViolation("active box position is already occupied");
          }

          Statement statement(transaction_.handle(),
                              "SELECT id FROM fmgr_sqlite_conformance_sample "
                              "WHERE box_id = ? AND position_label = ? "
                              "AND status IN ('active', 'checked_out') AND id != ? LIMIT 1");
          bind_text(statement.get(), 1, entity.box_id->to_string());
          bind_text(statement.get(), 2, entity.position_label.value());
          bind_text(statement.get(), 3, entity.id.to_string());
          if (statement.step_row()) {
            throw UniqueViolation("active box position is already occupied");
          }
        }
      }

      void flush(sqlite3* handle) {
        for (const auto& [unused_id, pending] : pending_) {
          (void)unused_id;
          if (pending.is_insert) {
            insert_pending(handle, pending.entity);
          } else {
            update_pending(handle, pending);
          }
        }
      }

      static void bind_entity(sqlite3_stmt* statement, const SqliteConformanceSample& entity) {
        bind_text(statement, 1, entity.id.to_string());
        bind_text(statement, 2, entity.lab_id.to_string());
        bind_text(statement, 3, entity.name);
        bind_text(statement, 4, std::string(core::to_string(entity.status)));
        if (entity.box_id.has_value()) {
          bind_text(statement, 5, entity.box_id->to_string());
        } else {
          bind_null(statement, 5);
        }
        if (entity.position_label.has_value()) {
          bind_text(statement, 6, entity.position_label.value());
        } else {
          bind_null(statement, 6);
        }
        bind_text(statement, 7, entity.custom_fields.dump());
        bind_int64(statement, 8, entity.created_at.unix_micros());
      }

      static void insert_pending(sqlite3* handle, const SqliteConformanceSample& entity) {
        Statement statement(handle, "INSERT INTO fmgr_sqlite_conformance_sample "
                                    "(id, lab_id, name, status, box_id, position_label, "
                                    "custom_fields_json, created_at_micros, version) "
                                    "VALUES (?, ?, ?, ?, ?, ?, ?, ?, 1)");
        bind_entity(statement.get(), entity);
        statement.step_done();
      }

      static void update_pending(sqlite3* handle, const PendingSample& pending) {
        Statement version_statement(
            handle, "SELECT version FROM fmgr_sqlite_conformance_sample WHERE id = ?");
        bind_text(version_statement.get(), 1, pending.entity.id.to_string());
        if (!version_statement.step_row()) {
          throw NotFound("sqlite conformance sample not found");
        }
        const auto current_version =
            static_cast<std::uint64_t>(sqlite3_column_int64(version_statement.get(), 0));
        if (pending.original_version.has_value() &&
            current_version != pending.original_version.value()) {
          throw SerializationFailure("serializable transaction conflict");
        }

        Statement statement(handle,
                            "UPDATE fmgr_sqlite_conformance_sample SET "
                            "id = ?, lab_id = ?, name = ?, status = ?, box_id = ?, "
                            "position_label = ?, custom_fields_json = ?, created_at_micros = ?, "
                            "version = version + 1 WHERE id = ?");
        bind_entity(statement.get(), pending.entity);
        bind_text(statement.get(), 9, pending.entity.id.to_string());
        statement.step_done();
      }

      SqliteTransaction& transaction_;
      std::map<SqliteConformanceSample::Id, PendingSample> pending_;
      std::map<SqliteConformanceSample::Id, std::uint64_t> observed_versions_;
    };

    [[nodiscard]] std::vector<SqliteMigration> sqlite_conformance_migrations() {
      return {
          SqliteMigration{
              .version = 1,
              .name = "sqlite_conformance_sample",
              .up_sql = R"sql(
CREATE TABLE IF NOT EXISTS fmgr_sqlite_conformance_sample (
  id TEXT PRIMARY KEY,
  lab_id TEXT NOT NULL,
  name TEXT NOT NULL,
  status TEXT NOT NULL,
  box_id TEXT,
  position_label TEXT,
  custom_fields_json TEXT NOT NULL CHECK (json_valid(custom_fields_json)),
  created_at_micros INTEGER NOT NULL,
  version INTEGER NOT NULL DEFAULT 1
);

CREATE UNIQUE INDEX IF NOT EXISTS fmgr_sqlite_conformance_sample_active_position_unique
  ON fmgr_sqlite_conformance_sample(box_id, position_label)
  WHERE status IN ('active', 'checked_out')
    AND box_id IS NOT NULL
    AND position_label IS NOT NULL;
)sql",
          },
      };
    }

    [[nodiscard]] std::filesystem::path database_path(std::string_view suffix) {
      const auto unique = std::to_string(static_cast<unsigned long long>(
                              ::testing::UnitTest::GetInstance()->random_seed())) +
                          "-" + std::to_string(reinterpret_cast<std::uintptr_t>(&suffix));
      return std::filesystem::temp_directory_path() /
             (std::string("freezermanager-sqlite-") + unique + "-" + std::string(suffix) + ".db");
    }

    class SqliteBackendConformanceTest : public ::testing::Test {
    protected:
      SqliteBackendConformanceTest()
          : db_path_(database_path("conformance")),
            backend_(SqliteBackendOptions{
                .database_path = db_path_.string(),
                .migrations = sqlite_conformance_migrations(),
            }) {
        backend_.register_repository_factory<SqliteConformanceSample>(
            [](SqliteTransaction& transaction) {
              return std::make_unique<SqliteConformanceSampleRepository>(transaction);
            });
      }

      void SetUp() override {
        std::filesystem::remove(db_path_);
        backend_.migrate_to_latest();
      }

      void TearDown() override {
        std::filesystem::remove(db_path_);
        std::filesystem::remove(db_path_.string() + "-wal");
        std::filesystem::remove(db_path_.string() + "-shm");
      }

      [[nodiscard]] SqliteBackend& backend() {
        return backend_;
      }

    private:
      std::filesystem::path db_path_;
      SqliteBackend backend_;
    };

    TEST_F(SqliteBackendConformanceTest, CrudRoundTripUpdatesAndSoftDeletesSample) {
      auto transaction = backend().begin(IsolationLevel::Serializable);
      auto& repository = transaction->repo<SqliteConformanceSample>();
      auto entity = sample(1, "alpha", core::Timestamp::from_unix_micros(100));

      repository.insert(entity, mutation_context());
      transaction->commit();

      transaction = backend().begin(IsolationLevel::Serializable);
      auto& read_repository = transaction->repo<SqliteConformanceSample>();
      auto stored = read_repository.find_by_id(entity.id);
      ASSERT_TRUE(stored.has_value());
      // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
      auto stored_value = stored.value();
      EXPECT_EQ(stored_value.name, "alpha");

      entity.name = "beta";
      read_repository.update(entity, mutation_context());
      read_repository.soft_delete(entity.id, mutation_context());
      transaction->commit();

      transaction = backend().begin(IsolationLevel::Serializable);
      auto& final_repository = transaction->repo<SqliteConformanceSample>();
      stored = final_repository.find_by_id(entity.id);
      ASSERT_TRUE(stored.has_value());
      // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
      stored_value = stored.value();
      EXPECT_EQ(stored_value.name, "beta");
      EXPECT_EQ(stored_value.status, core::SampleStatus::Tombstoned);
    }

    TEST_F(SqliteBackendConformanceTest, QueryDslAppliesFiltersSortingAndPagination) {
      auto transaction = backend().begin(IsolationLevel::Serializable);
      auto& repository = transaction->repo<SqliteConformanceSample>();

      auto alpha = sample(10, "alpha", core::Timestamp::from_unix_micros(100));
      auto beta = sample(11, "beta", core::Timestamp::from_unix_micros(200));
      auto gamma = sample(12, "gamma", core::Timestamp::from_unix_micros(300));
      beta.custom_fields = nlohmann::json{{"project", "beta-project"}};
      gamma.custom_fields = nlohmann::json{{"project", "beta-project"}};

      repository.insert(alpha, mutation_context());
      repository.insert(beta, mutation_context());
      repository.insert(gamma, mutation_context());
      transaction->commit();

      transaction = backend().begin(IsolationLevel::Serializable);
      auto& query_repository = transaction->repo<SqliteConformanceSample>();
      const auto query =
          Query<SqliteConformanceSample>::where(
              field<SqliteConformanceSample, core::LabId>(SqliteConformanceSample::Field::LabId) ==
              id_from_low<core::LabId>(1))
              .and_where(field<SqliteConformanceSample, core::Timestamp>(
                             SqliteConformanceSample::Field::CreatedAt)
                             .between(core::Timestamp::from_unix_micros(100),
                                      core::Timestamp::from_unix_micros(300)))
              .and_where(
                  field<SqliteConformanceSample, std::string>(SqliteConformanceSample::Field::Name)
                      .in({"beta", "gamma"}))
              .and_where(
                  json_path<SqliteConformanceSample>(SqliteConformanceSample::Field::CustomFields,
                                                     {"project"}) == "beta-project")
              .order_by(
                  field<SqliteConformanceSample, std::string>(SqliteConformanceSample::Field::Name),
                  SortDirection::Descending)
              .limit(1)
              .offset(1);

      const auto results = query_repository.query(query);

      ASSERT_EQ(results.size(), 1U);
      EXPECT_EQ(results.front().name, "beta");
    }

    // G0.4: ListSamples' `query` filter is rendered on SQLite as
    // `column LIKE ? ESCAPE '\'` with %, _ and \ escaped. These tests pin the
    // observable behaviour of that rendering.
    TEST_F(SqliteBackendConformanceTest, ContainsCiMatchesSubstringCaseInsensitively) {
      auto transaction = backend().begin(IsolationLevel::Serializable);
      auto& repository = transaction->repo<SqliteConformanceSample>();
      repository.insert(sample(60, "Alpha-1", core::Timestamp::from_unix_micros(100)),
                        mutation_context());
      repository.insert(sample(61, "beta-2", core::Timestamp::from_unix_micros(200)),
                        mutation_context());
      repository.insert(sample(62, "GAMMA-3", core::Timestamp::from_unix_micros(300)),
                        mutation_context());
      transaction->commit();

      transaction = backend().begin(IsolationLevel::Serializable);
      auto& query_repository = transaction->repo<SqliteConformanceSample>();
      const auto matches = query_repository.query(Query<SqliteConformanceSample>::where(contains_ci(
          field<SqliteConformanceSample, std::string>(SqliteConformanceSample::Field::Name),
          "PHa")));
      ASSERT_EQ(matches.size(), 1U);
      EXPECT_EQ(matches.front().name, "Alpha-1");

      const auto suffix = query_repository.query(Query<SqliteConformanceSample>::where(contains_ci(
          field<SqliteConformanceSample, std::string>(SqliteConformanceSample::Field::Name),
          "pha-")));
      ASSERT_EQ(suffix.size(), 1U);
      EXPECT_EQ(suffix.front().name, "Alpha-1");

      const auto upper = query_repository.query(Query<SqliteConformanceSample>::where(contains_ci(
          field<SqliteConformanceSample, std::string>(SqliteConformanceSample::Field::Name),
          "gamma")));
      ASSERT_EQ(upper.size(), 1U);
      EXPECT_EQ(upper.front().name, "GAMMA-3");
    }

    TEST_F(SqliteBackendConformanceTest, ContainsCiMatchesAnyListedField) {
      auto transaction = backend().begin(IsolationLevel::Serializable);
      auto& repository = transaction->repo<SqliteConformanceSample>();
      repository.insert(sample(70, "one", core::Timestamp::from_unix_micros(100)),
                        mutation_context());
      repository.insert(sample(71, "two", core::Timestamp::from_unix_micros(200)),
                        mutation_context());
      transaction->commit();

      transaction = backend().begin(IsolationLevel::Serializable);
      auto& query_repository = transaction->repo<SqliteConformanceSample>();
      const auto by_position = query_repository.query(
          Query<SqliteConformanceSample>::where(contains_ci_any<SqliteConformanceSample>(
              {SqliteConformanceSample::Field::Name, SqliteConformanceSample::Field::PositionLabel},
              "a71")));
      ASSERT_EQ(by_position.size(), 1U);
      EXPECT_EQ(by_position.front().name, "two");

      const auto by_name = query_repository.query(
          Query<SqliteConformanceSample>::where(contains_ci_any<SqliteConformanceSample>(
              {SqliteConformanceSample::Field::Name, SqliteConformanceSample::Field::PositionLabel},
              "ONE")));
      ASSERT_EQ(by_name.size(), 1U);
      EXPECT_EQ(by_name.front().name, "one");
    }

    TEST_F(SqliteBackendConformanceTest, ContainsCiTreatsWildcardsAndNonAsciiLiterally) {
      auto transaction = backend().begin(IsolationLevel::Serializable);
      auto& repository = transaction->repo<SqliteConformanceSample>();
      repository.insert(sample(80, R"(50%_x\y)", core::Timestamp::from_unix_micros(100)),
                        mutation_context());
      repository.insert(sample(81, "50abc", core::Timestamp::from_unix_micros(200)),
                        mutation_context());
      repository.insert(sample(82, "样品-Δ", core::Timestamp::from_unix_micros(300)),
                        mutation_context());
      transaction->commit();

      transaction = backend().begin(IsolationLevel::Serializable);
      auto& query_repository = transaction->repo<SqliteConformanceSample>();
      const auto query_name = [&](std::string_view needle) {
        return query_repository.query(Query<SqliteConformanceSample>::where(contains_ci(
            field<SqliteConformanceSample, std::string>(SqliteConformanceSample::Field::Name),
            needle)));
      };

      const auto percent_and_underscore = query_name("%_");
      ASSERT_EQ(percent_and_underscore.size(), 1U);
      EXPECT_EQ(percent_and_underscore.front().name, R"(50%_x\y)");

      const auto backslash = query_name(R"(x\y)");
      ASSERT_EQ(backslash.size(), 1U);
      EXPECT_EQ(backslash.front().name, R"(50%_x\y)");

      const auto lone_percent = query_name("%");
      ASSERT_EQ(lone_percent.size(), 1U);
      EXPECT_EQ(lone_percent.front().name, R"(50%_x\y)");

      const auto non_ascii = query_name("样品");
      ASSERT_EQ(non_ascii.size(), 1U);
      EXPECT_EQ(non_ascii.front().name, "样品-Δ");

      const auto non_ascii_suffix = query_name("-Δ");
      ASSERT_EQ(non_ascii_suffix.size(), 1U);
      EXPECT_EQ(non_ascii_suffix.front().name, "样品-Δ");
    }

    TEST_F(SqliteBackendConformanceTest, SoftDeletedRowsAreHiddenUnlessIncluded) {
      auto entity = sample(20, "hidden", core::Timestamp::from_unix_micros(100));
      auto transaction = backend().begin(IsolationLevel::Serializable);
      auto& repository = transaction->repo<SqliteConformanceSample>();
      repository.insert(entity, mutation_context());
      repository.soft_delete(entity.id, mutation_context());
      transaction->commit();

      transaction = backend().begin(IsolationLevel::Serializable);
      auto& query_repository = transaction->repo<SqliteConformanceSample>();
      EXPECT_TRUE(query_repository.query(Query<SqliteConformanceSample>::all()).empty());

      const auto visible =
          query_repository.query(Query<SqliteConformanceSample>::all().include_tombstoned());
      ASSERT_EQ(visible.size(), 1U);
      EXPECT_EQ(visible.front().id, entity.id);
    }

    TEST_F(SqliteBackendConformanceTest, DuplicateActiveBoxPositionThrowsPortableUniqueViolation) {
      auto left = sample(30, "left", core::Timestamp::from_unix_micros(100));
      auto right = sample(31, "right", core::Timestamp::from_unix_micros(101));
      right.box_id = left.box_id;
      right.position_label = left.position_label;

      auto transaction = backend().begin(IsolationLevel::Serializable);
      auto& repository = transaction->repo<SqliteConformanceSample>();
      repository.insert(left, mutation_context());

      EXPECT_THROW(repository.insert(right, mutation_context()), UniqueViolation);
    }

    TEST_F(SqliteBackendConformanceTest, UnsupportedEntityRepositoryThrowsPortableError) {
      auto transaction = backend().begin(IsolationLevel::Serializable);
      EXPECT_THROW((void)transaction->repo<UnsupportedSqliteConformanceEntity>(),
                   UnsupportedOperation);
    }

    TEST_F(SqliteBackendConformanceTest, SerializableOverlappingUpdatesRejectOneCommit) {
      auto entity = sample(40, "original", core::Timestamp::from_unix_micros(100));
      auto seed_transaction = backend().begin(IsolationLevel::Serializable);
      seed_transaction->repo<SqliteConformanceSample>().insert(entity, mutation_context());
      seed_transaction->commit();

      auto left_transaction = backend().begin(IsolationLevel::Serializable);
      auto right_transaction = backend().begin(IsolationLevel::Serializable);

      auto& left_repository = left_transaction->repo<SqliteConformanceSample>();
      auto& right_repository = right_transaction->repo<SqliteConformanceSample>();

      auto left = left_repository.find_by_id(entity.id);
      auto right = right_repository.find_by_id(entity.id);
      ASSERT_TRUE(left.has_value());
      ASSERT_TRUE(right.has_value());
      // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
      auto left_entity = left.value();
      // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
      auto right_entity = right.value();
      left_entity.name = "left";
      right_entity.name = "right";
      left_repository.update(left_entity, mutation_context());
      right_repository.update(right_entity, mutation_context());

      left_transaction->commit();
      EXPECT_THROW(right_transaction->commit(), SerializationFailure);
    }

    TEST_F(SqliteBackendConformanceTest, ConcurrentPlacementPreservesActivePositionUniqueness) {
      const auto stress = std::getenv("FMGR_STORAGE_STRESS") != nullptr;
      const std::size_t thread_count = stress ? 50U : 8U;
      const std::size_t attempts_per_thread = stress ? 1000U : 20U;

      std::mutex result_mutex;
      std::size_t successes = 0;
      std::size_t unique_violations = 0;
      std::vector<std::thread> threads;
      threads.reserve(thread_count);

      for (std::size_t thread_index = 0; thread_index < thread_count; ++thread_index) {
        threads.emplace_back([&, thread_index]() {
          for (std::size_t attempt = 0; attempt < attempts_per_thread; ++attempt) {
            auto entity = sample(1000 + (thread_index * attempts_per_thread) + attempt, "placed",
                                 core::Timestamp::from_unix_micros(100));
            entity.box_id = id_from_low<core::BoxId>(777);
            entity.position_label = "A1";

            try {
              auto transaction = backend().begin(IsolationLevel::Serializable);
              auto& repository = transaction->repo<SqliteConformanceSample>();
              repository.insert(entity, mutation_context());
              transaction->commit();
              std::scoped_lock lock(result_mutex);
              ++successes;
            } catch (const UniqueViolation&) {
              std::scoped_lock lock(result_mutex);
              ++unique_violations;
            } catch (const SerializationFailure&) {
              std::scoped_lock lock(result_mutex);
              ++unique_violations;
            }
          }
        });
      }

      for (auto& thread : threads) {
        thread.join();
      }

      EXPECT_EQ(successes, 1U);
      EXPECT_EQ(unique_violations, (thread_count * attempts_per_thread) - 1U);
    }

    TEST_F(SqliteBackendConformanceTest, MutationsAppendAuditEventsAtomically) {
      auto entity = sample(50, "audited", core::Timestamp::from_unix_micros(100));
      auto transaction = backend().begin(IsolationLevel::Serializable);
      transaction->repo<SqliteConformanceSample>().insert(entity, mutation_context());
      transaction->commit();

      EXPECT_EQ(backend().audit_event_count_for_tests(), 1U);
    }

    TEST_F(SqliteBackendConformanceTest, AuditAppendFailurePreventsMutationCommit) {
      auto entity = sample(60, "audit-failure", core::Timestamp::from_unix_micros(100));
      auto transaction = backend().begin(IsolationLevel::Serializable);
      transaction->repo<SqliteConformanceSample>().insert(entity, mutation_context());
      backend().fail_next_audit_append_for_tests();

      EXPECT_THROW(transaction->commit(), ConstraintViolation);

      transaction = backend().begin(IsolationLevel::Serializable);
      EXPECT_FALSE(transaction->repo<SqliteConformanceSample>().find_by_id(entity.id).has_value());
      EXPECT_EQ(backend().audit_event_count_for_tests(), 0U);
    }

    TEST_F(SqliteBackendConformanceTest, MigrationCanDowngradeAndForwardMigrateSeedData) {
      auto seed_transaction = backend().begin(IsolationLevel::Serializable);
      seed_transaction->repo<SqliteConformanceSample>().insert(
          sample(900, "seed", core::Timestamp::from_unix_micros(900)), mutation_context());
      seed_transaction->commit();

      backend().downgrade_to_zero_for_tests();
      EXPECT_EQ(backend().current_version(), SchemaVersion{0});

      backend().migrate_to_latest();
      EXPECT_EQ(backend().current_version(), SchemaVersion{1});

      auto transaction = backend().begin(IsolationLevel::Serializable);
      EXPECT_TRUE(transaction->repo<SqliteConformanceSample>()
                      .find_by_id(id_from_low<core::SampleId>(900))
                      .has_value());
    }

    // =====================================================================
    // #116: two same-rank CustomFieldDefinitions of one key
    // =====================================================================
    //
    // `resolve_custom_field_defs` ranks candidates and keeps the most specific,
    // but two definitions of one key *at the same rank* are never compared — the
    // tie-break keeps whichever row iterates last. Whether that is reachable at
    // all is a property of the schema, not of the resolver: migration 7's
    // partial unique index `cfd_lab_scope_type_key_unique` is supposed to make
    // the tie impossible. These tests pin that on the *domain* schema, so the
    // index under test is the one production deploys, and they run in both
    // insertion orders because "whichever row iterates last" is exactly what
    // must not decide the answer. The Postgres twin asserts the same thing on the
    // other backend; the risk this guards is precisely that they disagree.
    class SqliteCustomFieldUniquenessConformanceTest : public ::testing::Test {
    protected:
      void SetUp() override {
        db_path_ = database_path("cfd-uniqueness");
        std::filesystem::remove(db_path_);
        backend_ = std::make_unique<SqliteBackend>(
            SqliteBackendOptions{.database_path = db_path_.string()});
        register_identity_repositories(*backend_);
        register_item_type_repositories(*backend_);
        backend_->migrate_to_latest();
      }

      void TearDown() override {
        backend_.reset();
        std::filesystem::remove(db_path_);
        std::filesystem::remove(db_path_.string() + "-wal");
        std::filesystem::remove(db_path_.string() + "-shm");
      }

      [[nodiscard]] IStorageBackend& backend() {
        return *backend_;
      }

      // One lab and one item type, committed first: a CustomFieldDefinition
      // insert validates its lab and item_type_id against the *persisted* rows.
      void seed_lineage() {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::Lab>().insert(
            core::Lab{.id = lab_id_,
                      .name = "Lab",
                      .contact = "lab@example.org",
                      .created_at = core::Timestamp::from_unix_micros(100),
                      .settings_json = nlohmann::json::object()},
            mutation_context());
        txn->repo<core::ItemType>().insert(
            core::ItemType{.id = node_id_,
                           .lab_id = lab_id_,
                           .parent_id = std::nullopt,
                           .name = "blood",
                           .created_at = core::Timestamp::from_unix_micros(101)},
            mutation_context());
        txn->commit();
      }

      static void insert_definition(IStorageBackend& backend,
                                    const core::CustomFieldDefinition& cfd) {
        auto txn = backend.begin(IsolationLevel::Serializable);
        txn->repo<core::CustomFieldDefinition>().insert(cfd, mutation_context());
        txn->commit();
      }

      // The live (non-tombstoned) definitions of `key` in this lab.
      [[nodiscard]] std::vector<core::CustomFieldDefinition>
      live_definitions(const std::string& key) {
        auto txn = backend().begin(IsolationLevel::Serializable);
        std::vector<core::CustomFieldDefinition> matches;
        for (const auto& cfd : txn->repo<core::CustomFieldDefinition>().query(
                 Query<core::CustomFieldDefinition>::where(
                     field<core::CustomFieldDefinition, core::LabId>(
                         core::CustomFieldDefinition::Field::LabId) == lab_id_))) {
          if (cfd.key == key) {
            matches.push_back(cfd);
          }
        }
        return matches;
      }

      // What the server hands to core::validate_custom_fields for this node.
      [[nodiscard]] std::vector<core::CustomFieldDefinition> resolved() {
        auto txn = backend().begin(IsolationLevel::Serializable);
        return resolve_custom_field_defs(*txn, lab_id_, node_id_);
      }

      const core::LabId lab_id_ = id_from_low<core::LabId>(1161);
      const core::ItemTypeId node_id_ = id_from_low<core::ItemTypeId>(1162);

    private:
      std::filesystem::path db_path_;
      std::unique_ptr<SqliteBackend> backend_;
    };

    // The property is order-independent even though the survivor is not: the
    // first write of a key wins, the second is refused, and the resolver agrees
    // with the table. Reversing which definition is written first must flip the
    // survivor and nothing else.
    TEST_F(SqliteCustomFieldUniquenessConformanceTest,
           SameRankDefinitionsOfOneKeyAreRefusedInEitherInsertionOrder) {
      seed_lineage();

      // Order A: "alpha" first. A duplicate written afterwards must be refused.
      const auto alpha =
          make_conformance_cfd(1, lab_id_, node_id_, "same_rank_a", "alpha", /*required=*/false);
      insert_definition(backend(), alpha);
      const auto alpha_duplicate =
          make_conformance_cfd(2, lab_id_, node_id_, "same_rank_a", "beta", /*required=*/true);
      EXPECT_THROW(insert_definition(backend(), alpha_duplicate), UniqueViolation);

      // Order B: "beta" first. The same pair, insertion order reversed.
      const auto beta =
          make_conformance_cfd(3, lab_id_, node_id_, "same_rank_b", "beta", /*required=*/true);
      insert_definition(backend(), beta);
      const auto beta_duplicate =
          make_conformance_cfd(4, lab_id_, node_id_, "same_rank_b", "alpha", /*required=*/false);
      EXPECT_THROW(insert_definition(backend(), beta_duplicate), UniqueViolation);

      // Defined outcome, both orders: exactly one live row, the first one, and
      // the resolver returns exactly that one — not "whichever iterated last".
      const auto first_key_rows = live_definitions("same_rank_a");
      ASSERT_EQ(first_key_rows.size(), 1U);
      EXPECT_EQ(first_key_rows.front().label, "alpha");
      EXPECT_FALSE(first_key_rows.front().required);

      const auto second_key_rows = live_definitions("same_rank_b");
      ASSERT_EQ(second_key_rows.size(), 1U);
      EXPECT_EQ(second_key_rows.front().label, "beta");
      EXPECT_TRUE(second_key_rows.front().required);

      const auto definitions = resolved();
      ASSERT_EQ(definitions.size(), 2U);
      for (const auto& cfd : definitions) {
        if (cfd.key == "same_rank_a") {
          EXPECT_EQ(cfd.label, "alpha");
          EXPECT_FALSE(cfd.required);
        } else if (cfd.key == "same_rank_b") {
          EXPECT_EQ(cfd.label, "beta");
          EXPECT_TRUE(cfd.required);
        } else {
          ADD_FAILURE() << "unexpected resolved key: " << cfd.key;
        }
      }
    }

    // The same pair one rank lower: both definitions lab-global, so
    // `item_type_id` is NULL. A unique index without the COALESCE sentinel would
    // let these coexist on both engines (NULLs compare distinct), which is the
    // half of the constraint most likely to rot silently.
    TEST_F(SqliteCustomFieldUniquenessConformanceTest,
           SameRankLabGlobalDefinitionsOfOneKeyAreRefusedInEitherInsertionOrder) {
      seed_lineage();

      const auto first = make_conformance_cfd(11, lab_id_, std::nullopt, "global_key", "first",
                                              /*required=*/false);
      insert_definition(backend(), first);
      const auto duplicate = make_conformance_cfd(12, lab_id_, std::nullopt, "global_key", "second",
                                                  /*required=*/true);
      EXPECT_THROW(insert_definition(backend(), duplicate), UniqueViolation);

      const auto rows = live_definitions("global_key");
      ASSERT_EQ(rows.size(), 1U);
      EXPECT_EQ(rows.front().label, "first");

      const auto definitions = resolved();
      ASSERT_EQ(definitions.size(), 1U);
      EXPECT_EQ(definitions.front().label, "first");
      EXPECT_FALSE(definitions.front().required);
    }

    // The index is partial on `archived_at_micros IS NULL`, which is what makes
    // "archive the old definition, define the key again" legal. A constraint
    // that forgot the predicate would break that path instead.
    TEST_F(SqliteCustomFieldUniquenessConformanceTest,
           ArchivedDefinitionDoesNotBlockItsReplacement) {
      seed_lineage();

      const auto original =
          make_conformance_cfd(21, lab_id_, node_id_, "redefined", "old", /*required=*/false);
      insert_definition(backend(), original);
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::CustomFieldDefinition>().soft_delete(original.id, mutation_context());
        txn->commit();
      }

      const auto replacement =
          make_conformance_cfd(22, lab_id_, node_id_, "redefined", "new", /*required=*/true);
      insert_definition(backend(), replacement);

      const auto rows = live_definitions("redefined");
      ASSERT_EQ(rows.size(), 1U);
      EXPECT_EQ(rows.front().label, "new");

      const auto definitions = resolved();
      ASSERT_EQ(definitions.size(), 1U);
      EXPECT_EQ(definitions.front().label, "new");
      EXPECT_TRUE(definitions.front().required);
    }

  } // namespace
} // namespace fmgr::storage
