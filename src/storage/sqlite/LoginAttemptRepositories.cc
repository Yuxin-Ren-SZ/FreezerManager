// SPDX-License-Identifier: AGPL-3.0-or-later

#include "storage/sqlite/LoginAttemptRepositories.h"

#include "core/login_attempt.h"
#include "storage/LoginAttemptTraits.h"
#include "storage/detail/LoginAttemptColumns.h"
#include "storage/detail/QuerySqlBuilder.h"

#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
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
          throw_sqlite_error(result, handle_, "prepare sqlite login_attempt statement");
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
        throw_sqlite_error(result, handle_, "step sqlite login_attempt statement");
      }

      void step_done() const {
        const auto result = sqlite3_step(statement_);
        if (result != SQLITE_DONE) {
          throw_sqlite_error(result, handle_, "execute sqlite login_attempt statement");
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

    [[nodiscard]] std::string column_text(sqlite3_stmt* statement, int column) {
      const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(statement, column));
      return text == nullptr ? std::string() : std::string(text);
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

    using detail::login_attempt_column_name;
    using detail::validate_login_attempt;

    // ---- Column mapping ----

    constexpr std::string_view k_columns =
        "id, email, failure_count, locked_until_micros, last_activity_micros, cleared_at_micros";

    [[nodiscard]] core::LoginAttempt read_login_attempt(sqlite3_stmt* statement) {
      return core::LoginAttempt{
          .id = core::LoginAttemptId::parse(column_text(statement, 0)),
          .email = column_text(statement, 1),
          .failure_count = sqlite3_column_int64(statement, 2),
          .locked_until = column_optional_timestamp(statement, 3),
          .last_activity = core::Timestamp::from_unix_micros(sqlite3_column_int64(statement, 4)),
          .cleared_at = column_optional_timestamp(statement, 5),
      };
    }

    // ---- Repository ----
    //
    // Modeled on SessionRepository: mutations are staged and flushed at commit via
    // a transaction commit-hook, so the audit chain sees them atomically with the
    // rest of the transaction.

    class LoginAttemptRepository final : public IRepository<core::LoginAttempt> {
    public:
      explicit LoginAttemptRepository(SqliteTransaction& transaction) : transaction_(transaction) {
        transaction_.add_commit_hook([this](sqlite3* handle) { flush(handle); });
      }

      [[nodiscard]] std::optional<core::LoginAttempt>
      find_by_id(const core::LoginAttemptId& entity_id) override {
        if (const auto iter = pending_.find(entity_id); iter != pending_.end()) {
          return iter->second.entity;
        }
        return load(entity_id);
      }

      [[nodiscard]] std::vector<core::LoginAttempt>
      query(const Query<core::LoginAttempt>& query_spec) override {
        std::string sql = "SELECT ";
        sql += k_columns;
        sql += " FROM login_attempts";
        std::vector<nlohmann::json> parameters;
        const auto defaults = query_spec.includes_tombstoned()
                                  ? std::vector<std::string>{}
                                  : std::vector<std::string>{"cleared_at_micros IS NULL"};
        detail::SqliteDialect dialect;
        detail::append_where(sql, parameters, defaults, query_spec.predicates(),
                             detail::login_attempt_column_name, dialect);
        detail::append_order_limit(sql, parameters, query_spec, detail::login_attempt_column_name,
                                   dialect);

        Statement statement(transaction_.handle(), sql);
        bind_parameters(statement.get(), parameters);

        std::vector<core::LoginAttempt> results;
        while (statement.step_row()) {
          results.push_back(read_login_attempt(statement.get()));
        }
        for (const auto& [entity_id, pending_entity] : pending_) {
          if (!pending_entity.is_insert) {
            continue;
          }
          if (!query_spec.includes_tombstoned() && pending_entity.entity.cleared_at.has_value()) {
            continue;
          }
          const auto already_in_results =
              std::ranges::any_of(results, [&](const core::LoginAttempt& row) {
                return row.id == pending_entity.entity.id;
              });
          if (!already_in_results) {
            results.push_back(pending_entity.entity);
          }
        }
        return results;
      }

      void insert(const core::LoginAttempt& entity, const MutationContext& context) override {
        if (pending_.contains(entity.id) || load(entity.id).has_value()) {
          throw UniqueViolation("login_attempt id already exists");
        }
        validate_login_attempt(entity);
        pending_.insert_or_assign(entity.id, PendingEntity{.entity = entity, .is_insert = true});
        transaction_.note_mutation(std::string(EntityTraits<core::LoginAttempt>::entity_name()),
                                   entity.id.to_string(), context, "insert",
                                   AuditSnapshot{.after = nlohmann::json(entity)});
      }

      void update(const core::LoginAttempt& entity, const MutationContext& context) override {
        if (!pending_.contains(entity.id) && !load(entity.id).has_value()) {
          throw NotFound("login_attempt not found");
        }
        validate_login_attempt(entity);
        auto before = prior_snapshot(entity.id);
        const auto is_insert = pending_.contains(entity.id) && pending_.at(entity.id).is_insert;
        pending_.insert_or_assign(entity.id,
                                  PendingEntity{.entity = entity, .is_insert = is_insert});
        transaction_.note_mutation(
            std::string(EntityTraits<core::LoginAttempt>::entity_name()), entity.id.to_string(),
            context, "update",
            AuditSnapshot{.before = std::move(before), .after = nlohmann::json(entity)});
      }

      void soft_delete(const core::LoginAttemptId& entity_id,
                       const MutationContext& context) override {
        auto entity = find_by_id(entity_id);
        if (!entity.has_value()) {
          throw NotFound("login_attempt not found");
        }
        entity->cleared_at = now_timestamp();
        auto before = prior_snapshot(entity_id);
        const auto is_insert = pending_.contains(entity_id) && pending_.at(entity_id).is_insert;
        pending_.insert_or_assign(entity_id,
                                  PendingEntity{.entity = entity.value(), .is_insert = is_insert});
        transaction_.note_mutation(
            std::string(EntityTraits<core::LoginAttempt>::entity_name()), entity_id.to_string(),
            context, "soft_delete",
            AuditSnapshot{.before = std::move(before), .after = nlohmann::json(entity.value())});
      }

    private:
      struct PendingEntity {
        core::LoginAttempt entity;
        bool is_insert{false};
      };

      [[nodiscard]] std::optional<nlohmann::json>
      prior_snapshot(const core::LoginAttemptId& entity_id) const {
        if (const auto iter = pending_.find(entity_id); iter != pending_.end()) {
          return nlohmann::json(iter->second.entity);
        }
        if (const auto loaded = load(entity_id); loaded.has_value()) {
          return nlohmann::json(loaded.value());
        }
        return std::nullopt;
      }

      void flush(sqlite3* handle) {
        for (const auto& [unused_id, pending_entity] : pending_) {
          (void)unused_id;
          if (pending_entity.is_insert) {
            insert_pending(handle, pending_entity.entity);
          } else {
            update_pending(handle, pending_entity.entity);
          }
        }
      }

      [[nodiscard]] std::optional<core::LoginAttempt>
      load(const core::LoginAttemptId& entity_id) const {
        std::string sql = "SELECT ";
        sql += k_columns;
        sql += " FROM login_attempts WHERE id = ?";
        Statement statement(transaction_.handle(), sql);
        bind_text(statement.get(), 1, entity_id.to_string());
        if (!statement.step_row()) {
          return std::nullopt;
        }
        return read_login_attempt(statement.get());
      }

      static void bind_entity(sqlite3_stmt* statement, const core::LoginAttempt& entity) {
        bind_text(statement, 1, entity.id.to_string());
        bind_text(statement, 2, entity.email);
        bind_int64(statement, 3, entity.failure_count);
        bind_optional_timestamp(statement, 4, entity.locked_until);
        bind_int64(statement, 5, entity.last_activity.unix_micros());
        bind_optional_timestamp(statement, 6, entity.cleared_at);
      }

      static void insert_pending(sqlite3* handle, const core::LoginAttempt& entity) {
        Statement statement(handle, "INSERT INTO login_attempts "
                                    "(id, email, failure_count, locked_until_micros, "
                                    "last_activity_micros, cleared_at_micros) "
                                    "VALUES (?, ?, ?, ?, ?, ?)");
        bind_entity(statement.get(), entity);
        statement.step_done();
      }

      static void update_pending(sqlite3* handle, const core::LoginAttempt& entity) {
        Statement statement(handle,
                            "UPDATE login_attempts SET "
                            "id = ?, email = ?, failure_count = ?, locked_until_micros = ?, "
                            "last_activity_micros = ?, cleared_at_micros = ? WHERE id = ?");
        bind_entity(statement.get(), entity);
        bind_text(statement.get(), 7, entity.id.to_string());
        statement.step_done();
      }

      SqliteTransaction& transaction_;
      std::map<core::LoginAttemptId, PendingEntity> pending_;
    };

  } // namespace

  void register_login_attempt_repositories(SqliteBackend& backend) {
    backend.register_repository_factory<core::LoginAttempt>([](SqliteTransaction& transaction) {
      return std::make_unique<LoginAttemptRepository>(transaction);
    });
  }

} // namespace fmgr::storage
