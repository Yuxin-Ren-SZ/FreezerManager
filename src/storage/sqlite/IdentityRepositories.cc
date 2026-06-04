// SPDX-License-Identifier: AGPL-3.0-or-later

#include "storage/sqlite/IdentityRepositories.h"

#include "core/identity.h"
#include "storage/IdentityTraits.h"
#include "storage/detail/IdentityColumns.h"
#include "storage/detail/QuerySqlBuilder.h"

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
          throw_sqlite_error(result, handle_, "prepare sqlite identity statement");
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
        throw_sqlite_error(result, handle_, "step sqlite identity statement");
      }

      void step_done() const {
        const auto result = sqlite3_step(statement_);
        if (result != SQLITE_DONE) {
          throw_sqlite_error(result, handle_, "execute sqlite identity statement");
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

    void bind_bool(sqlite3_stmt* statement, int index, bool value) {
      const auto result = sqlite3_bind_int(statement, index, value ? 1 : 0);
      if (result != SQLITE_OK) {
        throw ConstraintViolation("failed to bind sqlite bool parameter");
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

    template <typename StrongId>
    void bind_optional_id(sqlite3_stmt* statement, int index,
                          const std::optional<StrongId>& optional_id) {
      if (optional_id.has_value()) {
        bind_text(statement, index, optional_id->to_string());
      } else {
        bind_null(statement, index);
      }
    }

    void bind_optional_string(sqlite3_stmt* statement, int index,
                              const std::optional<std::string>& value) {
      if (value.has_value()) {
        bind_text(statement, index, value.value());
      } else {
        bind_null(statement, index);
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

    [[nodiscard]] std::optional<std::string> column_optional_text(sqlite3_stmt* statement,
                                                                  int column) {
      if (sqlite3_column_type(statement, column) == SQLITE_NULL) {
        return std::nullopt;
      }
      return column_text(statement, column);
    }

    template <typename StrongId>
    [[nodiscard]] std::optional<StrongId> column_optional_id(sqlite3_stmt* statement, int column) {
      const auto text = column_optional_text(statement, column);
      if (!text.has_value()) {
        return std::nullopt;
      }
      return StrongId::parse(text.value());
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

    template <typename Entity> class SqliteIdentityRepositoryBase : public IRepository<Entity> {
    public:
      explicit SqliteIdentityRepositoryBase(SqliteTransaction& transaction)
          : transaction_(transaction) {
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

      void stage_insert(const Entity& entity, const MutationContext& context) {
        const auto entity_id = id_of(entity);
        if (pending_.contains(entity_id) || load(entity_id).has_value()) {
          throw UniqueViolation(std::string(EntityTraits<Entity>::entity_name()) +
                                " id already exists");
        }
        validate(entity);
        pending_.insert_or_assign(entity_id, PendingEntity{.entity = entity, .is_insert = true});
        transaction_.note_mutation(std::string(EntityTraits<Entity>::entity_name()),
                                   entity_id.to_string(), context);
      }

      void stage_update(const Entity& entity, const MutationContext& context) {
        const auto entity_id = id_of(entity);
        if (!pending_.contains(entity_id) && !load(entity_id).has_value()) {
          throw NotFound(std::string(EntityTraits<Entity>::entity_name()) + " not found");
        }
        validate(entity);
        const auto is_insert = pending_.contains(entity_id) && pending_.at(entity_id).is_insert;
        pending_.insert_or_assign(entity_id,
                                  PendingEntity{.entity = entity, .is_insert = is_insert});
        transaction_.note_mutation(std::string(EntityTraits<Entity>::entity_name()),
                                   entity_id.to_string(), context);
      }

      [[nodiscard]] std::optional<Entity>
      find_staged(const typename EntityTraits<Entity>::Id& entity_id) const {
        const auto iterator = pending_.find(entity_id);
        if (iterator == pending_.end()) {
          return std::nullopt;
        }
        return iterator->second.entity;
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

    void bind_json_parameter(sqlite3_stmt* statement, int index, const nlohmann::json& value) {
      if (value.is_null()) {
        bind_null(statement, index);
        return;
      }
      if (value.is_number_integer()) {
        bind_int64(statement, index, value.get<std::int64_t>());
        return;
      }
      if (value.is_boolean()) {
        bind_bool(statement, index, value.get<bool>());
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

    [[nodiscard]] core::Lab read_lab(sqlite3_stmt* statement) {
      return core::Lab{
          .id = core::LabId::parse(column_text(statement, 0)),
          .name = column_text(statement, 1),
          .contact = column_text(statement, 2),
          .created_at = core::Timestamp::from_unix_micros(sqlite3_column_int64(statement, 3)),
          .settings_json = nlohmann::json::parse(column_text(statement, 4)),
          .is_phi_enabled = sqlite3_column_int(statement, 5) != 0,
          .archived_at = column_optional_timestamp(statement, 6),
      };
    }

    [[nodiscard]] core::User read_user(sqlite3_stmt* statement) {
      return core::User{
          .id = core::UserId::parse(column_text(statement, 0)),
          .primary_email = column_text(statement, 1),
          .display_name = column_text(statement, 2),
          .status = core::parse_user_status(column_text(statement, 3)),
          .created_at = core::Timestamp::from_unix_micros(sqlite3_column_int64(statement, 4)),
          .auth_bindings = nlohmann::json::parse(column_text(statement, 5)),
          .totp_secret_enc = column_optional_text(statement, 6),
          .default_lab_id = column_optional_id<core::LabId>(statement, 7),
      };
    }

    [[nodiscard]] core::LabMembership read_membership(sqlite3_stmt* statement) {
      return core::LabMembership{
          .user_id = core::UserId::parse(column_text(statement, 0)),
          .lab_id = core::LabId::parse(column_text(statement, 1)),
          .role_id = column_optional_id<core::RoleId>(statement, 2),
          .scope_filters_json = nlohmann::json::parse(column_text(statement, 3)),
          .invited_by = column_optional_id<core::UserId>(statement, 4),
          .joined_at = core::Timestamp::from_unix_micros(sqlite3_column_int64(statement, 5)),
          .revoked_at = column_optional_timestamp(statement, 6),
      };
    }

    class LabRepository final : public SqliteIdentityRepositoryBase<core::Lab> {
    public:
      using SqliteIdentityRepositoryBase::SqliteIdentityRepositoryBase;

      [[nodiscard]] std::optional<core::Lab> find_by_id(const core::LabId& entity_id) override {
        if (auto staged = find_staged(entity_id); staged.has_value()) {
          return staged;
        }
        return load(entity_id);
      }

      [[nodiscard]] std::vector<core::Lab> query(const Query<core::Lab>& query_spec) override {
        std::string sql = "SELECT id, name, contact, created_at_micros, settings_json, "
                          "is_phi_enabled, archived_at_micros FROM labs";
        std::vector<nlohmann::json> parameters;
        const auto defaults = query_spec.includes_tombstoned()
                                  ? std::vector<std::string>{}
                                  : std::vector<std::string>{"archived_at_micros IS NULL"};
        detail::SqliteDialect dialect;
        detail::append_where(sql, parameters, defaults, query_spec.predicates(),
                             detail::lab_column_name, dialect);
        detail::append_order_limit(sql, parameters, query_spec, detail::lab_column_name, dialect);

        Statement statement(transaction().handle(), sql);
        bind_parameters(statement.get(), parameters);

        std::vector<core::Lab> results;
        while (statement.step_row()) {
          results.push_back(read_lab(statement.get()));
        }
        return results;
      }

      void insert(const core::Lab& entity, const MutationContext& context) override {
        stage_insert(entity, context);
      }

      void update(const core::Lab& entity, const MutationContext& context) override {
        stage_update(entity, context);
      }

      void soft_delete(const core::LabId& entity_id, const MutationContext& context) override {
        auto entity = find_by_id(entity_id);
        if (!entity.has_value()) {
          throw NotFound("lab not found");
        }
        entity->archived_at = now_timestamp();
        update(entity.value(), context);
      }

    private:
      void flush(sqlite3* handle) override {
        for (const auto& [unused_id, pending] : pending()) {
          (void)unused_id;
          if (pending.is_insert) {
            insert_pending(handle, pending.entity);
          } else {
            update_pending(handle, pending.entity);
          }
        }
      }

      [[nodiscard]] std::optional<core::Lab> load(const core::LabId& entity_id) const override {
        Statement statement(transaction().handle(),
                            "SELECT id, name, contact, created_at_micros, settings_json, "
                            "is_phi_enabled, archived_at_micros FROM labs WHERE id = ?");
        bind_text(statement.get(), 1, entity_id.to_string());
        if (!statement.step_row()) {
          return std::nullopt;
        }
        return read_lab(statement.get());
      }

      void validate(const core::Lab& entity) const override {
        detail::validate_lab(entity);
      }

      [[nodiscard]] core::LabId id_of(const core::Lab& entity) const override {
        return entity.id;
      }

      static void bind_entity(sqlite3_stmt* statement, const core::Lab& entity) {
        bind_text(statement, 1, entity.id.to_string());
        bind_text(statement, 2, entity.name);
        bind_text(statement, 3, entity.contact);
        bind_int64(statement, 4, entity.created_at.unix_micros());
        bind_text(statement, 5, entity.settings_json.dump());
        bind_bool(statement, 6, entity.is_phi_enabled);
        bind_optional_timestamp(statement, 7, entity.archived_at);
      }

      static void insert_pending(sqlite3* handle, const core::Lab& entity) {
        Statement statement(handle, "INSERT INTO labs "
                                    "(id, name, contact, created_at_micros, settings_json, "
                                    "is_phi_enabled, archived_at_micros) "
                                    "VALUES (?, ?, ?, ?, ?, ?, ?)");
        bind_entity(statement.get(), entity);
        statement.step_done();
      }

      static void update_pending(sqlite3* handle, const core::Lab& entity) {
        Statement statement(handle, "UPDATE labs SET id = ?, name = ?, contact = ?, "
                                    "created_at_micros = ?, settings_json = ?, "
                                    "is_phi_enabled = ?, archived_at_micros = ? WHERE id = ?");
        bind_entity(statement.get(), entity);
        bind_text(statement.get(), 8, entity.id.to_string());
        statement.step_done();
      }
    };

    class UserRepository final : public SqliteIdentityRepositoryBase<core::User> {
    public:
      using SqliteIdentityRepositoryBase::SqliteIdentityRepositoryBase;

      [[nodiscard]] std::optional<core::User> find_by_id(const core::UserId& entity_id) override {
        if (auto staged = find_staged(entity_id); staged.has_value()) {
          return staged;
        }
        return load(entity_id);
      }

      [[nodiscard]] std::vector<core::User> query(const Query<core::User>& query_spec) override {
        std::string sql = "SELECT id, primary_email, display_name, status, created_at_micros, "
                          "auth_bindings_json, totp_secret_enc, default_lab_id FROM users";
        std::vector<nlohmann::json> parameters;
        const auto defaults = query_spec.includes_tombstoned()
                                  ? std::vector<std::string>{}
                                  : std::vector<std::string>{"status != 'disabled'"};
        detail::SqliteDialect dialect;
        detail::append_where(sql, parameters, defaults, query_spec.predicates(),
                             detail::user_column_name, dialect);
        detail::append_order_limit(sql, parameters, query_spec, detail::user_column_name, dialect);

        Statement statement(transaction().handle(), sql);
        bind_parameters(statement.get(), parameters);

        std::vector<core::User> results;
        while (statement.step_row()) {
          results.push_back(read_user(statement.get()));
        }
        return results;
      }

      void insert(const core::User& entity, const MutationContext& context) override {
        stage_insert(entity, context);
      }

      void update(const core::User& entity, const MutationContext& context) override {
        stage_update(entity, context);
      }

      void soft_delete(const core::UserId& entity_id, const MutationContext& context) override {
        auto entity = find_by_id(entity_id);
        if (!entity.has_value()) {
          throw NotFound("user not found");
        }
        entity->status = core::UserStatus::Disabled;
        update(entity.value(), context);
      }

    private:
      void flush(sqlite3* handle) override {
        for (const auto& [unused_id, pending] : pending()) {
          (void)unused_id;
          if (pending.is_insert) {
            insert_pending(handle, pending.entity);
          } else {
            update_pending(handle, pending.entity);
          }
        }
      }

      [[nodiscard]] std::optional<core::User> load(const core::UserId& entity_id) const override {
        Statement statement(transaction().handle(),
                            "SELECT id, primary_email, display_name, status, created_at_micros, "
                            "auth_bindings_json, totp_secret_enc, default_lab_id "
                            "FROM users WHERE id = ?");
        bind_text(statement.get(), 1, entity_id.to_string());
        if (!statement.step_row()) {
          return std::nullopt;
        }
        return read_user(statement.get());
      }

      void validate(const core::User& entity) const override {
        detail::validate_user(entity);
      }

      [[nodiscard]] core::UserId id_of(const core::User& entity) const override {
        return entity.id;
      }

      static void bind_entity(sqlite3_stmt* statement, const core::User& entity) {
        bind_text(statement, 1, entity.id.to_string());
        bind_text(statement, 2, entity.primary_email);
        bind_text(statement, 3, entity.display_name);
        bind_text(statement, 4, std::string(core::to_string(entity.status)));
        bind_int64(statement, 5, entity.created_at.unix_micros());
        bind_text(statement, 6, entity.auth_bindings.dump());
        bind_optional_string(statement, 7, entity.totp_secret_enc);
        bind_optional_id(statement, 8, entity.default_lab_id);
      }

      static void insert_pending(sqlite3* handle, const core::User& entity) {
        Statement statement(handle, "INSERT INTO users "
                                    "(id, primary_email, display_name, status, created_at_micros, "
                                    "auth_bindings_json, totp_secret_enc, default_lab_id) "
                                    "VALUES (?, ?, ?, ?, ?, ?, ?, ?)");
        bind_entity(statement.get(), entity);
        statement.step_done();
      }

      static void update_pending(sqlite3* handle, const core::User& entity) {
        Statement statement(handle, "UPDATE users SET id = ?, primary_email = ?, "
                                    "display_name = ?, status = ?, created_at_micros = ?, "
                                    "auth_bindings_json = ?, totp_secret_enc = ?, "
                                    "default_lab_id = ? WHERE id = ?");
        bind_entity(statement.get(), entity);
        bind_text(statement.get(), 9, entity.id.to_string());
        statement.step_done();
      }
    };

    class LabMembershipRepository final : public SqliteIdentityRepositoryBase<core::LabMembership> {
    public:
      using SqliteIdentityRepositoryBase::SqliteIdentityRepositoryBase;

      [[nodiscard]] std::optional<core::LabMembership>
      find_by_id(const core::LabMembershipId& entity_id) override {
        if (auto staged = find_staged(entity_id); staged.has_value()) {
          return staged;
        }
        return load(entity_id);
      }

      [[nodiscard]] std::vector<core::LabMembership>
      query(const Query<core::LabMembership>& query_spec) override {
        std::string sql = "SELECT user_id, lab_id, role_id, scope_filters_json, invited_by, "
                          "joined_at_micros, revoked_at_micros FROM lab_memberships";
        std::vector<nlohmann::json> parameters;
        const auto defaults = query_spec.includes_tombstoned()
                                  ? std::vector<std::string>{}
                                  : std::vector<std::string>{"revoked_at_micros IS NULL"};
        detail::SqliteDialect dialect;
        detail::append_where(sql, parameters, defaults, query_spec.predicates(),
                             detail::membership_column_name, dialect);
        detail::append_order_limit(sql, parameters, query_spec, detail::membership_column_name,
                                   dialect);

        Statement statement(transaction().handle(), sql);
        bind_parameters(statement.get(), parameters);

        std::vector<core::LabMembership> results;
        while (statement.step_row()) {
          results.push_back(read_membership(statement.get()));
        }
        return results;
      }

      void insert(const core::LabMembership& entity, const MutationContext& context) override {
        stage_insert(entity, context);
      }

      void update(const core::LabMembership& entity, const MutationContext& context) override {
        stage_update(entity, context);
      }

      void soft_delete(const core::LabMembershipId& entity_id,
                       const MutationContext& context) override {
        auto entity = find_by_id(entity_id);
        if (!entity.has_value()) {
          throw NotFound("lab membership not found");
        }
        entity->revoked_at = now_timestamp();
        update(entity.value(), context);
      }

    private:
      void flush(sqlite3* handle) override {
        for (const auto& [unused_id, pending] : pending()) {
          (void)unused_id;
          if (pending.is_insert) {
            insert_pending(handle, pending.entity);
          } else {
            update_pending(handle, pending.entity);
          }
        }
      }

      [[nodiscard]] std::optional<core::LabMembership>
      load(const core::LabMembershipId& entity_id) const override {
        Statement statement(transaction().handle(),
                            "SELECT user_id, lab_id, role_id, scope_filters_json, invited_by, "
                            "joined_at_micros, revoked_at_micros FROM lab_memberships "
                            "WHERE user_id = ? AND lab_id = ?");
        bind_text(statement.get(), 1, entity_id.user_id.to_string());
        bind_text(statement.get(), 2, entity_id.lab_id.to_string());
        if (!statement.step_row()) {
          return std::nullopt;
        }
        return read_membership(statement.get());
      }

      void validate(const core::LabMembership& entity) const override {
        detail::validate_membership(entity);
      }

      [[nodiscard]] core::LabMembershipId id_of(const core::LabMembership& entity) const override {
        return entity.id();
      }

      static void bind_entity(sqlite3_stmt* statement, const core::LabMembership& entity) {
        bind_text(statement, 1, entity.user_id.to_string());
        bind_text(statement, 2, entity.lab_id.to_string());
        bind_optional_id(statement, 3, entity.role_id);
        bind_text(statement, 4, entity.scope_filters_json.dump());
        bind_optional_id(statement, 5, entity.invited_by);
        bind_int64(statement, 6, entity.joined_at.unix_micros());
        bind_optional_timestamp(statement, 7, entity.revoked_at);
      }

      static void insert_pending(sqlite3* handle, const core::LabMembership& entity) {
        Statement statement(handle, "INSERT INTO lab_memberships "
                                    "(user_id, lab_id, role_id, scope_filters_json, invited_by, "
                                    "joined_at_micros, revoked_at_micros) "
                                    "VALUES (?, ?, ?, ?, ?, ?, ?)");
        bind_entity(statement.get(), entity);
        statement.step_done();
      }

      static void update_pending(sqlite3* handle, const core::LabMembership& entity) {
        Statement statement(handle, "UPDATE lab_memberships SET user_id = ?, lab_id = ?, "
                                    "role_id = ?, scope_filters_json = ?, invited_by = ?, "
                                    "joined_at_micros = ?, revoked_at_micros = ? "
                                    "WHERE user_id = ? AND lab_id = ?");
        bind_entity(statement.get(), entity);
        bind_text(statement.get(), 8, entity.user_id.to_string());
        bind_text(statement.get(), 9, entity.lab_id.to_string());
        statement.step_done();
      }
    };

  } // namespace

  void register_identity_repositories(SqliteBackend& backend) {
    backend.register_repository_factory<core::Lab>([](SqliteTransaction& transaction) {
      return std::make_unique<LabRepository>(transaction);
    });
    backend.register_repository_factory<core::User>([](SqliteTransaction& transaction) {
      return std::make_unique<UserRepository>(transaction);
    });
    backend.register_repository_factory<core::LabMembership>([](SqliteTransaction& transaction) {
      return std::make_unique<LabMembershipRepository>(transaction);
    });
  }

} // namespace fmgr::storage
