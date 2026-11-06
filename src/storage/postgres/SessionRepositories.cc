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
        "user_agent, revoked_at_micros, mfa_complete, totp_verified_generation, "
        "totp_verified_at_micros";

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
          .totp_verified_generation = [&]() -> std::optional<core::Uuid> {
            const auto value = pg_optional_string(row, "totp_verified_generation");
            return value ? std::make_optional(core::Uuid::parse(*value)) : std::nullopt;
          }(),
          .totp_verified_at = pg_optional_timestamp(row, "totp_verified_at_micros"),
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
      params.append(entity.totp_verified_generation
                        ? std::make_optional(entity.totp_verified_generation->to_string())
                        : std::nullopt);
      params.append(micros_or_null(entity.totp_verified_at));
      return params;
    }

    class SessionRepository final : public IRepository<core::Session>,
                                    public detail::TotpSessionWriter {
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
        if (entity.totp_verified_generation || entity.totp_verified_at) {
          throw ConstraintViolation("generic session insert cannot supply TOTP proof");
        }
        validate_session(entity);
        try {
          txn_.work().exec(
              "INSERT INTO sessions (id, user_id, token_hash, token_prefix, "
              "created_at_micros, last_seen_at_micros, ip, user_agent, "
              "revoked_at_micros, mfa_complete, totp_verified_generation, totp_verified_at_micros) "
              "VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12)",
              bind_session(entity));
        } catch (const pqxx::sql_error& err) {
          throw_pqxx_error(err);
        }
        txn_.note_mutation(std::string(EntityTraits<core::Session>::entity_name()),
                           entity.id.to_string(), context, "insert", detail::audit_after(entity));
      }

      void update(const core::Session& entity, const MutationContext& context) override {
        validate_session(entity);
        const auto before = find_by_id(entity.id);
        if (!before.has_value()) {
          throw NotFound("session not found");
        }
        require_owner_lock(before->user_id);
        if (entity.user_id != before->user_id) {
          throw ConstraintViolation("session ownership cannot change");
        }
        if (entity.totp_verified_generation != before->totp_verified_generation ||
            entity.totp_verified_at != before->totp_verified_at) {
          throw ConstraintViolation("generic session update cannot alter TOTP proof");
        }
        write_update(entity, context, before, "update");
      }

      void soft_delete(const core::SessionId& entity_id, const MutationContext& context) override {
        auto entity = find_by_id(entity_id);
        if (!entity.has_value()) {
          throw NotFound("session not found");
        }
        require_owner_lock(entity->user_id);
        const auto before = entity;
        entity->revoked_at = now_timestamp();
        write_update(entity.value(), context, before, "soft_delete");
      }

      void write_session(const detail::MfaWriteCapability&, const core::Session& entity,
                         const MutationContext& context, const std::string& action) override {
        require_owner_lock(entity.user_id);
        validate_session(entity);
        write_update(entity, context, find_by_id(entity.id), action);
      }

    private:
      void require_owner_lock(const core::UserId& user_id) const {
        if (txn_.auth_user_lock_status(user_id) != AuthUserLockStatus::Locked) {
          throw ConstraintViolation("session mutation requires its owning user lock");
        }
      }

      void write_update(const core::Session& entity, const MutationContext& context,
                        const std::optional<core::Session>& before, const std::string& action) {
        try {
          const auto result = txn_.work().exec(
              "UPDATE sessions SET user_id = $2, token_hash = $3, "
              "token_prefix = $4, created_at_micros = $5, last_seen_at_micros = $6, "
              "ip = $7, user_agent = $8, revoked_at_micros = $9, mfa_complete = $10, "
              "totp_verified_generation = $11, totp_verified_at_micros = $12 "
              "WHERE id = $1",
              bind_session(entity));
          if (result.affected_rows() == 0) {
            throw NotFound("session not found");
          }
        } catch (const pqxx::sql_error& err) {
          throw_pqxx_error(err);
        }
        txn_.note_mutation(std::string(EntityTraits<core::Session>::entity_name()),
                           entity.id.to_string(), context, action,
                           detail::audit_change(before, entity));
      }

      PostgresTransaction& txn_;
    };

    class MfaStateRepository final : public IRepository<core::MfaSecurityState>,
                                     public detail::MfaStateWriter {
    public:
      explicit MfaStateRepository(PostgresTransaction& txn) : txn_(txn) {}
      [[nodiscard]] std::optional<core::MfaSecurityState>
      find_by_id(const core::UserId& user_id) override {
        const auto rows = query(Query<core::MfaSecurityState>::where(
            field<core::MfaSecurityState, core::UserId>(core::MfaSecurityState::Field::UserId) ==
            user_id));
        return rows.empty() ? std::nullopt : std::make_optional(rows.front());
      }
      [[nodiscard]] std::vector<core::MfaSecurityState>
      query(const Query<core::MfaSecurityState>& spec) override {
        std::string sql =
            "SELECT user_id, last_accepted_step, failure_count, next_allowed_at_micros, "
            "observed_at_micros, totp_generation FROM mfa_security_states";
        std::vector<nlohmann::json> parameters;
        detail::PostgresDialect dialect;
        detail::append_where(sql, parameters, {}, spec.predicates(), detail::mfa_state_column,
                             dialect);
        detail::append_order_limit(sql, parameters, spec, detail::mfa_state_column, dialect);
        try {
          std::vector<core::MfaSecurityState> result;
          for (pqxx::row_ref row : txn_.work().exec(sql, pg_bind_params(parameters))) {
            const auto generation = pg_optional_string(row, "totp_generation");
            result.push_back(core::MfaSecurityState{
                .user_id = core::UserId::parse(row.at("user_id").as<std::string>()),
                .last_accepted_step =
                    row.at("last_accepted_step").is_null()
                        ? std::nullopt
                        : std::make_optional(row.at("last_accepted_step").as<std::int64_t>()),
                .failure_count = row.at("failure_count").as<std::int32_t>(),
                .next_allowed_at = pg_optional_timestamp(row, "next_allowed_at_micros"),
                .observed_at = pg_optional_timestamp(row, "observed_at_micros"),
                .totp_generation = generation ? std::make_optional(core::Uuid::parse(*generation))
                                              : std::nullopt});
          }
          return result;
        } catch (const pqxx::sql_error& error) {
          throw_pqxx_error(error);
        }
      }
      void insert(const core::MfaSecurityState&, const MutationContext&) override {
        throw ConstraintViolation("MFA state initialization requires its backend lifecycle");
      }
      void soft_delete(const core::UserId&, const MutationContext&) override {
        throw ConstraintViolation("MFA security state cannot be deleted");
      }
      void update(const core::MfaSecurityState& state, const MutationContext& ctx) override {
        detail::require_mfa_owner_lock(txn_, state.user_id);
        const auto before = find_by_id(state.user_id);
        if (!before) {
          throw NotFound("MFA state not found");
        }
        detail::validate_mfa_update(*before, state);
        write(state, ctx, before, "auth.mfa_state_updated");
      }
      void write_state(const detail::MfaWriteCapability&, const core::MfaSecurityState& state,
                       const MutationContext& ctx, bool initialize) override {
        detail::require_mfa_owner_lock(txn_, state.user_id);
        detail::validate_mfa_state(state);
        const auto before = find_by_id(state.user_id);
        if (initialize == before.has_value()) {
          throw ConstraintViolation("invalid MFA initialization state");
        }
        write(state, ctx, before,
              initialize ? "auth.mfa_state_initialized" : "auth.mfa_generation_changed");
      }

    private:
      void write(const core::MfaSecurityState& state, const MutationContext& ctx,
                 const std::optional<core::MfaSecurityState>& before, const std::string& action) {
        try {
          const auto generation = state.totp_generation
                                      ? std::make_optional(state.totp_generation->to_string())
                                      : std::nullopt;
          const pqxx::params params{state.user_id.to_string(),
                                    state.last_accepted_step,
                                    state.failure_count,
                                    micros_or_null(state.next_allowed_at),
                                    micros_or_null(state.observed_at),
                                    generation};
          txn_.work().exec(before
                               ? "UPDATE mfa_security_states SET last_accepted_step = $2, "
                                 "failure_count = $3, next_allowed_at_micros = $4, "
                                 "observed_at_micros = $5, totp_generation = $6 WHERE user_id = $1"
                               : "INSERT INTO mfa_security_states (user_id, last_accepted_step, "
                                 "failure_count, next_allowed_at_micros, observed_at_micros, "
                                 "totp_generation) VALUES ($1, $2, $3, $4, $5, $6)",
                           params);
        } catch (const pqxx::sql_error& error) {
          throw_pqxx_error(error);
        }
        txn_.note_mutation("mfa_security_state", state.user_id.to_string(), ctx, action,
                           AuditSnapshot{.before = before
                                                       ? std::make_optional(nlohmann::json(*before))
                                                       : std::nullopt,
                                         .after = nlohmann::json(state)});
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
              "VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10)",
              bind_api_token(entity));
        } catch (const pqxx::sql_error& err) {
          throw_pqxx_error(err);
        }
        txn_.note_mutation(std::string(EntityTraits<core::ApiToken>::entity_name()),
                           entity.id.to_string(), context, "insert", detail::audit_after(entity));
      }

      void update(const core::ApiToken& entity, const MutationContext& context) override {
        validate_api_token(entity);
        const auto before = find_by_id(entity.id);
        write_update(entity, context, before);
      }

      void soft_delete(const core::ApiTokenId& entity_id, const MutationContext& context) override {
        auto entity = find_by_id(entity_id);
        if (!entity.has_value()) {
          throw NotFound("api_token not found");
        }
        const auto before = entity;
        entity->revoked_at = now_timestamp();
        write_update(entity.value(), context, before);
      }

    private:
      void write_update(const core::ApiToken& entity, const MutationContext& context,
                        const std::optional<core::ApiToken>& before) {
        try {
          const auto result = txn_.work().exec(
              "UPDATE api_tokens SET user_id = $2, lab_id = $3, name = $4, scope_json = $5, "
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
                           entity.id.to_string(), context, "soft_delete",
                           detail::audit_change(before, entity));
      }

      PostgresTransaction& txn_;
    };

  } // namespace

  core::MfaSecurityState PostgresTransaction::initialize_mfa_state(const core::UserId& user_id,
                                                                   const MutationContext& ctx) {
    return detail::initialize_mfa_state(*this, detail::MfaWriteCapability{}, user_id, ctx);
  }
  void PostgresTransaction::replace_totp_secret(const core::UserId& user_id,
                                                const std::optional<std::string>& secret,
                                                const MutationContext& ctx) {
    detail::replace_totp_secret(*this, detail::MfaWriteCapability{}, user_id, secret, ctx,
                                now_timestamp());
  }
  void PostgresTransaction::complete_totp_session(const core::SessionId& session_id,
                                                  const core::Uuid& generation,
                                                  core::Timestamp verified_at,
                                                  const MutationContext& ctx) {
    detail::complete_totp_session(*this, detail::MfaWriteCapability{}, session_id, generation,
                                  verified_at, ctx);
  }

  void register_session_repositories(PostgresBackend& backend) {
    backend.register_repository_factory<core::MfaSecurityState>(
        [](PostgresTransaction& txn) { return std::make_unique<MfaStateRepository>(txn); });
    backend.register_repository_factory<core::Session>(
        [](PostgresTransaction& txn) { return std::make_unique<SessionRepository>(txn); });
    backend.register_repository_factory<core::ApiToken>(
        [](PostgresTransaction& txn) { return std::make_unique<ApiTokenRepository>(txn); });
  }

} // namespace fmgr::storage
