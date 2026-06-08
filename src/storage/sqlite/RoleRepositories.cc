// SPDX-License-Identifier: AGPL-3.0-or-later

#include "storage/sqlite/RoleRepositories.h"

#include "core/role.h"
#include "storage/RoleTraits.h"
#include "storage/detail/RoleColumns.h"

#include <sqlite3.h>

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fmgr::storage {
  namespace {

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
          throw_sqlite_error(result, handle_, "prepare sqlite role statement");
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
        throw_sqlite_error(result, handle_, "step sqlite role statement");
      }

      void step_done() const {
        const auto result = sqlite3_step(statement_);
        if (result != SQLITE_DONE) {
          throw_sqlite_error(result, handle_, "execute sqlite role statement");
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

    void bind_optional_text(sqlite3_stmt* statement, int index,
                            const std::optional<std::string>& value) {
      if (value.has_value()) {
        bind_text(statement, index, value.value());
      } else {
        bind_null(statement, index);
      }
    }

    void bind_optional_int64(sqlite3_stmt* statement, int index,
                             const std::optional<std::int64_t>& value) {
      if (value.has_value()) {
        bind_int64(statement, index, value.value());
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

    using detail::validate_role;

    [[nodiscard]] core::Role read_role(sqlite3_stmt* statement) {
      const auto lab_id_text = column_optional_text(statement, 1);
      return core::Role{
          .id = core::RoleId::parse(column_text(statement, 0)),
          .lab_id = lab_id_text.has_value()
                        ? std::optional<core::LabId>(core::LabId::parse(lab_id_text.value()))
                        : std::nullopt,
          .kind = core::parse_role_kind(column_text(statement, 2)),
          .name = column_text(statement, 3),
          .description = column_text(statement, 4),
          .is_builtin = sqlite3_column_int(statement, 5) != 0,
          .created_at = core::Timestamp::from_unix_micros(sqlite3_column_int64(statement, 6)),
          .archived_at = column_optional_timestamp(statement, 7),
      };
    }

    [[nodiscard]] core::RolePermission read_role_permission(sqlite3_stmt* statement) {
      return core::RolePermission{
          .role_id = core::RoleId::parse(column_text(statement, 0)),
          .permission = core::parse_permission(column_text(statement, 1)),
      };
    }

    template <typename Entity> class SqliteRoleRepositoryBase : public IRepository<Entity> {
    public:
      explicit SqliteRoleRepositoryBase(SqliteTransaction& transaction)
          : transaction_(transaction) {
        transaction_.add_commit_hook([this](sqlite3* handle) { flush(handle); });
      }

    protected:
      enum class PendingKind : std::uint8_t { Insert, Update, Delete };

      struct PendingEntity {
        Entity entity;
        PendingKind kind{PendingKind::Insert};
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

      void stage(typename EntityTraits<Entity>::Id entity_id, PendingEntity pending,
                 const MutationContext& context) {
        // Derive the audit snapshot/action from the pending op before moving it in.
        AuditSnapshot snapshot;
        std::string action;
        if (pending.kind == PendingKind::Insert) {
          action = "insert";
          snapshot.after = nlohmann::json(pending.entity);
        } else if (pending.kind == PendingKind::Delete) {
          action = "soft_delete";
          snapshot.before = nlohmann::json(pending.entity);
        } else {
          action = "update";
          snapshot.after = nlohmann::json(pending.entity);
        }
        pending_.insert_or_assign(std::move(entity_id), std::move(pending));
        transaction_.note_mutation(std::string(EntityTraits<Entity>::entity_name()),
                                   id_string(pending_.rbegin()->first), context, std::move(action),
                                   std::move(snapshot));
      }

      [[nodiscard]] std::optional<Entity>
      find_staged(const typename EntityTraits<Entity>::Id& entity_id) const {
        const auto iterator = pending_.find(entity_id);
        if (iterator == pending_.end() || iterator->second.kind == PendingKind::Delete) {
          return std::nullopt;
        }
        return iterator->second.entity;
      }

      [[nodiscard]] bool
      is_staged_deleted(const typename EntityTraits<Entity>::Id& entity_id) const {
        const auto iterator = pending_.find(entity_id);
        return iterator != pending_.end() && iterator->second.kind == PendingKind::Delete;
      }

    private:
      // NOLINTBEGIN(portability-template-virtual-member-function)
      virtual void flush(sqlite3* handle) = 0;
      [[nodiscard]] virtual std::string
      id_string(const typename EntityTraits<Entity>::Id& entity_id) const = 0;
      // NOLINTEND(portability-template-virtual-member-function)

      SqliteTransaction& transaction_;
      std::map<typename EntityTraits<Entity>::Id, PendingEntity> pending_;
    };

    class RoleRepository final : public SqliteRoleRepositoryBase<core::Role> {
    public:
      using SqliteRoleRepositoryBase::SqliteRoleRepositoryBase;

      [[nodiscard]] std::optional<core::Role> find_by_id(const core::RoleId& entity_id) override {
        if (auto staged = find_staged(entity_id); staged.has_value()) {
          return staged;
        }
        if (is_staged_deleted(entity_id)) {
          return std::nullopt;
        }
        return load(entity_id);
      }

      [[nodiscard]] std::vector<core::Role> query(const Query<core::Role>& query_spec) override {
        std::string sql =
            "SELECT id, lab_id, kind, name, description, is_builtin, created_at_micros, "
            "archived_at_micros FROM roles";
        if (!query_spec.includes_tombstoned()) {
          sql += " WHERE archived_at_micros IS NULL";
        }
        sql += " ORDER BY created_at_micros ASC, id ASC";
        if (const auto limit = query_spec.limit_count(); limit.has_value()) {
          sql += " LIMIT " + std::to_string(limit.value());
        }

        Statement statement(transaction().handle(), sql);
        std::vector<core::Role> results;
        while (statement.step_row()) {
          results.push_back(read_role(statement.get()));
        }
        return results;
      }

      void insert(const core::Role& entity, const MutationContext& context) override {
        validate_role(entity);
        if (find_by_id(entity.id).has_value()) {
          throw UniqueViolation("role id already exists");
        }
        stage(entity.id, PendingEntity{.entity = entity, .kind = PendingKind::Insert}, context);
      }

      void update(const core::Role& entity, const MutationContext& context) override {
        validate_role(entity);
        if (!find_by_id(entity.id).has_value()) {
          throw NotFound("role not found");
        }
        stage(entity.id, PendingEntity{.entity = entity, .kind = PendingKind::Update}, context);
      }

      void soft_delete(const core::RoleId& entity_id, const MutationContext& context) override {
        auto entity = find_by_id(entity_id);
        if (!entity.has_value()) {
          throw NotFound("role not found");
        }
        if (entity->is_builtin) {
          throw ConstraintViolation("built-in roles cannot be archived");
        }
        entity->archived_at = now_timestamp();
        stage(entity_id, PendingEntity{.entity = entity.value(), .kind = PendingKind::Update},
              context);
      }

    private:
      void flush(sqlite3* handle) override {
        for (const auto& [unused_id, pending_entity] : pending()) {
          (void)unused_id;
          if (pending_entity.kind == PendingKind::Insert) {
            insert_pending(handle, pending_entity.entity);
          } else if (pending_entity.kind == PendingKind::Update) {
            update_pending(handle, pending_entity.entity);
          }
        }
      }

      [[nodiscard]] std::string id_string(const core::RoleId& entity_id) const override {
        return entity_id.to_string();
      }

      [[nodiscard]] std::optional<core::Role> load(const core::RoleId& entity_id) const {
        Statement statement(
            transaction().handle(),
            "SELECT id, lab_id, kind, name, description, is_builtin, created_at_micros, "
            "archived_at_micros FROM roles WHERE id = ?");
        bind_text(statement.get(), 1, entity_id.to_string());
        if (!statement.step_row()) {
          return std::nullopt;
        }
        return read_role(statement.get());
      }

      static void bind_entity(sqlite3_stmt* statement, const core::Role& entity) {
        bind_text(statement, 1, entity.id.to_string());
        if (entity.lab_id.has_value()) {
          bind_text(statement, 2, entity.lab_id->to_string());
        } else {
          bind_null(statement, 2);
        }
        bind_text(statement, 3, std::string(core::to_string(entity.kind)));
        bind_text(statement, 4, entity.name);
        bind_text(statement, 5, entity.description);
        bind_int64(statement, 6, entity.is_builtin ? 1 : 0);
        bind_int64(statement, 7, entity.created_at.unix_micros());
        bind_optional_int64(statement, 8,
                            entity.archived_at.has_value()
                                ? std::optional<std::int64_t>(entity.archived_at->unix_micros())
                                : std::nullopt);
      }

      static void insert_pending(sqlite3* handle, const core::Role& entity) {
        Statement statement(handle,
                            "INSERT INTO roles (id, lab_id, kind, name, description, is_builtin, "
                            "created_at_micros, archived_at_micros) "
                            "VALUES (?, ?, ?, ?, ?, ?, ?, ?)");
        bind_entity(statement.get(), entity);
        statement.step_done();
      }

      static void update_pending(sqlite3* handle, const core::Role& entity) {
        Statement statement(handle, "UPDATE roles SET id = ?, lab_id = ?, kind = ?, name = ?, "
                                    "description = ?, is_builtin = ?, created_at_micros = ?, "
                                    "archived_at_micros = ? WHERE id = ?");
        bind_entity(statement.get(), entity);
        bind_text(statement.get(), 9, entity.id.to_string());
        statement.step_done();
      }
    };

    class RolePermissionRepository final : public SqliteRoleRepositoryBase<core::RolePermission> {
    public:
      using SqliteRoleRepositoryBase::SqliteRoleRepositoryBase;

      [[nodiscard]] std::optional<core::RolePermission>
      find_by_id(const core::RolePermissionId& entity_id) override {
        if (auto staged = find_staged(entity_id); staged.has_value()) {
          return staged;
        }
        if (is_staged_deleted(entity_id)) {
          return std::nullopt;
        }
        return load(entity_id);
      }

      [[nodiscard]] std::vector<core::RolePermission>
      query(const Query<core::RolePermission>& query_spec) override {
        std::string sql = "SELECT role_id, permission_key FROM role_permissions";
        std::vector<std::string> params;
        if (!query_spec.predicates().empty()) {
          sql += " WHERE ";
          for (std::size_t i = 0; i < query_spec.predicates().size(); ++i) {
            if (i != 0) {
              sql += " AND ";
            }
            const auto& predicate = query_spec.predicates().at(i);
            if (predicate.op != PredicateOperator::Equal) {
              throw UnsupportedOperation("role_permission query supports only equality predicates");
            }
            switch (predicate.field) {
            case core::RolePermission::Field::RoleId:
              sql += "role_id = ?";
              break;
            case core::RolePermission::Field::PermissionKey:
              sql += "permission_key = ?";
              break;
            }
            params.push_back(predicate.value.is_string() ? predicate.value.get<std::string>()
                                                         : predicate.value.dump());
          }
        }
        sql += " ORDER BY role_id, permission_key";

        Statement statement(transaction().handle(), sql);
        for (std::size_t i = 0; i < params.size(); ++i) {
          bind_text(statement.get(), static_cast<int>(i + 1), params.at(i));
        }
        std::vector<core::RolePermission> results;
        while (statement.step_row()) {
          results.push_back(read_role_permission(statement.get()));
        }
        return results;
      }

      void insert(const core::RolePermission& entity, const MutationContext& context) override {
        if (find_by_id(entity.id()).has_value()) {
          throw UniqueViolation("role permission already granted");
        }
        stage(entity.id(), PendingEntity{.entity = entity, .kind = PendingKind::Insert}, context);
      }

      void update(const core::RolePermission& /*entity*/,
                  const MutationContext& /*context*/) override {
        // RolePermission rows have only their composite key as state; updating
        // them has no semantic meaning. Callers should delete + re-insert.
        throw UnsupportedOperation("role_permission rows are immutable; insert + delete instead");
      }

      void soft_delete(const core::RolePermissionId& entity_id,
                       const MutationContext& context) override {
        auto entity = find_by_id(entity_id);
        if (!entity.has_value()) {
          throw NotFound("role permission grant not found");
        }
        stage(entity_id, PendingEntity{.entity = entity.value(), .kind = PendingKind::Delete},
              context);
      }

    private:
      void flush(sqlite3* handle) override {
        for (const auto& [unused_id, pending_entity] : pending()) {
          (void)unused_id;
          if (pending_entity.kind == PendingKind::Insert) {
            insert_pending(handle, pending_entity.entity);
          } else if (pending_entity.kind == PendingKind::Delete) {
            delete_pending(handle, pending_entity.entity);
          }
        }
      }

      [[nodiscard]] std::string id_string(const core::RolePermissionId& entity_id) const override {
        return entity_id.to_string();
      }

      [[nodiscard]] std::optional<core::RolePermission>
      load(const core::RolePermissionId& entity_id) const {
        Statement statement(transaction().handle(),
                            "SELECT role_id, permission_key FROM role_permissions "
                            "WHERE role_id = ? AND permission_key = ?");
        bind_text(statement.get(), 1, entity_id.role_id.to_string());
        bind_text(statement.get(), 2, std::string(core::to_key(entity_id.permission)));
        if (!statement.step_row()) {
          return std::nullopt;
        }
        return read_role_permission(statement.get());
      }

      static void insert_pending(sqlite3* handle, const core::RolePermission& entity) {
        Statement statement(handle, "INSERT INTO role_permissions (role_id, permission_key) "
                                    "VALUES (?, ?)");
        bind_text(statement.get(), 1, entity.role_id.to_string());
        bind_text(statement.get(), 2, std::string(core::to_key(entity.permission)));
        statement.step_done();
      }

      static void delete_pending(sqlite3* handle, const core::RolePermission& entity) {
        Statement statement(handle, "DELETE FROM role_permissions "
                                    "WHERE role_id = ? AND permission_key = ?");
        bind_text(statement.get(), 1, entity.role_id.to_string());
        bind_text(statement.get(), 2, std::string(core::to_key(entity.permission)));
        statement.step_done();
      }
    };

  } // namespace

  void register_role_repositories(SqliteBackend& backend) {
    backend.register_repository_factory<core::Role>([](SqliteTransaction& transaction) {
      return std::make_unique<RoleRepository>(transaction);
    });
    backend.register_repository_factory<core::RolePermission>([](SqliteTransaction& transaction) {
      return std::make_unique<RolePermissionRepository>(transaction);
    });
  }

} // namespace fmgr::storage
