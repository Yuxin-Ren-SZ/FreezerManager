// SPDX-License-Identifier: AGPL-3.0-or-later

#include "storage/postgres/SessionRepositories.h"

#include "core/session.h"
#include "storage/SessionTraits.h"
#include "storage/detail/QuerySqlBuilder.h"
#include "storage/detail/SessionColumns.h"
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

    using detail::id_or_null;
    using detail::micros_or_null;
    using detail::pg_bind_params;
    using detail::pg_optional_id;
    using detail::pg_optional_string;
    using detail::pg_optional_timestamp;
    using detail::throw_pqxx_error;
    using detail::validate_api_token;
    using detail::validate_session;

    [[nodiscard]] core::Timestamp now_timestamp() {
      const auto now =
          std::chrono::time_point_cast<std::chrono::microseconds>(std::chrono::system_clock::now());
      return core::Timestamp::from_unix_micros(now.time_since_epoch().count());
    }

    // ---- Session ----

    constexpr const char* session_columns =
        "id, user_id, token_hash, token_prefix, created_at_micros, last_seen_at_micros, ip, "
        "user_agent, revoked_at_micros, mfa_complete";

    [[nodiscard]] core::Session read_session(pqxx::row_ref row) {
      return core::Session{
          .id = core::SessionId::parse(row.at("id").as<std::string>()),
          .user_id = core::UserId::parse(row.at("user_id").as<std::string>()),
          .token_hash = row.at("token_hash").as<std::string>(),
          .token_prefix = row.at("token_prefix").as<std::string>(),
          .created_at =
              core::Timestamp::from_unix_micros(row.at("created_at_micros").as<std::int64_t>()),
          .last_seen_at =
              core::Timestamp::from_unix_micros(row.at("last_seen_at_micros").as<std::int64_t>()),
          .ip = pg_optional_string(row, "ip"),
          .user_agent = pg_optional_string(row, "user_agent"),
          .revoked_at = pg_optional_timestamp(row, "revoked_at_micros"),
          .mfa_complete = row.at("mfa_complete").as<bool>(),
      };
    }

    [[nodiscard]] pqxx::params bind_session(const core::Session& entity) {
      pqxx::params params;
      params.append(entity.id.to_string());
      params.append(entity.user_id.to_string());
      params.append(entity.token_hash);
      params.append(entity.token_prefix);
      params.append(entity.created_at.unix_micros());
      params.append(entity.last_seen_at.unix_micros());
      params.append(entity.ip);
      params.append(entity.user_agent);
      params.append(micros_or_null(entity.revoked_at));
      params.append(entity.mfa_complete);
      return params;
    }

    class SessionRepository final : public IRepository<core::Session> {
    public:
      explicit SessionRepository(PostgresTransaction& txn) : txn_(txn) {}

      [[nodiscard]] std::optional<core::Session>
      find_by_id(const core::SessionId& entity_id) override {
        try {
          const auto result = txn_.work().exec(std::string("SELECT ") + session_columns +
                                                   " FROM sessions WHERE id = $1",
                                               pqxx::params{entity_id.to_string()});
          if (result.empty()) {
            return std::nullopt;
          }
          return read_session(result[0]);
        } catch (const pqxx::sql_error& err) {
          throw_pqxx_error(err);
        }
      }

      [[nodiscard]] std::vector<core::Session>
      query(const Query<core::Session>& query_spec) override {
        std::string sql = std::string("SELECT ") + session_columns + " FROM sessions";
        std::vector<nlohmann::json> parameters;
        const auto defaults = query_spec.includes_tombstoned()
                                  ? std::vector<std::string>{}
                                  : std::vector<std::string>{"revoked_at_micros IS NULL"};
        detail::PostgresDialect dialect;
        detail::append_where(sql, parameters, defaults, query_spec.predicates(),
                             detail::session_column_name, dialect);
        detail::append_order_limit(sql, parameters, query_spec, detail::session_column_name,
                                   dialect);
        try {
          const auto result = txn_.work().exec(sql, pg_bind_params(parameters));
          std::vector<core::Session> results;
          for (pqxx::row_ref row : result) {
            results.push_back(read_session(row));
          }
          return results;
        } catch (const pqxx::sql_error& err) {
          throw_pqxx_error(err);
        }
      }

      void insert(const core::Session& entity, const MutationContext& context) override {
        validate_session(entity);
        try {
          txn_.work().exec("INSERT INTO sessions (id, user_id, token_hash, token_prefix, "
                           "created_at_micros, last_seen_at_micros, ip, user_agent, "
                           "revoked_at_micros, mfa_complete) "
                           "VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10)",
                           bind_session(entity));
        } catch (const pqxx::sql_error& err) {
          throw_pqxx_error(err);
        }
        txn_.note_mutation(std::string(EntityTraits<core::Session>::entity_name()),
                           entity.id.to_string(), context);
      }

      void update(const core::Session& entity, const MutationContext& context) override {
        validate_session(entity);
        write_update(entity, context);
      }

      void soft_delete(const core::SessionId& entity_id, const MutationContext& context) override {
        auto entity = find_by_id(entity_id);
        if (!entity.has_value()) {
          throw NotFound("session not found");
        }
        entity->revoked_at = now_timestamp();
        write_update(entity.value(), context);
      }

    private:
      void write_update(const core::Session& entity, const MutationContext& context) {
        try {
          const auto result = txn_.work().exec(
              "UPDATE sessions SET user_id = $2, token_hash = $3, "
              "token_prefix = $4, created_at_micros = $5, last_seen_at_micros = $6, "
              "ip = $7, user_agent = $8, revoked_at_micros = $9, mfa_complete = $10 "
              "WHERE id = $1",
              bind_session(entity));
          if (result.affected_rows() == 0) {
            throw NotFound("session not found");
          }
        } catch (const pqxx::sql_error& err) {
          throw_pqxx_error(err);
        }
        txn_.note_mutation(std::string(EntityTraits<core::Session>::entity_name()),
                           entity.id.to_string(), context);
      }

      PostgresTransaction& txn_;
    };

    // ---- ApiToken ----

    constexpr const char* api_token_columns =
        "id, user_id, lab_id, name, scope_json, token_hash, token_prefix, created_at_micros, "
        "expires_at_micros, revoked_at_micros";

    [[nodiscard]] core::ApiToken read_api_token(pqxx::row_ref row) {
      return core::ApiToken{
          .id = core::ApiTokenId::parse(row.at("id").as<std::string>()),
          .user_id = core::UserId::parse(row.at("user_id").as<std::string>()),
          .lab_id = pg_optional_id<core::LabId>(row, "lab_id"),
          .name = row.at("name").as<std::string>(),
          .scope_json = row.at("scope_json").as<std::string>(),
          .token_hash = row.at("token_hash").as<std::string>(),
          .token_prefix = row.at("token_prefix").as<std::string>(),
          .created_at =
              core::Timestamp::from_unix_micros(row.at("created_at_micros").as<std::int64_t>()),
          .expires_at = pg_optional_timestamp(row, "expires_at_micros"),
          .revoked_at = pg_optional_timestamp(row, "revoked_at_micros"),
      };
    }

    [[nodiscard]] pqxx::params bind_api_token(const core::ApiToken& entity) {
      pqxx::params params;
      params.append(entity.id.to_string());
      params.append(entity.user_id.to_string());
      params.append(id_or_null(entity.lab_id));
      params.append(entity.name);
      params.append(entity.scope_json);
      params.append(entity.token_hash);
      params.append(entity.token_prefix);
      params.append(entity.created_at.unix_micros());
      params.append(micros_or_null(entity.expires_at));
      params.append(micros_or_null(entity.revoked_at));
      return params;
    }

    class ApiTokenRepository final : public IRepository<core::ApiToken> {
    public:
      explicit ApiTokenRepository(PostgresTransaction& txn) : txn_(txn) {}

      [[nodiscard]] std::optional<core::ApiToken>
      find_by_id(const core::ApiTokenId& entity_id) override {
        try {
          const auto result = txn_.work().exec(std::string("SELECT ") + api_token_columns +
                                                   " FROM api_tokens WHERE id = $1",
                                               pqxx::params{entity_id.to_string()});
          if (result.empty()) {
            return std::nullopt;
          }
          return read_api_token(result[0]);
        } catch (const pqxx::sql_error& err) {
          throw_pqxx_error(err);
        }
      }

      [[nodiscard]] std::vector<core::ApiToken>
      query(const Query<core::ApiToken>& query_spec) override {
        std::string sql = std::string("SELECT ") + api_token_columns + " FROM api_tokens";
        std::vector<nlohmann::json> parameters;
        const auto defaults = query_spec.includes_tombstoned()
                                  ? std::vector<std::string>{}
                                  : std::vector<std::string>{"revoked_at_micros IS NULL"};
        detail::PostgresDialect dialect;
        detail::append_where(sql, parameters, defaults, query_spec.predicates(),
                             detail::api_token_column_name, dialect);
        detail::append_order_limit(sql, parameters, query_spec, detail::api_token_column_name,
                                   dialect);
        try {
          const auto result = txn_.work().exec(sql, pg_bind_params(parameters));
          std::vector<core::ApiToken> results;
          for (pqxx::row_ref row : result) {
            results.push_back(read_api_token(row));
          }
          return results;
        } catch (const pqxx::sql_error& err) {
          throw_pqxx_error(err);
        }
      }

      void insert(const core::ApiToken& entity, const MutationContext& context) override {
        validate_api_token(entity);
        try {
          txn_.work().exec(
              "INSERT INTO api_tokens (id, user_id, lab_id, name, scope_json, token_hash, "
              "token_prefix, created_at_micros, expires_at_micros, revoked_at_micros) "
              "VALUES ($1, $2, $3, $4, $5::jsonb, $6, $7, $8, $9, $10)",
              bind_api_token(entity));
        } catch (const pqxx::sql_error& err) {
          throw_pqxx_error(err);
        }
        txn_.note_mutation(std::string(EntityTraits<core::ApiToken>::entity_name()),
                           entity.id.to_string(), context);
      }

      void update(const core::ApiToken& entity, const MutationContext& context) override {
        validate_api_token(entity);
        write_update(entity, context);
      }

      void soft_delete(const core::ApiTokenId& entity_id, const MutationContext& context) override {
        auto entity = find_by_id(entity_id);
        if (!entity.has_value()) {
          throw NotFound("api_token not found");
        }
        entity->revoked_at = now_timestamp();
        write_update(entity.value(), context);
      }

    private:
      void write_update(const core::ApiToken& entity, const MutationContext& context) {
        try {
          const auto result = txn_.work().exec(
              "UPDATE api_tokens SET user_id = $2, lab_id = $3, name = $4, scope_json = $5::jsonb, "
              "token_hash = $6, token_prefix = $7, created_at_micros = $8, expires_at_micros = $9, "
              "revoked_at_micros = $10 WHERE id = $1",
              bind_api_token(entity));
          if (result.affected_rows() == 0) {
            throw NotFound("api_token not found");
          }
        } catch (const pqxx::sql_error& err) {
          throw_pqxx_error(err);
        }
        txn_.note_mutation(std::string(EntityTraits<core::ApiToken>::entity_name()),
                           entity.id.to_string(), context);
      }

      PostgresTransaction& txn_;
    };

  } // namespace

  void register_session_repositories(PostgresBackend& backend) {
    backend.register_repository_factory<core::Session>(
        [](PostgresTransaction& txn) { return std::make_unique<SessionRepository>(txn); });
    backend.register_repository_factory<core::ApiToken>(
        [](PostgresTransaction& txn) { return std::make_unique<ApiTokenRepository>(txn); });
  }

} // namespace fmgr::storage
