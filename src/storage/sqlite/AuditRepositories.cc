// SPDX-License-Identifier: AGPL-3.0-or-later

#include "storage/sqlite/AuditRepositories.h"

#include "core/audit_event.h"
#include "storage/AuditTraits.h"
#include "storage/detail/AuditColumns.h"

#include <sqlite3.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace fmgr::storage {
  namespace {

    [[nodiscard]] std::string sqlite_error_msg(sqlite3* handle, std::string_view action) {
      return std::string(action) + ": " + sqlite3_errmsg(handle);
    }

    [[noreturn]] void throw_sqlite_error(int code, sqlite3* handle, std::string_view action) {
      const auto ext = sqlite3_extended_errcode(handle);
      const auto eff = (ext == SQLITE_OK) ? code : ext;
      const auto msg = sqlite_error_msg(handle, action);
      switch (eff) {
      case SQLITE_CONSTRAINT_UNIQUE:
      case SQLITE_CONSTRAINT_PRIMARYKEY:
        throw UniqueViolation(msg);
      case SQLITE_CONSTRAINT_FOREIGNKEY:
        throw ForeignKeyViolation(msg);
      case SQLITE_BUSY:
      case SQLITE_LOCKED:
        throw Unavailable(msg);
      default:
        throw ConstraintViolation(msg);
      }
    }

    class Statement {
    public:
      Statement(sqlite3* handle, const std::string& sql) : handle_(handle) {
        const auto result = sqlite3_prepare_v2(handle_, sql.c_str(), -1, &stmt_, nullptr);
        if (result != SQLITE_OK) {
          throw_sqlite_error(result, handle_, "prepare audit statement");
        }
      }
      ~Statement() {
        sqlite3_finalize(stmt_);
      }

      Statement(const Statement&) = delete;
      Statement& operator=(const Statement&) = delete;

      [[nodiscard]] sqlite3_stmt* get() const {
        return stmt_;
      }

      [[nodiscard]] bool step_row() const {
        const auto result = sqlite3_step(stmt_);
        if (result == SQLITE_ROW) {
          return true;
        }
        if (result == SQLITE_DONE) {
          return false;
        }
        throw_sqlite_error(result, handle_, "step audit statement");
      }

    private:
      sqlite3* handle_;
      sqlite3_stmt* stmt_{nullptr};
    };

    void bind_text(sqlite3_stmt* stmt, int idx, const std::string& value) {
      sqlite3_bind_text(stmt, idx, value.c_str(), static_cast<int>(value.size()), SQLITE_TRANSIENT);
    }

    [[nodiscard]] std::string col_text(sqlite3_stmt* stmt, int col) {
      const auto* text_ptr = reinterpret_cast<const char*>(sqlite3_column_text(stmt, col));
      return text_ptr == nullptr ? std::string{} : std::string{text_ptr};
    }

    [[nodiscard]] std::optional<std::string> col_opt_text(sqlite3_stmt* stmt, int col) {
      if (sqlite3_column_type(stmt, col) == SQLITE_NULL) {
        return std::nullopt;
      }
      return col_text(stmt, col);
    }

    constexpr std::string_view k_audit_cols =
        "id, at_micros, actor_user_id, actor_session_id, lab_id, "
        "action, entity_kind, entity_id, before_json, after_json, "
        "request_id, prev_hash, this_hash";

    [[nodiscard]] core::AuditEvent read_event(sqlite3_stmt* stmt) {
      return core::AuditEvent{
          .id = core::AuditEventId::parse(col_text(stmt, 0)),
          .at = core::Timestamp::from_unix_micros(sqlite3_column_int64(stmt, 1)),
          .actor_user_id = core::UserId::parse(col_text(stmt, 2)),
          .actor_session_id = col_text(stmt, 3),
          .lab_id = [&]() -> std::optional<core::LabId> {
            const auto text_opt = col_opt_text(stmt, 4);
            return text_opt.has_value() ? std::make_optional(core::LabId::parse(*text_opt))
                                        : std::nullopt;
          }(),
          .action = col_text(stmt, 5),
          .entity_kind = col_text(stmt, 6),
          .entity_id = col_opt_text(stmt, 7),
          .before_json = col_text(stmt, 8),
          .after_json = col_text(stmt, 9),
          .request_id = col_text(stmt, 10),
          .prev_hash = col_text(stmt, 11),
          .this_hash = col_text(stmt, 12),
      };
    }

    // ---- AuditEventRepository ----

    class AuditEventRepository final : public IRepository<core::AuditEvent> {
    public:
      explicit AuditEventRepository(SqliteTransaction& txn) : txn_(txn) {}

      [[nodiscard]] std::optional<core::AuditEvent>
      find_by_id(const core::AuditEventId& entity_id) override {
        std::string sql = "SELECT ";
        sql += k_audit_cols;
        sql += " FROM audit_events WHERE id = ?";
        Statement stmt(txn_.handle(), sql);
        bind_text(stmt.get(), 1, entity_id.to_string());
        if (!stmt.step_row()) {
          return std::nullopt;
        }
        return read_event(stmt.get());
      }

      [[nodiscard]] std::vector<core::AuditEvent>
      query(const Query<core::AuditEvent>& query_spec) override {
        std::string sql = "SELECT ";
        sql += k_audit_cols;
        sql += " FROM audit_events";
        // Audit events are never soft-deleted; include_tombstoned() is a no-op.
        // No default WHERE clause: all rows are always visible.
        if (!query_spec.predicates().empty()) {
          sql += " WHERE ";
          for (std::size_t i = 0; i < query_spec.predicates().size(); ++i) {
            if (i != 0) {
              sql += " AND ";
            }
            const auto& pred = query_spec.predicates().at(i);
            sql += detail::audit_event_column_name(pred.field) + " = ?";
          }
        }
        // Sort / limit / offset
        if (!query_spec.sorts().empty()) {
          sql += " ORDER BY ";
          for (std::size_t i = 0; i < query_spec.sorts().size(); ++i) {
            if (i != 0) {
              sql += ", ";
            }
            const auto& sort_spec = query_spec.sorts().at(i);
            sql += detail::audit_event_column_name(sort_spec.field);
            sql += (sort_spec.direction == SortDirection::Ascending) ? " ASC" : " DESC";
          }
        }
        const auto limit = query_spec.limit_count();
        const auto offset = query_spec.offset_count();
        if (limit.has_value()) {
          sql += " LIMIT " + std::to_string(*limit);
        }
        if (offset.has_value()) {
          if (!limit.has_value()) {
            sql += " LIMIT -1";
          }
          sql += " OFFSET " + std::to_string(*offset);
        }

        Statement stmt(txn_.handle(), sql);
        // Bind predicate values (equality only for now).
        int idx = 1;
        for (const auto& pred : query_spec.predicates()) {
          if (pred.value.is_string()) {
            bind_text(stmt.get(), idx, pred.value.get<std::string>());
          } else {
            sqlite3_bind_int64(stmt.get(), idx, pred.value.get<std::int64_t>());
          }
          ++idx;
        }

        std::vector<core::AuditEvent> results;
        while (stmt.step_row()) {
          results.push_back(read_event(stmt.get()));
        }
        return results;
      }

      void insert(const core::AuditEvent& /*entity*/, const MutationContext& /*context*/) override {
        throw UnsupportedOperation("AuditEvent rows are written by SqliteTransaction::commit(); "
                                   "do not insert them manually");
      }

      void update(const core::AuditEvent& /*entity*/, const MutationContext& /*context*/) override {
        throw UnsupportedOperation("audit_events is append-only");
      }

      void soft_delete(const core::AuditEventId& /*entity_id*/,
                       const MutationContext& /*context*/) override {
        throw UnsupportedOperation("audit_events is append-only");
      }

    private:
      SqliteTransaction& txn_;
    };

  } // namespace

  void register_audit_repositories(SqliteBackend& backend) {
    backend.register_repository_factory<core::AuditEvent>([](SqliteTransaction& transaction) {
      return std::make_unique<AuditEventRepository>(transaction);
    });
  }

} // namespace fmgr::storage
