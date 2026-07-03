// SPDX-License-Identifier: AGPL-3.0-or-later

#include "storage/postgres/LoginAttemptRepositories.h"

#include "core/login_attempt.h"
#include "storage/LoginAttemptTraits.h"
#include "storage/detail/LoginAttemptColumns.h"
#include "storage/detail/QuerySqlBuilder.h"
#include "storage/postgres/PostgresRepoSupport.h"

#include <pqxx/pqxx>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace fmgr::storage {
  namespace {

    using detail::micros_or_null;
    using detail::pg_bind_params;
    using detail::pg_optional_timestamp;
    using detail::throw_pqxx_error;
    using detail::validate_login_attempt;

    [[nodiscard]] core::Timestamp now_timestamp() {
      const auto now =
          std::chrono::time_point_cast<std::chrono::microseconds>(std::chrono::system_clock::now());
      return core::Timestamp::from_unix_micros(now.time_since_epoch().count());
    }

    constexpr const char* login_attempt_columns =
        "id, email, failure_count, locked_until_micros, last_activity_micros, cleared_at_micros";

    [[nodiscard]] core::LoginAttempt read_login_attempt(pqxx::row_ref row) {
      return core::LoginAttempt{
          .id = core::LoginAttemptId::parse(row.at("id").as<std::string>()),
          .email = row.at("email").as<std::string>(),
          .failure_count = row.at("failure_count").as<std::int64_t>(),
          .locked_until = pg_optional_timestamp(row, "locked_until_micros"),
          .last_activity =
              core::Timestamp::from_unix_micros(row.at("last_activity_micros").as<std::int64_t>()),
          .cleared_at = pg_optional_timestamp(row, "cleared_at_micros"),
      };
    }

    [[nodiscard]] pqxx::params bind_login_attempt(const core::LoginAttempt& entity) {
      pqxx::params params;
      params.append(entity.id.to_string());
      params.append(entity.email);
      params.append(entity.failure_count);
      params.append(micros_or_null(entity.locked_until));
      params.append(entity.last_activity.unix_micros());
      params.append(micros_or_null(entity.cleared_at));
      return params;
    }

    class LoginAttemptRepository final : public IRepository<core::LoginAttempt> {
    public:
      explicit LoginAttemptRepository(PostgresTransaction& txn) : txn_(txn) {}

      [[nodiscard]] std::optional<core::LoginAttempt>
      find_by_id(const core::LoginAttemptId& entity_id) override {
        try {
          const auto result = txn_.work().exec(std::string("SELECT ") + login_attempt_columns +
                                                   " FROM login_attempts WHERE id = $1",
                                               pqxx::params{entity_id.to_string()});
          if (result.empty()) {
            return std::nullopt;
          }
          return read_login_attempt(result[0]);
        } catch (const pqxx::sql_error& err) {
          throw_pqxx_error(err);
        }
      }

      [[nodiscard]] std::vector<core::LoginAttempt>
      query(const Query<core::LoginAttempt>& query_spec) override {
        std::string sql = std::string("SELECT ") + login_attempt_columns + " FROM login_attempts";
        std::vector<nlohmann::json> parameters;
        const auto defaults = query_spec.includes_tombstoned()
                                  ? std::vector<std::string>{}
                                  : std::vector<std::string>{"cleared_at_micros IS NULL"};
        detail::PostgresDialect dialect;
        detail::append_where(sql, parameters, defaults, query_spec.predicates(),
                             detail::login_attempt_column_name, dialect);
        detail::append_order_limit(sql, parameters, query_spec, detail::login_attempt_column_name,
                                   dialect);
        try {
          const auto result = txn_.work().exec(sql, pg_bind_params(parameters));
          std::vector<core::LoginAttempt> results;
          for (pqxx::row_ref row : result) {
            results.push_back(read_login_attempt(row));
          }
          return results;
        } catch (const pqxx::sql_error& err) {
          throw_pqxx_error(err);
        }
      }

      void insert(const core::LoginAttempt& entity, const MutationContext& context) override {
        validate_login_attempt(entity);
        try {
          txn_.work().exec("INSERT INTO login_attempts (id, email, failure_count, "
                           "locked_until_micros, last_activity_micros, cleared_at_micros) "
                           "VALUES ($1, $2, $3, $4, $5, $6)",
                           bind_login_attempt(entity));
        } catch (const pqxx::sql_error& err) {
          throw_pqxx_error(err);
        }
        txn_.note_mutation(std::string(EntityTraits<core::LoginAttempt>::entity_name()),
                           entity.id.to_string(), context, "insert", detail::audit_after(entity));
      }

      void update(const core::LoginAttempt& entity, const MutationContext& context) override {
        validate_login_attempt(entity);
        const auto before = find_by_id(entity.id);
        write_update(entity, context, before, "update");
      }

      void soft_delete(const core::LoginAttemptId& entity_id,
                       const MutationContext& context) override {
        auto entity = find_by_id(entity_id);
        if (!entity.has_value()) {
          throw NotFound("login_attempt not found");
        }
        const auto before = entity;
        entity->cleared_at = now_timestamp();
        write_update(entity.value(), context, before, "soft_delete");
      }

    private:
      void write_update(const core::LoginAttempt& entity, const MutationContext& context,
                        const std::optional<core::LoginAttempt>& before, const char* action) {
        try {
          const auto result =
              txn_.work().exec("UPDATE login_attempts SET email = $2, failure_count = $3, "
                               "locked_until_micros = $4, last_activity_micros = $5, "
                               "cleared_at_micros = $6 WHERE id = $1",
                               bind_login_attempt(entity));
          if (result.affected_rows() == 0) {
            throw NotFound("login_attempt not found");
          }
        } catch (const pqxx::sql_error& err) {
          throw_pqxx_error(err);
        }
        txn_.note_mutation(std::string(EntityTraits<core::LoginAttempt>::entity_name()),
                           entity.id.to_string(), context, action,
                           detail::audit_change(before, entity));
      }

      PostgresTransaction& txn_;
    };

  } // namespace

  void register_login_attempt_repositories(PostgresBackend& backend) {
    backend.register_repository_factory<core::LoginAttempt>(
        [](PostgresTransaction& txn) { return std::make_unique<LoginAttemptRepository>(txn); });
  }

} // namespace fmgr::storage
