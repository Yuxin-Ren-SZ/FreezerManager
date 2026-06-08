// SPDX-License-Identifier: AGPL-3.0-or-later

#include "storage/sqlite/SessionRepositories.h"

#include "core/session.h"
#include "storage/SessionTraits.h"
#include "storage/detail/QuerySqlBuilder.h"
#include "storage/detail/SessionColumns.h"

#include <sqlite3.h>

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fmgr::storage {
  namespace {

    // ---- SQLite helpers (local to this translation unit) ----

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
      case SQLITE_CONSTRAINT_FOREIGNKEY:
        throw ForeignKeyViolation(sqlite_error(handle, action));
      case SQLITE_BUSY:
      case SQLITE_LOCKED:
        throw Unavailable(sqlite_error(handle, action));
      case SQLITE_CONSTRAINT:
      case SQLITE_CONSTRAINT_CHECK:
      case SQLITE_CONSTRAINT_NOTNULL:
        throw ConstraintViolation(sqlite_error(handle, action));
      default:
        throw BackendError(BackendErrorCode::ConstraintViolation, sqlite_error(handle, action));
      }
    }

    class Statement {
    public:
      Statement(sqlite3* handle, const std::string& sql) : handle_(handle) {
        const auto result = sqlite3_prepare_v2(handle_, sql.c_str(), -1, &statement_, nullptr);
        if (result != SQLITE_OK) {
          throw_sqlite_error(result, handle_, "prepare sqlite session statement");
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
        throw_sqlite_error(result, handle_, "step sqlite session statement");
      }

      void step_done() const {
        const auto result = sqlite3_step(statement_);
        if (result != SQLITE_DONE) {
          throw_sqlite_error(result, handle_, "execute sqlite session statement");
        }
      }

    private:
      sqlite3* handle_;
      sqlite3_stmt* statement_{nullptr};
    };

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

    void bind_optional_timestamp(sqlite3_stmt* statement, int index,
                                 const std::optional<core::Timestamp>& timestamp) {
      if (timestamp.has_value()) {
        bind_int64(statement, index, timestamp->unix_micros());
      } else {
        bind_null(statement, index);
      }
    }

    void bind_optional_text(sqlite3_stmt* statement, int index,
                            const std::optional<std::string>& value) {
      if (value.has_value()) {
        bind_text(statement, index, value.value());
      } else {
        bind_null(statement, index);
      }
    }

    [[nodiscard]] std::string column_text(sqlite3_stmt* statement, int column) {
      const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(statement, column));
      return text == nullptr ? std::string() : std::string(text);
    }

    [[nodiscard]] std::optional<std::string> column_optional_text(sqlite3_stmt* statement,
                                                                  int column) {
      if (sqlite3_column_type(statement, column) == SQLITE_NULL) {
        return std::nullopt;
      }
      return column_text(statement, column);
    }

    [[nodiscard]] std::optional<core::Timestamp> column_optional_timestamp(sqlite3_stmt* statement,
                                                                           int column) {
      if (sqlite3_column_type(statement, column) == SQLITE_NULL) {
        return std::nullopt;
      }
      return core::Timestamp::from_unix_micros(sqlite3_column_int64(statement, column));
    }

    [[nodiscard]] core::Timestamp now_timestamp() {
      const auto now =
          std::chrono::time_point_cast<std::chrono::microseconds>(std::chrono::system_clock::now());
      return core::Timestamp::from_unix_micros(now.time_since_epoch().count());
    }

    void bind_json_parameter(sqlite3_stmt* statement, int index, const nlohmann::json& value) {
      if (value.is_null()) {
        bind_null(statement, index);
        return;
      }
      if (value.is_number_integer()) {
        bind_int64(statement, index, value.get<std::int64_t>());
        return;
      }
      if (value.is_string()) {
        bind_text(statement, index, value.get<std::string>());
        return;
      }
      bind_text(statement, index, value.dump());
    }

    void bind_parameters(sqlite3_stmt* statement, const std::vector<nlohmann::json>& parameters) {
      int index = 1;
      for (const auto& parameter : parameters) {
        bind_json_parameter(statement, index, parameter);
        ++index;
      }
    }

    using detail::api_token_column_name;
    using detail::session_column_name;
    using detail::validate_api_token;
    using detail::validate_session;

    // ---- Base repository template ----

    template <typename Entity> class SqliteSessionRepoBase : public IRepository<Entity> {
    public:
      explicit SqliteSessionRepoBase(SqliteTransaction& transaction) : transaction_(transaction) {
        transaction_.add_commit_hook([this](sqlite3* handle) { flush(handle); });
      }

    protected:
      struct PendingEntity {
        Entity entity;
        bool is_insert{false};
      };

      [[nodiscard]] SqliteTransaction& transaction() {
        return transaction_;
      }
      [[nodiscard]] const SqliteTransaction& transaction() const {
        return transaction_;
      }

      [[nodiscard]] const std::map<typename EntityTraits<Entity>::Id, PendingEntity>&
      pending() const {
        return pending_;
      }
      [[nodiscard]] std::map<typename EntityTraits<Entity>::Id, PendingEntity>& pending() {
        return pending_;
      }

      // Snapshot of the latest persisted/staged state of `entity_id`, or nullopt
      // if it does not exist yet. Used as the audit "before" image on updates.
      [[nodiscard]] std::optional<nlohmann::json>
      prior_snapshot(const typename EntityTraits<Entity>::Id& entity_id) const {
        if (const auto staged = find_staged(entity_id); staged.has_value()) {
          return nlohmann::json(staged.value());
        }
        if (const auto loaded = load(entity_id); loaded.has_value()) {
          return nlohmann::json(loaded.value());
        }
        return std::nullopt;
      }

      void stage_insert(const Entity& entity, const MutationContext& context) {
        const auto entity_id = id_of(entity);
        if (pending_.contains(entity_id) || load(entity_id).has_value()) {
          throw UniqueViolation(std::string(EntityTraits<Entity>::entity_name()) +
                                " id already exists");
        }
        validate(entity);
        pending_.insert_or_assign(entity_id, PendingEntity{.entity = entity, .is_insert = true});
        transaction_.note_mutation(std::string(EntityTraits<Entity>::entity_name()),
                                   entity_id.to_string(), context, "insert",
                                   AuditSnapshot{.after = nlohmann::json(entity)});
      }

      void stage_update(const Entity& entity, const MutationContext& context) {
        const auto entity_id = id_of(entity);
        if (!pending_.contains(entity_id) && !load(entity_id).has_value()) {
          throw NotFound(std::string(EntityTraits<Entity>::entity_name()) + " not found");
        }
        validate(entity);
        auto before = prior_snapshot(entity_id);
        const auto is_insert = pending_.contains(entity_id) && pending_.at(entity_id).is_insert;
        pending_.insert_or_assign(entity_id,
                                  PendingEntity{.entity = entity, .is_insert = is_insert});
        transaction_.note_mutation(
            std::string(EntityTraits<Entity>::entity_name()), entity_id.to_string(), context,
            "update", AuditSnapshot{.before = std::move(before), .after = nlohmann::json(entity)});
      }

      [[nodiscard]] std::optional<Entity>
      find_staged(const typename EntityTraits<Entity>::Id& entity_id) const {
        const auto iter = pending_.find(entity_id);
        if (iter == pending_.end()) {
          return std::nullopt;
        }
        return iter->second.entity;
      }

    private:
      // NOLINTBEGIN(portability-template-virtual-member-function)
      virtual void flush(sqlite3* handle) = 0;
      [[nodiscard]] virtual std::optional<Entity>
      load(const typename EntityTraits<Entity>::Id& entity_id) const = 0;
      virtual void validate(const Entity& entity) const = 0;
      [[nodiscard]] virtual typename EntityTraits<Entity>::Id id_of(const Entity& entity) const = 0;
      // NOLINTEND(portability-template-virtual-member-function)

      SqliteTransaction& transaction_;
      std::map<typename EntityTraits<Entity>::Id, PendingEntity> pending_;
    };

    // ---- Session column mapping ----

    constexpr std::string_view k_session_columns =
        "id, user_id, token_hash, token_prefix, "
        "created_at_micros, last_seen_at_micros, ip, user_agent, revoked_at_micros, mfa_complete";

    [[nodiscard]] core::Session read_session(sqlite3_stmt* statement) {
      return core::Session{
          .id = core::SessionId::parse(column_text(statement, 0)),
          .user_id = core::UserId::parse(column_text(statement, 1)),
          .token_hash = column_text(statement, 2),
          .token_prefix = column_text(statement, 3),
          .created_at = core::Timestamp::from_unix_micros(sqlite3_column_int64(statement, 4)),
          .last_seen_at = core::Timestamp::from_unix_micros(sqlite3_column_int64(statement, 5)),
          .ip = column_optional_text(statement, 6),
          .user_agent = column_optional_text(statement, 7),
          .revoked_at = column_optional_timestamp(statement, 8),
          .mfa_complete = sqlite3_column_int(statement, 9) != 0,
      };
    }

    // ---- SessionRepository ----

    class SessionRepository final : public SqliteSessionRepoBase<core::Session> {
    public:
      using SqliteSessionRepoBase::SqliteSessionRepoBase;

      [[nodiscard]] std::optional<core::Session>
      find_by_id(const core::SessionId& entity_id) override {
        if (auto staged = find_staged(entity_id); staged.has_value()) {
          return staged;
        }
        return load(entity_id);
      }

      [[nodiscard]] std::vector<core::Session>
      query(const Query<core::Session>& query_spec) override {
        std::string sql = "SELECT ";
        sql += k_session_columns;
        sql += " FROM sessions";
        std::vector<nlohmann::json> parameters;
        // Default: active sessions only.  include_tombstoned() shows revoked rows too.
        const auto defaults = query_spec.includes_tombstoned()
                                  ? std::vector<std::string>{}
                                  : std::vector<std::string>{"revoked_at_micros IS NULL"};
        detail::SqliteDialect dialect;
        detail::append_where(sql, parameters, defaults, query_spec.predicates(),
                             detail::session_column_name, dialect);
        detail::append_order_limit(sql, parameters, query_spec, detail::session_column_name,
                                   dialect);

        Statement statement(transaction().handle(), sql);
        bind_parameters(statement.get(), parameters);

        std::vector<core::Session> results;
        while (statement.step_row()) {
          results.push_back(read_session(statement.get()));
        }
        // Append in-flight staged inserts not yet flushed.
        for (const auto& [entity_id, pending_entity] : pending()) {
          if (!pending_entity.is_insert) {
            continue;
          }
          if (!query_spec.includes_tombstoned() && pending_entity.entity.revoked_at.has_value()) {
            continue;
          }
          const auto already_in_results =
              std::ranges::any_of(results, [&](const core::Session& sess) {
                return sess.id == pending_entity.entity.id;
              });
          if (!already_in_results) {
            results.push_back(pending_entity.entity);
          }
        }
        return results;
      }

      void insert(const core::Session& entity, const MutationContext& context) override {
        stage_insert(entity, context);
      }

      void update(const core::Session& entity, const MutationContext& context) override {
        stage_update(entity, context);
      }

      void soft_delete(const core::SessionId& entity_id, const MutationContext& context) override {
        auto entity = find_by_id(entity_id);
        if (!entity.has_value()) {
          throw NotFound("session not found");
        }
        entity->revoked_at = now_timestamp();
        auto before = prior_snapshot(entity_id);
        const auto is_insert = pending().contains(entity_id) && pending().at(entity_id).is_insert;
        pending().insert_or_assign(entity_id,
                                   PendingEntity{.entity = entity.value(), .is_insert = is_insert});
        transaction().note_mutation(
            std::string(EntityTraits<core::Session>::entity_name()), entity_id.to_string(), context,
            "soft_delete",
            AuditSnapshot{.before = std::move(before), .after = nlohmann::json(entity.value())});
      }

    private:
      void flush(sqlite3* handle) override {
        for (const auto& [unused_id, pending_entity] : pending()) {
          (void)unused_id;
          if (pending_entity.is_insert) {
            insert_pending(handle, pending_entity.entity);
          } else {
            update_pending(handle, pending_entity.entity);
          }
        }
      }

      [[nodiscard]] std::optional<core::Session>
      load(const core::SessionId& entity_id) const override {
        std::string sql = "SELECT ";
        sql += k_session_columns;
        sql += " FROM sessions WHERE id = ?";
        Statement statement(transaction().handle(), sql);
        bind_text(statement.get(), 1, entity_id.to_string());
        if (!statement.step_row()) {
          return std::nullopt;
        }
        return read_session(statement.get());
      }

      void validate(const core::Session& entity) const override {
        validate_session(entity);
      }

      [[nodiscard]] core::SessionId id_of(const core::Session& entity) const override {
        return entity.id;
      }

      static void bind_entity(sqlite3_stmt* statement, const core::Session& entity) {
        bind_text(statement, 1, entity.id.to_string());
        bind_text(statement, 2, entity.user_id.to_string());
        bind_text(statement, 3, entity.token_hash);
        bind_text(statement, 4, entity.token_prefix);
        bind_int64(statement, 5, entity.created_at.unix_micros());
        bind_int64(statement, 6, entity.last_seen_at.unix_micros());
        bind_optional_text(statement, 7, entity.ip);
        bind_optional_text(statement, 8, entity.user_agent);
        bind_optional_timestamp(statement, 9, entity.revoked_at);
        bind_int64(statement, 10, entity.mfa_complete ? 1 : 0);
      }

      static void insert_pending(sqlite3* handle, const core::Session& entity) {
        Statement statement(handle, "INSERT INTO sessions "
                                    "(id, user_id, token_hash, token_prefix, "
                                    "created_at_micros, last_seen_at_micros, "
                                    "ip, user_agent, revoked_at_micros, mfa_complete) "
                                    "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
        bind_entity(statement.get(), entity);
        statement.step_done();
      }

      static void update_pending(sqlite3* handle, const core::Session& entity) {
        Statement statement(handle, "UPDATE sessions SET "
                                    "id = ?, user_id = ?, token_hash = ?, token_prefix = ?, "
                                    "created_at_micros = ?, last_seen_at_micros = ?, "
                                    "ip = ?, user_agent = ?, revoked_at_micros = ?, "
                                    "mfa_complete = ? WHERE id = ?");
        bind_entity(statement.get(), entity);
        bind_text(statement.get(), 11, entity.id.to_string());
        statement.step_done();
      }
    };

    // ---- ApiToken column mapping ----

    constexpr std::string_view k_api_token_columns =
        "id, user_id, lab_id, name, scope_json, token_hash, token_prefix, "
        "created_at_micros, expires_at_micros, revoked_at_micros";

    [[nodiscard]] core::ApiToken read_api_token(sqlite3_stmt* statement) {
      return core::ApiToken{
          .id = core::ApiTokenId::parse(column_text(statement, 0)),
          .user_id = core::UserId::parse(column_text(statement, 1)),
          .lab_id = [&]() -> std::optional<core::LabId> {
            const auto text = column_optional_text(statement, 2);
            return text.has_value() ? std::make_optional(core::LabId::parse(*text)) : std::nullopt;
          }(),
          .name = column_text(statement, 3),
          .scope_json = column_text(statement, 4),
          .token_hash = column_text(statement, 5),
          .token_prefix = column_text(statement, 6),
          .created_at = core::Timestamp::from_unix_micros(sqlite3_column_int64(statement, 7)),
          .expires_at = column_optional_timestamp(statement, 8),
          .revoked_at = column_optional_timestamp(statement, 9),
      };
    }

    // ---- ApiTokenRepository ----

    class ApiTokenRepository final : public SqliteSessionRepoBase<core::ApiToken> {
    public:
      using SqliteSessionRepoBase::SqliteSessionRepoBase;

      [[nodiscard]] std::optional<core::ApiToken>
      find_by_id(const core::ApiTokenId& entity_id) override {
        if (auto staged = find_staged(entity_id); staged.has_value()) {
          return staged;
        }
        return load(entity_id);
      }

      [[nodiscard]] std::vector<core::ApiToken>
      query(const Query<core::ApiToken>& query_spec) override {
        std::string sql = "SELECT ";
        sql += k_api_token_columns;
        sql += " FROM api_tokens";
        std::vector<nlohmann::json> parameters;
        const auto defaults = query_spec.includes_tombstoned()
                                  ? std::vector<std::string>{}
                                  : std::vector<std::string>{"revoked_at_micros IS NULL"};
        detail::SqliteDialect dialect;
        detail::append_where(sql, parameters, defaults, query_spec.predicates(),
                             detail::api_token_column_name, dialect);
        detail::append_order_limit(sql, parameters, query_spec, detail::api_token_column_name,
                                   dialect);

        Statement statement(transaction().handle(), sql);
        bind_parameters(statement.get(), parameters);

        std::vector<core::ApiToken> results;
        while (statement.step_row()) {
          results.push_back(read_api_token(statement.get()));
        }
        for (const auto& [entity_id, pending_entity] : pending()) {
          if (!pending_entity.is_insert) {
            continue;
          }
          if (!query_spec.includes_tombstoned() && pending_entity.entity.revoked_at.has_value()) {
            continue;
          }
          const auto already_in_results =
              std::ranges::any_of(results, [&](const core::ApiToken& tok) {
                return tok.id == pending_entity.entity.id;
              });
          if (!already_in_results) {
            results.push_back(pending_entity.entity);
          }
        }
        return results;
      }

      void insert(const core::ApiToken& entity, const MutationContext& context) override {
        stage_insert(entity, context);
      }

      void update(const core::ApiToken& entity, const MutationContext& context) override {
        stage_update(entity, context);
      }

      void soft_delete(const core::ApiTokenId& entity_id, const MutationContext& context) override {
        auto entity = find_by_id(entity_id);
        if (!entity.has_value()) {
          throw NotFound("api_token not found");
        }
        entity->revoked_at = now_timestamp();
        auto before = prior_snapshot(entity_id);
        const auto is_insert = pending().contains(entity_id) && pending().at(entity_id).is_insert;
        pending().insert_or_assign(entity_id,
                                   PendingEntity{.entity = entity.value(), .is_insert = is_insert});
        transaction().note_mutation(
            std::string(EntityTraits<core::ApiToken>::entity_name()), entity_id.to_string(),
            context, "soft_delete",
            AuditSnapshot{.before = std::move(before), .after = nlohmann::json(entity.value())});
      }

    private:
      void flush(sqlite3* handle) override {
        for (const auto& [unused_id, pending_entity] : pending()) {
          (void)unused_id;
          if (pending_entity.is_insert) {
            insert_pending(handle, pending_entity.entity);
          } else {
            update_pending(handle, pending_entity.entity);
          }
        }
      }

      [[nodiscard]] std::optional<core::ApiToken>
      load(const core::ApiTokenId& entity_id) const override {
        std::string sql = "SELECT ";
        sql += k_api_token_columns;
        sql += " FROM api_tokens WHERE id = ?";
        Statement statement(transaction().handle(), sql);
        bind_text(statement.get(), 1, entity_id.to_string());
        if (!statement.step_row()) {
          return std::nullopt;
        }
        return read_api_token(statement.get());
      }

      void validate(const core::ApiToken& entity) const override {
        validate_api_token(entity);
      }

      [[nodiscard]] core::ApiTokenId id_of(const core::ApiToken& entity) const override {
        return entity.id;
      }

      static void bind_entity(sqlite3_stmt* statement, const core::ApiToken& entity) {
        bind_text(statement, 1, entity.id.to_string());
        bind_text(statement, 2, entity.user_id.to_string());
        if (entity.lab_id.has_value()) {
          bind_text(statement, 3, entity.lab_id->to_string());
        } else {
          bind_null(statement, 3);
        }
        bind_text(statement, 4, entity.name);
        bind_text(statement, 5, entity.scope_json);
        bind_text(statement, 6, entity.token_hash);
        bind_text(statement, 7, entity.token_prefix);
        bind_int64(statement, 8, entity.created_at.unix_micros());
        bind_optional_timestamp(statement, 9, entity.expires_at);
        bind_optional_timestamp(statement, 10, entity.revoked_at);
      }

      static void insert_pending(sqlite3* handle, const core::ApiToken& entity) {
        Statement statement(handle, "INSERT INTO api_tokens "
                                    "(id, user_id, lab_id, name, scope_json, "
                                    "token_hash, token_prefix, "
                                    "created_at_micros, expires_at_micros, revoked_at_micros) "
                                    "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
        bind_entity(statement.get(), entity);
        statement.step_done();
      }

      static void update_pending(sqlite3* handle, const core::ApiToken& entity) {
        Statement statement(handle,
                            "UPDATE api_tokens SET "
                            "id = ?, user_id = ?, lab_id = ?, name = ?, scope_json = ?, "
                            "token_hash = ?, token_prefix = ?, "
                            "created_at_micros = ?, expires_at_micros = ?, revoked_at_micros = ? "
                            "WHERE id = ?");
        bind_entity(statement.get(), entity);
        bind_text(statement.get(), 11, entity.id.to_string());
        statement.step_done();
      }
    };

  } // namespace

  void register_session_repositories(SqliteBackend& backend) {
    backend.register_repository_factory<core::Session>([](SqliteTransaction& transaction) {
      return std::make_unique<SessionRepository>(transaction);
    });
    backend.register_repository_factory<core::ApiToken>([](SqliteTransaction& transaction) {
      return std::make_unique<ApiTokenRepository>(transaction);
    });
  }

} // namespace fmgr::storage
