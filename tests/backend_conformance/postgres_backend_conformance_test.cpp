// SPDX-License-Identifier: AGPL-3.0-or-later

#include "storage/postgres/PostgresBackend.h"

#include "auth/AuthTypes.h"
#include "core/enums.h"
#include "core/ids.h"
#include "core/timestamp.h"
#include "rpc/AuthMiddleware.h"

#include "test_helpers.h"
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <pqxx/pqxx>

#include <array>
#include <cstdint>
#include <cstdlib>
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

    // ---- URL helper ----

    [[nodiscard]] std::optional<std::string> postgres_test_url() {
      const char* url = std::getenv("FMGR_TEST_POSTGRES_URL");
      if (url == nullptr || std::string_view(url).empty()) {
        return std::nullopt;
      }
      return std::string(url);
    }

    // ---- Conformance entity ----

    [[nodiscard]] core::Uuid uuid_from_low(std::uint64_t low_bits) {
      std::array<std::uint8_t, 16> bytes{};
      for (std::size_t i = 0; i < 8; ++i) {
        bytes.at(15 - i) = static_cast<std::uint8_t>((low_bits >> (i * 8U)) & 0xffU);
      }
      return core::Uuid(bytes);
    }

    template <typename StrongId> [[nodiscard]] StrongId id_from_low(std::uint64_t low_bits) {
      return StrongId(uuid_from_low(low_bits));
    }

    struct PgConformanceSample {
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

      friend bool operator==(const PgConformanceSample&, const PgConformanceSample&) = default;
    };

    struct UnsupportedPgConformanceEntity {
      using Id = core::BoxId;
      enum class Field : std::uint8_t { Id };
    };

  } // namespace

  template <> struct EntityTraits<PgConformanceSample> {
    using Id = PgConformanceSample::Id;
    using Field = PgConformanceSample::Field;

    [[nodiscard]] static constexpr std::string_view entity_name() {
      return "pg_conformance_sample";
    }

    [[nodiscard]] static constexpr Field tombstone_field() {
      return Field::Status;
    }
  };

  template <> struct EntityTraits<UnsupportedPgConformanceEntity> {
    using Id = UnsupportedPgConformanceEntity::Id;
    using Field = UnsupportedPgConformanceEntity::Field;

    [[nodiscard]] static constexpr std::string_view entity_name() {
      return "unsupported_pg_conformance_entity";
    }

    [[nodiscard]] static constexpr Field tombstone_field() {
      return Field::Id;
    }
  };

  namespace {

    [[nodiscard]] MutationContext mutation_context() {
      return MutationContext{
          .actor_user_id = id_from_low<core::UserId>(500),
          .actor_session_id = "pg-conformance-session",
          .request_id = "pg-conformance-request",
          .reason = "postgres backend conformance test",
          .lab_id = id_from_low<core::LabId>(1).to_string(),
      };
    }

    [[nodiscard]] PgConformanceSample sample(std::uint64_t id_low_bits, std::string name,
                                             core::Timestamp created_at) {
      return PgConformanceSample{
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

    // ---- Postgres conformance repository ----

    [[nodiscard]] std::string column_name(PgConformanceSample::Field field) {
      switch (field) {
      case PgConformanceSample::Field::Id:
        return "id";
      case PgConformanceSample::Field::LabId:
        return "lab_id";
      case PgConformanceSample::Field::Name:
        return "name";
      case PgConformanceSample::Field::Status:
        return "status";
      case PgConformanceSample::Field::BoxId:
        return "box_id";
      case PgConformanceSample::Field::PositionLabel:
        return "position_label";
      case PgConformanceSample::Field::CustomFields:
        return "custom_fields_json";
      case PgConformanceSample::Field::CreatedAt:
        return "created_at_micros";
      }
      throw ConstraintViolation("unknown pg conformance field");
    }

    [[nodiscard]] PgConformanceSample read_sample_row(pqxx::row_ref row) {
      return PgConformanceSample{
          .id = core::SampleId::parse(row.at("id").as<std::string>()),
          .lab_id = core::LabId::parse(row.at("lab_id").as<std::string>()),
          .name = row.at("name").as<std::string>(),
          .status = core::parse_sample_status(row.at("status").as<std::string>()),
          .box_id = row.at("box_id").is_null()
                        ? std::optional<core::BoxId>{}
                        : core::BoxId::parse(row.at("box_id").as<std::string>()),
          .position_label =
              row.at("position_label").is_null()
                  ? std::optional<std::string>{}
                  : std::optional<std::string>{row.at("position_label").as<std::string>()},
          .custom_fields = nlohmann::json::parse(row.at("custom_fields_json").as<std::string>()),
          .created_at =
              core::Timestamp::from_unix_micros(row.at("created_at_micros").as<std::int64_t>()),
      };
    }

    [[nodiscard]] bool is_active_occupant(core::SampleStatus status) {
      return status == core::SampleStatus::Active || status == core::SampleStatus::CheckedOut;
    }

    class PgConformanceSampleRepository final : public IRepository<PgConformanceSample> {
    public:
      explicit PgConformanceSampleRepository(PostgresTransaction& txn) : txn_(txn) {}

      [[nodiscard]] std::optional<PgConformanceSample>
      find_by_id(const PgConformanceSample::Id& entity_id) override {
        auto& work = txn_.work();
        const auto result =
            work.exec("SELECT id,lab_id,name,status,box_id,position_label,custom_fields_json,"
                      "created_at_micros FROM fmgr_pg_conformance_sample WHERE id=$1",
                      pqxx::params{entity_id.to_string()});
        if (result.empty())
          return std::nullopt;
        for (pqxx::row_ref row : result) {
          return read_sample_row(row);
        }
        return std::nullopt;
      }

      // NOLINTBEGIN(readability-function-cognitive-complexity)
      [[nodiscard]] std::vector<PgConformanceSample>
      query(const Query<PgConformanceSample>& query_spec) override {
        std::string sql = "SELECT id,lab_id,name,status,box_id,position_label,"
                          "custom_fields_json,created_at_micros FROM fmgr_pg_conformance_sample";
        pqxx::params params;
        std::vector<std::string> where_clauses;
        int param_idx = 1;

        if (!query_spec.includes_tombstoned()) {
          where_clauses.emplace_back("status != 'tombstoned'");
        }

        for (const auto& predicate : query_spec.predicates()) {
          const auto col = column_name(predicate.field);
          switch (predicate.op) {
          case PredicateOperator::Equal:
            where_clauses.push_back(col + " = $" + std::to_string(param_idx++));
            append_json_param(params, predicate.value);
            break;
          case PredicateOperator::Between:
            where_clauses.push_back(col + " BETWEEN $" + std::to_string(param_idx) + " AND $" +
                                    std::to_string(param_idx + 1));
            param_idx += 2;
            append_json_param(params, predicate.lower);
            append_json_param(params, predicate.upper);
            break;
          case PredicateOperator::In: {
            std::string in_clause = col + " IN (";
            for (std::size_t i = 0; i < predicate.values.size(); ++i) {
              if (i != 0)
                in_clause += ",";
              in_clause += "$" + std::to_string(param_idx++);
              append_json_param(params, predicate.values.at(i));
            }
            in_clause += ")";
            where_clauses.push_back(std::move(in_clause));
            break;
          }
          case PredicateOperator::JsonPathEqual:
            // For JSONB: custom_fields_json->>'key' = $N
            where_clauses.push_back(col + "->>" + pg_json_path(predicate.json_path) + " = $" +
                                    std::to_string(param_idx++));
            append_json_param(params, predicate.value);
            break;
          case PredicateOperator::GreaterThanOrEqual:
            where_clauses.push_back(col + " >= $" + std::to_string(param_idx++));
            append_json_param(params, predicate.value);
            break;
          case PredicateOperator::LessThanOrEqual:
            where_clauses.push_back(col + " <= $" + std::to_string(param_idx++));
            append_json_param(params, predicate.value);
            break;
          }
        }

        if (!where_clauses.empty()) {
          sql += " WHERE ";
          for (std::size_t i = 0; i < where_clauses.size(); ++i) {
            if (i != 0)
              sql += " AND ";
            sql += where_clauses.at(i);
          }
        }

        if (!query_spec.sorts().empty()) {
          sql += " ORDER BY ";
          for (std::size_t i = 0; i < query_spec.sorts().size(); ++i) {
            if (i != 0)
              sql += ", ";
            const auto& s = query_spec.sorts().at(i);
            sql += column_name(s.field);
            sql += s.direction == SortDirection::Ascending ? " ASC" : " DESC";
          }
        }

        if (query_spec.limit_count().has_value()) {
          sql += " LIMIT $" + std::to_string(param_idx++);
          params.append(static_cast<std::int64_t>(query_spec.limit_count().value()));
        }
        if (query_spec.offset_count().has_value()) {
          sql += " OFFSET $" + std::to_string(param_idx++);
          params.append(static_cast<std::int64_t>(query_spec.offset_count().value()));
        }

        const auto result = txn_.work().exec(sql, params);
        std::vector<PgConformanceSample> results;
        for (pqxx::row_ref row : result) {
          results.push_back(read_sample_row(row));
        }
        return results;
      }
      // NOLINTEND(readability-function-cognitive-complexity)

      void insert(const PgConformanceSample& entity, const MutationContext& context) override {
        check_position_uniqueness(entity);
        const std::optional<std::string> box_id_str =
            entity.box_id.has_value() ? std::optional<std::string>{entity.box_id->to_string()}
                                      : std::optional<std::string>{};
        const std::optional<std::string>& pos_label = entity.position_label;

        try {
          pqxx::params ins;
          ins.append(entity.id.to_string());
          ins.append(entity.lab_id.to_string());
          ins.append(entity.name);
          ins.append(std::string(core::to_string(entity.status)));
          ins.append(box_id_str);
          ins.append(pos_label);
          ins.append(entity.custom_fields.dump());
          ins.append(entity.created_at.unix_micros());
          txn_.work().exec(
              "INSERT INTO fmgr_pg_conformance_sample "
              "(id,lab_id,name,status,box_id,position_label,custom_fields_json,created_at_micros,"
              "version) VALUES ($1,$2,$3,$4,$5,$6,$7::jsonb,$8,1)",
              ins);
        } catch (const pqxx::sql_error& err) {
          const std::string_view state = err.sqlstate();
          if (state == "23505")
            throw UniqueViolation(err.what());
          if (state == "40001" || state == "40P01")
            throw SerializationFailure(err.what());
          throw BackendError(BackendErrorCode::ConstraintViolation, err.what());
        }

        txn_.note_mutation(std::string(EntityTraits<PgConformanceSample>::entity_name()),
                           entity.id.to_string(), context);
      }

      void update(const PgConformanceSample& entity, const MutationContext& context) override {
        check_position_uniqueness(entity);
        const std::optional<std::string> box_id_str =
            entity.box_id.has_value() ? std::optional<std::string>{entity.box_id->to_string()}
                                      : std::optional<std::string>{};
        const std::optional<std::string>& pos_label = entity.position_label;

        try {
          // Lock the row for update (serialization check). Under SERIALIZABLE a
          // concurrent committed update raises 40001 here, so this must share the
          // sqlstate mapping below rather than escaping as a raw pqxx error.
          const auto ver_result = txn_.work().exec(
              "SELECT version FROM fmgr_pg_conformance_sample WHERE id=$1 FOR UPDATE",
              pqxx::params{entity.id.to_string()});
          if (ver_result.empty())
            throw NotFound("pg conformance sample not found");

          pqxx::params upd;
          upd.append(entity.id.to_string());
          upd.append(entity.lab_id.to_string());
          upd.append(entity.name);
          upd.append(std::string(core::to_string(entity.status)));
          upd.append(box_id_str);
          upd.append(pos_label);
          upd.append(entity.custom_fields.dump());
          upd.append(entity.created_at.unix_micros());
          txn_.work().exec(
              "UPDATE fmgr_pg_conformance_sample SET "
              "id=$1,lab_id=$2,name=$3,status=$4,box_id=$5,position_label=$6,"
              "custom_fields_json=$7::jsonb,created_at_micros=$8,version=version+1 WHERE id=$1",
              upd);
        } catch (const pqxx::sql_error& err) {
          const std::string_view state = err.sqlstate();
          if (state == "23505")
            throw UniqueViolation(err.what());
          if (state == "40001" || state == "40P01")
            throw SerializationFailure(err.what());
          throw BackendError(BackendErrorCode::ConstraintViolation, err.what());
        }

        txn_.note_mutation(std::string(EntityTraits<PgConformanceSample>::entity_name()),
                           entity.id.to_string(), context);
      }

      void soft_delete(const PgConformanceSample::Id& entity_id,
                       const MutationContext& context) override {
        auto entity = find_by_id(entity_id);
        if (!entity.has_value())
          throw NotFound("pg conformance sample not found");
        entity->status = core::SampleStatus::Tombstoned;
        update(*entity, context);
      }

    private:
      [[nodiscard]] static std::string pg_json_path(const std::vector<std::string>& segments) {
        // For a single-level path like ["project"], return "'project'"
        // For deeper paths we'd need jsonb path operators, but conformance only uses 1-level.
        if (segments.empty())
          return "''";
        return "'" + segments.front() + "'";
      }

      static void append_json_param(pqxx::params& params, const nlohmann::json& value) {
        if (value.is_string()) {
          params.append(value.get<std::string>());
        } else if (value.is_number_integer()) {
          params.append(value.get<std::int64_t>());
        } else if (value.is_boolean()) {
          params.append(value.get<bool>());
        } else if (value.is_null()) {
          params.append(std::optional<std::string>{});
        } else {
          params.append(value.dump());
        }
      }

      void check_position_uniqueness(const PgConformanceSample& entity) {
        if (!is_active_occupant(entity.status) || !entity.box_id.has_value() ||
            !entity.position_label.has_value()) {
          return;
        }
        const auto conflict = txn_.work().exec(
            "SELECT id FROM fmgr_pg_conformance_sample "
            "WHERE box_id=$1 AND position_label=$2 AND status IN ('active','checked_out')"
            " AND id!=$3 LIMIT 1",
            pqxx::params{entity.box_id->to_string(), entity.position_label.value(),
                         entity.id.to_string()});
        if (!conflict.empty()) {
          throw UniqueViolation("active box position is already occupied");
        }
      }

      PostgresTransaction& txn_;
    };

    // ---- Migration for conformance entity ----

    [[nodiscard]] std::vector<PostgresMigration> pg_conformance_migrations() {
      // The conformance entity is a standalone table. The only shared dependency
      // the backend's audit-append path needs is audit_events (default migration
      // 1: FK-free and fully idempotent). Bundle it with the sample table as a
      // single version-1 migration so current_version() stays 1 and a
      // downgrade -> re-migrate cycle is clean (no domain RLS policies to
      // re-create). custom_fields_json is JSONB to match the real samples table
      // and the JSONB query DSL (`->>`).
      const auto audit = default_postgres_migrations().front(); // 0001_init = audit_events
      return {
          {.version = 1, .name = "pg_conformance", .up_sql = audit.up_sql + R"sql(
CREATE TABLE IF NOT EXISTS fmgr_pg_conformance_sample (
  id                  TEXT   PRIMARY KEY,
  lab_id              TEXT   NOT NULL,
  name                TEXT   NOT NULL,
  status              TEXT   NOT NULL,
  box_id              TEXT,
  position_label      TEXT,
  custom_fields_json  JSONB  NOT NULL DEFAULT '{}',
  created_at_micros   BIGINT NOT NULL,
  version             BIGINT NOT NULL DEFAULT 1
);
CREATE UNIQUE INDEX IF NOT EXISTS fmgr_pg_conformance_sample_active_position_unique
  ON fmgr_pg_conformance_sample(box_id, position_label)
  WHERE status IN ('active','checked_out')
    AND box_id IS NOT NULL
    AND position_label IS NOT NULL;
)sql"},
      };
    }

    // ---- Test fixture ----

    class PostgresBackendConformanceTest : public ::testing::Test {
    protected:
      PostgresBackendConformanceTest() {
        const auto url = postgres_test_url();
        if (!url.has_value())
          return;

        // Each test run gets a fresh schema by wiping and recreating.
        {
          pqxx::connection setup_conn(*url);
          pqxx::work txn(setup_conn);
          txn.exec("DROP SCHEMA public CASCADE");
          txn.exec("CREATE SCHEMA public");
          txn.commit();
        }

        backend_ = std::make_unique<PostgresBackend>(PostgresBackendOptions{
            .connection_string = *url,
            // Must cover the concurrency test's thread count (6, or 20 under
            // FMGR_STORAGE_STRESS); each thread holds one pooled connection for
            // its transaction. Too small -> pool-acquire BackendError escapes a
            // worker thread -> std::terminate.
            .pool_size = 24,
            .migrations = pg_conformance_migrations(),
        });
        backend_->register_repository_factory<PgConformanceSample>([](PostgresTransaction& txn) {
          return std::make_unique<PgConformanceSampleRepository>(txn);
        });
        backend_->migrate_to_latest();
      }

      void SetUp() override {
        if (!postgres_test_url().has_value()) {
          GTEST_SKIP() << "FMGR_TEST_POSTGRES_URL not set; skipping Postgres conformance tests";
        }
      }

      [[nodiscard]] PostgresBackend& backend() {
        return *backend_;
      }

    private:
      std::unique_ptr<PostgresBackend> backend_;
    };

    // ---- Conformance tests (mirrors SQLite conformance suite) ----

    TEST_F(PostgresBackendConformanceTest, CrudRoundTripUpdatesAndSoftDeletesSample) {
      auto txn = backend().begin(IsolationLevel::Serializable);
      auto entity = sample(1, "alpha", core::Timestamp::from_unix_micros(100));
      txn->repo<PgConformanceSample>().insert(entity, mutation_context());
      txn->commit();

      txn = backend().begin(IsolationLevel::Serializable);
      auto stored = txn->repo<PgConformanceSample>().find_by_id(entity.id);
      ASSERT_TRUE(stored.has_value());
      // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
      EXPECT_EQ(stored->name, "alpha");

      entity.name = "beta";
      txn->repo<PgConformanceSample>().update(entity, mutation_context());
      txn->repo<PgConformanceSample>().soft_delete(entity.id, mutation_context());
      txn->commit();

      txn = backend().begin(IsolationLevel::Serializable);
      stored = txn->repo<PgConformanceSample>().find_by_id(entity.id);
      ASSERT_TRUE(stored.has_value());
      // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
      EXPECT_EQ(stored->name, "beta");
      EXPECT_EQ(stored->status, core::SampleStatus::Tombstoned);
    }

    TEST_F(PostgresBackendConformanceTest, QueryDslAppliesFiltersSortingAndPagination) {
      auto txn = backend().begin(IsolationLevel::Serializable);
      auto alpha = sample(10, "alpha", core::Timestamp::from_unix_micros(100));
      auto beta = sample(11, "beta", core::Timestamp::from_unix_micros(200));
      auto gamma = sample(12, "gamma", core::Timestamp::from_unix_micros(300));
      beta.custom_fields = nlohmann::json{{"project", "beta-project"}};
      gamma.custom_fields = nlohmann::json{{"project", "beta-project"}};
      txn->repo<PgConformanceSample>().insert(alpha, mutation_context());
      txn->repo<PgConformanceSample>().insert(beta, mutation_context());
      txn->repo<PgConformanceSample>().insert(gamma, mutation_context());
      txn->commit();

      txn = backend().begin(IsolationLevel::Serializable);
      const auto query =
          Query<PgConformanceSample>::where(
              field<PgConformanceSample, core::LabId>(PgConformanceSample::Field::LabId) ==
              id_from_low<core::LabId>(1))
              .and_where(
                  field<PgConformanceSample, core::Timestamp>(PgConformanceSample::Field::CreatedAt)
                      .between(core::Timestamp::from_unix_micros(100),
                               core::Timestamp::from_unix_micros(300)))
              .and_where(field<PgConformanceSample, std::string>(PgConformanceSample::Field::Name)
                             .in({"beta", "gamma"}))
              .and_where(json_path<PgConformanceSample>(PgConformanceSample::Field::CustomFields,
                                                        {"project"}) == "beta-project")
              .order_by(field<PgConformanceSample, std::string>(PgConformanceSample::Field::Name),
                        SortDirection::Descending)
              .limit(1)
              .offset(1);

      const auto results = txn->repo<PgConformanceSample>().query(query);
      ASSERT_EQ(results.size(), 1U);
      EXPECT_EQ(results.front().name, "beta");
    }

    TEST_F(PostgresBackendConformanceTest, SoftDeletedRowsAreHiddenUnlessIncluded) {
      auto entity = sample(20, "hidden", core::Timestamp::from_unix_micros(100));
      auto txn = backend().begin(IsolationLevel::Serializable);
      txn->repo<PgConformanceSample>().insert(entity, mutation_context());
      txn->repo<PgConformanceSample>().soft_delete(entity.id, mutation_context());
      txn->commit();

      txn = backend().begin(IsolationLevel::Serializable);
      EXPECT_TRUE(
          txn->repo<PgConformanceSample>().query(Query<PgConformanceSample>::all()).empty());
      const auto visible = txn->repo<PgConformanceSample>().query(
          Query<PgConformanceSample>::all().include_tombstoned());
      ASSERT_EQ(visible.size(), 1U);
      EXPECT_EQ(visible.front().id, entity.id);
    }

    TEST_F(PostgresBackendConformanceTest, DuplicateActiveBoxPositionThrowsUniqueViolation) {
      auto left = sample(30, "left", core::Timestamp::from_unix_micros(100));
      auto right = sample(31, "right", core::Timestamp::from_unix_micros(101));
      right.box_id = left.box_id;
      right.position_label = left.position_label;

      auto txn = backend().begin(IsolationLevel::Serializable);
      txn->repo<PgConformanceSample>().insert(left, mutation_context());

      EXPECT_THROW(txn->repo<PgConformanceSample>().insert(right, mutation_context()),
                   UniqueViolation);
    }

    TEST_F(PostgresBackendConformanceTest, UnsupportedEntityRepositoryThrowsPortableError) {
      auto txn = backend().begin(IsolationLevel::Serializable);
      EXPECT_THROW((void)txn->repo<UnsupportedPgConformanceEntity>(), UnsupportedOperation);
    }

    TEST_F(PostgresBackendConformanceTest, SerializableOverlappingUpdatesRejectOneCommit) {
      auto entity = sample(40, "original", core::Timestamp::from_unix_micros(100));
      auto seed = backend().begin(IsolationLevel::Serializable);
      seed->repo<PgConformanceSample>().insert(entity, mutation_context());
      seed->commit();

      auto left_txn = backend().begin(IsolationLevel::Serializable);
      auto right_txn = backend().begin(IsolationLevel::Serializable);

      auto left = left_txn->repo<PgConformanceSample>().find_by_id(entity.id);
      auto right = right_txn->repo<PgConformanceSample>().find_by_id(entity.id);
      ASSERT_TRUE(left.has_value());
      ASSERT_TRUE(right.has_value());

      // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
      auto left_entity = left.value();
      // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
      auto right_entity = right.value();
      left_entity.name = "left";
      right_entity.name = "right";
      // Commit left before right writes: right still holds its pre-commit
      // snapshot, so its update (a `SELECT ... FOR UPDATE` on the row left just
      // changed) raises serialization_failure. Issuing right's update *before*
      // left commits would instead block on left's uncommitted row lock and
      // deadlock the single-threaded test.
      left_txn->repo<PgConformanceSample>().update(left_entity, mutation_context());
      left_txn->commit();

      EXPECT_THROW(
          {
            right_txn->repo<PgConformanceSample>().update(right_entity, mutation_context());
            right_txn->commit();
          },
          SerializationFailure);
    }

    TEST_F(PostgresBackendConformanceTest, ConcurrentPlacementPreservesActivePositionUniqueness) {
      const auto stress = std::getenv("FMGR_STORAGE_STRESS") != nullptr;
      const std::size_t thread_count = stress ? 20U : 6U;
      const std::size_t attempts_per_thread = stress ? 200U : 10U;

      std::mutex result_mutex;
      std::size_t successes = 0;
      std::size_t failures = 0;
      std::vector<std::thread> threads;
      threads.reserve(thread_count);

      for (std::size_t t = 0; t < thread_count; ++t) {
        threads.emplace_back([&, t]() {
          for (std::size_t attempt = 0; attempt < attempts_per_thread; ++attempt) {
            auto entity = sample(2000 + (t * attempts_per_thread) + attempt, "placed",
                                 core::Timestamp::from_unix_micros(100));
            entity.box_id = id_from_low<core::BoxId>(777);
            entity.position_label = "A1";
            try {
              auto txn = backend().begin(IsolationLevel::Serializable);
              txn->repo<PgConformanceSample>().insert(entity, mutation_context());
              txn->commit();
              std::scoped_lock lock(result_mutex);
              ++successes;
            } catch (const UniqueViolation&) {
              std::scoped_lock lock(result_mutex);
              ++failures;
            } catch (const SerializationFailure&) {
              std::scoped_lock lock(result_mutex);
              ++failures;
            } catch (const BackendError&) {
              // Any other backend-level error (e.g. pool acquire timeout) still
              // counts as a non-winning attempt; never let it escape the thread.
              std::scoped_lock lock(result_mutex);
              ++failures;
            }
          }
        });
      }
      for (auto& thread : threads)
        thread.join();

      EXPECT_EQ(successes, 1U);
      EXPECT_EQ(failures, (thread_count * attempts_per_thread) - 1U);
    }

    TEST_F(PostgresBackendConformanceTest, MutationsAppendAuditEventsAtomically) {
      auto entity = sample(50, "audited", core::Timestamp::from_unix_micros(100));
      auto txn = backend().begin(IsolationLevel::Serializable);
      txn->repo<PgConformanceSample>().insert(entity, mutation_context());
      txn->commit();

      EXPECT_EQ(backend().audit_event_count_for_tests(), 1U);
    }

    TEST_F(PostgresBackendConformanceTest, AuditAppendFailurePreventsMutationCommit) {
      auto entity = sample(60, "audit-failure", core::Timestamp::from_unix_micros(100));
      auto txn = backend().begin(IsolationLevel::Serializable);
      txn->repo<PgConformanceSample>().insert(entity, mutation_context());
      backend().fail_next_audit_append_for_tests();

      EXPECT_THROW(txn->commit(), ConstraintViolation);

      txn = backend().begin(IsolationLevel::Serializable);
      EXPECT_FALSE(txn->repo<PgConformanceSample>().find_by_id(entity.id).has_value());
      EXPECT_EQ(backend().audit_event_count_for_tests(), 0U);
    }

    TEST_F(PostgresBackendConformanceTest, MigrationCanDowngradeAndForwardMigrateSeedData) {
      auto seed = backend().begin(IsolationLevel::Serializable);
      seed->repo<PgConformanceSample>().insert(
          sample(900, "seed", core::Timestamp::from_unix_micros(900)), mutation_context());
      seed->commit();

      backend().downgrade_to_zero_for_tests();
      EXPECT_EQ(backend().current_version(), SchemaVersion{0});

      backend().migrate_to_latest();
      EXPECT_EQ(backend().current_version(), SchemaVersion{1});

      auto txn = backend().begin(IsolationLevel::Serializable);
      EXPECT_TRUE(txn->repo<PgConformanceSample>()
                      .find_by_id(id_from_low<core::SampleId>(900))
                      .has_value());
    }

    // ---- Postgres-specific: RLS isolation via AuthMiddleware ----

    // Fixture using full domain migrations to test RLS on real domain tables.
    //
    // The postgres superuser bypasses RLS even with FORCE ROW LEVEL SECURITY.
    // To exercise RLS policies we create a non-superuser role (fmgr_rls_tester)
    // and use SET LOCAL ROLE inside each test transaction so queries run under
    // that role, which IS subject to the policies.
    class PostgresRlsIntegrationTest : public ::testing::Test {
    protected:
      PostgresRlsIntegrationTest() {
        const auto url = postgres_test_url();
        if (!url.has_value())
          return;

        // Fresh schema for each test class instantiation.
        {
          pqxx::connection setup_conn(*url);
          pqxx::work txn(setup_conn);
          txn.exec("DROP SCHEMA public CASCADE");
          txn.exec("CREATE SCHEMA public");
          txn.commit();
        }

        backend_ = std::make_unique<PostgresBackend>(
            PostgresBackendOptions{.connection_string = *url, .pool_size = 2});
        backend_->migrate_to_latest();

        // Grant the non-superuser role access to the migrated schema.
        // This must happen after migrate_to_latest so the tables exist.
        {
          pqxx::connection grant_conn(*url);
          pqxx::work grant_txn(grant_conn);
          // Create the role idempotently.
          grant_txn.exec(
              "DO $$ BEGIN "
              "  IF NOT EXISTS (SELECT 1 FROM pg_roles WHERE rolname='fmgr_rls_tester') THEN "
              "    CREATE ROLE fmgr_rls_tester; "
              "  END IF; "
              "END $$");
          grant_txn.exec("GRANT USAGE ON SCHEMA public TO fmgr_rls_tester");
          grant_txn.exec("GRANT SELECT, INSERT ON ALL TABLES IN SCHEMA public TO fmgr_rls_tester");
          grant_txn.commit();
        }

        // Seed two labs and a storage_container scoped to lab_a_ (superuser bypasses RLS).
        {
          pqxx::connection seed_conn(*url);
          pqxx::work seed_txn(seed_conn);
          const auto now = static_cast<std::int64_t>(1'000'000LL);
          seed_txn.exec(
              "INSERT INTO labs (id,name,contact,created_at_micros,settings_json,is_phi_enabled) "
              "VALUES ($1,'Lab A','a@lab.org',$3,'{}',false),"
              "       ($2,'Lab B','b@lab.org',$3,'{}',false)",
              pqxx::params{lab_a_.to_string(), lab_b_.to_string(), now});
          seed_txn.exec(
              "INSERT INTO storage_containers "
              "(id,lab_id,kind,name,label,ordering_index,capacity_hint_json,created_at_micros) "
              "VALUES ($1,$2,'shelf','Shelf A','shelf-a',0,'{}', $3)",
              pqxx::params{container_id_.to_string(), lab_a_.to_string(), now});
          seed_txn.commit();
        }
      }

      void SetUp() override {
        if (!postgres_test_url().has_value()) {
          GTEST_SKIP() << "FMGR_TEST_POSTGRES_URL not set; skipping RLS integration tests";
        }
      }

      [[nodiscard]] PostgresBackend& backend() {
        return *backend_;
      }

      [[nodiscard]] auth::SessionContext ctx_for_lab(core::LabId lab) {
        return auth::SessionContext{
            .session_id = id_from_low<core::SessionId>(998),
            .user_id = id_from_low<core::UserId>(999),
            .permissions_by_lab = {{lab, {}}},
            .mfa_complete = true,
        };
      }

      const core::LabId lab_a_ = id_from_low<core::LabId>(10);
      const core::LabId lab_b_ = id_from_low<core::LabId>(20);
      const core::StorageContainerId container_id_ = id_from_low<core::StorageContainerId>(30);

    private:
      std::unique_ptr<PostgresBackend> backend_;
    };

    TEST_F(PostgresRlsIntegrationTest, InjectRlsVarsBlocksWrongLab) {
      auto txn = backend().begin(IsolationLevel::ReadCommitted);
      auto* pg_txn = dynamic_cast<storage::PostgresTransaction*>(txn.get());
      ASSERT_NE(pg_txn, nullptr);

      // Set session vars for lab_b_ — the container belongs to lab_a_.
      rpc::AuthMiddleware::inject_rls_vars(*txn, ctx_for_lab(lab_b_));
      // Switch to the non-superuser role so RLS policies actually apply.
      pg_txn->work().exec("SET LOCAL ROLE fmgr_rls_tester");

      const auto result = pg_txn->work().exec("SELECT id FROM storage_containers WHERE id=$1",
                                              pqxx::params{container_id_.to_string()});
      EXPECT_TRUE(result.empty()) << "RLS should block cross-lab access";
      txn->rollback();
    }

    TEST_F(PostgresRlsIntegrationTest, InjectRlsVarsAllowsCorrectLab) {
      auto txn = backend().begin(IsolationLevel::ReadCommitted);
      auto* pg_txn = dynamic_cast<storage::PostgresTransaction*>(txn.get());
      ASSERT_NE(pg_txn, nullptr);

      // Set session vars for lab_a_ — the container belongs to lab_a_.
      rpc::AuthMiddleware::inject_rls_vars(*txn, ctx_for_lab(lab_a_));
      // Switch to the non-superuser role so RLS policies actually apply.
      pg_txn->work().exec("SET LOCAL ROLE fmgr_rls_tester");

      const auto result = pg_txn->work().exec("SELECT id FROM storage_containers WHERE id=$1",
                                              pqxx::params{container_id_.to_string()});
      EXPECT_FALSE(result.empty()) << "RLS should allow access for the owning lab";
      txn->rollback();
    }

    TEST_F(PostgresRlsIntegrationTest, InjectRlsVarsSetsCorrectSessionVariable) {
      // Verify inject_rls_vars sets the right key (no double-prefix, B1 regression check).
      auto txn = backend().begin(IsolationLevel::ReadCommitted);
      auto* pg_txn = dynamic_cast<storage::PostgresTransaction*>(txn.get());
      ASSERT_NE(pg_txn, nullptr);

      rpc::AuthMiddleware::inject_rls_vars(*txn, ctx_for_lab(lab_a_));

      const auto result =
          pg_txn->work().exec("SELECT current_setting('app.current_lab_ids', true)");
      ASSERT_FALSE(result.empty());
      // Must match lab_a_'s UUID string, not 'app.current_lab_ids' or empty.
      EXPECT_EQ(result[0][0].as<std::string>(), lab_a_.to_string());
      txn->rollback();
    }

    // ---- Postgres-specific: misc capabilities ----

    TEST_F(PostgresBackendConformanceTest, CapabilitiesReportRowLevelSecurity) {
      EXPECT_TRUE(backend().caps().row_level_security);
      EXPECT_TRUE(backend().caps().json_path_equality);
      EXPECT_TRUE(backend().caps().json_path_indexes);
      EXPECT_TRUE(backend().caps().listen_notify);
    }

    TEST_F(PostgresBackendConformanceTest, MigrateToLatestIdempotent) {
      // Calling migrate_to_latest() twice must not fail or change the version.
      backend().migrate_to_latest();
      const auto version = backend().current_version();
      EXPECT_EQ(version, SchemaVersion{1});
    }

    TEST_F(PostgresBackendConformanceTest, SetSessionVarVisibleWithinTransaction) {
      // PostgresBackend::begin() returns unique_ptr<ITransaction>; cast for work() access.
      auto itxn = backend().begin(IsolationLevel::Serializable);
      auto* txn = dynamic_cast<PostgresTransaction*>(itxn.get());
      ASSERT_NE(txn, nullptr);

      // Set a custom app session variable and verify it's visible in the same txn.
      txn->set_session_var("current_lab_ids", "lab-a,lab-b");

      const auto result = txn->work().exec("SELECT current_setting('app.current_lab_ids', true)");
      ASSERT_FALSE(result.empty());
      for (pqxx::row_ref row : result) {
        EXPECT_EQ(row.at(0).as<std::string>(), "lab-a,lab-b");
        break;
      }

      itxn->rollback();

      // After rollback (SET LOCAL), new transaction sees empty string.
      auto itxn2 = backend().begin(IsolationLevel::Serializable);
      auto* txn2 = dynamic_cast<PostgresTransaction*>(itxn2.get());
      ASSERT_NE(txn2, nullptr);
      const auto result2 = txn2->work().exec("SELECT current_setting('app.current_lab_ids', true)");
      ASSERT_FALSE(result2.empty());
      for (pqxx::row_ref row : result2) {
        EXPECT_EQ(row.at(0).as<std::string>(), "");
        break;
      }
    }

  } // namespace
} // namespace fmgr::storage
