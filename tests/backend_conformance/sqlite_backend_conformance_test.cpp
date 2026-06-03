// SPDX-License-Identifier: AGPL-3.0-or-later

#include "storage/sqlite/SqliteBackend.h"

#include "core/enums.h"
#include "core/ids.h"
#include "core/timestamp.h"

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

        if (query_spec.limit_count().has_value()) {
          sql += " LIMIT ?";
          parameters.emplace_back(static_cast<std::int64_t>(query_spec.limit_count().value()));
        }
        if (query_spec.offset_count().has_value()) {
          if (!query_spec.limit_count().has_value()) {
            sql += " LIMIT -1";
          }
          sql += " OFFSET ?";
          parameters.emplace_back(static_cast<std::int64_t>(query_spec.offset_count().value()));
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

  } // namespace
} // namespace fmgr::storage
