// SPDX-License-Identifier: AGPL-3.0-or-later

#include "storage/sqlite/LayoutRepositories.h"

#include "core/freezer.h"
#include "storage/FreezerTraits.h"
#include "storage/detail/LayoutColumns.h"
#include "storage/detail/QuerySqlBuilder.h"

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
          throw_sqlite_error(result, handle_, "prepare sqlite layout statement");
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
        throw_sqlite_error(result, handle_, "step sqlite layout statement");
      }

      void step_done() const {
        const auto result = sqlite3_step(statement_);
        if (result != SQLITE_DONE) {
          throw_sqlite_error(result, handle_, "execute sqlite layout statement");
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

    void bind_double(sqlite3_stmt* statement, int index, double value) {
      const auto result = sqlite3_bind_double(statement, index, value);
      if (result != SQLITE_OK) {
        throw ConstraintViolation("failed to bind sqlite double parameter");
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

    void bind_optional_double(sqlite3_stmt* statement, int index,
                              const std::optional<double>& value) {
      if (value.has_value()) {
        bind_double(statement, index, value.value());
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

    void bind_bool(sqlite3_stmt* statement, int index, bool value) {
      const auto result = sqlite3_bind_int(statement, index, value ? 1 : 0);
      if (result != SQLITE_OK) {
        throw ConstraintViolation("failed to bind sqlite bool parameter");
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

    [[nodiscard]] std::optional<double> column_optional_double(sqlite3_stmt* statement,
                                                               int column) {
      if (sqlite3_column_type(statement, column) == SQLITE_NULL) {
        return std::nullopt;
      }
      return sqlite3_column_double(statement, column);
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

    using detail::validate_freezer;
    using detail::validate_storage_container;

    template <typename Entity> class SqliteLayoutRepositoryBase : public IRepository<Entity> {
    public:
      explicit SqliteLayoutRepositoryBase(SqliteTransaction& transaction)
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

      [[nodiscard]] std::map<typename EntityTraits<Entity>::Id, PendingEntity>& pending() {
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

    [[nodiscard]] core::Freezer read_freezer(sqlite3_stmt* statement) {
      return core::Freezer{
          .id = core::FreezerId::parse(column_text(statement, 0)),
          .lab_id = core::LabId::parse(column_text(statement, 1)),
          .name = column_text(statement, 2),
          .location = column_text(statement, 3),
          .model = column_text(statement, 4),
          .temp_target_c = column_optional_double(statement, 5),
          .layout_root_id = core::StorageContainerId::parse(column_text(statement, 6)),
          .created_at = core::Timestamp::from_unix_micros(sqlite3_column_int64(statement, 7)),
          .archived_at = column_optional_timestamp(statement, 8),
      };
    }

    [[nodiscard]] core::StorageContainer read_container(sqlite3_stmt* statement) {
      auto capacity_hint_text = column_text(statement, 7);
      std::optional<core::CapacityHint> capacity_hint;
      const auto capacity_json = nlohmann::json::parse(capacity_hint_text);
      if (!capacity_json.is_null()) {
        capacity_hint = capacity_json.get<core::CapacityHint>();
      }
      return core::StorageContainer{
          .id = core::StorageContainerId::parse(column_text(statement, 0)),
          .lab_id = core::LabId::parse(column_text(statement, 1)),
          .parent_id = column_optional_id<core::StorageContainerId>(statement, 2),
          .kind = core::parse_container_kind(column_text(statement, 3)),
          .name = column_text(statement, 4),
          .label = column_text(statement, 5),
          .ordering_index = sqlite3_column_int(statement, 6),
          .capacity_hint = capacity_hint,
          .created_at = core::Timestamp::from_unix_micros(sqlite3_column_int64(statement, 8)),
          .archived_at = column_optional_timestamp(statement, 9),
      };
    }

    constexpr std::string_view k_freezer_columns =
        "id, lab_id, name, location, model, temp_target_c, layout_root_id, "
        "created_at_micros, archived_at_micros";

    constexpr std::string_view k_container_columns =
        "id, lab_id, parent_id, kind, name, label, ordering_index, "
        "capacity_hint_json, created_at_micros, archived_at_micros";

    class FreezerRepository final : public SqliteLayoutRepositoryBase<core::Freezer> {
    public:
      using SqliteLayoutRepositoryBase::SqliteLayoutRepositoryBase;

      [[nodiscard]] std::optional<core::Freezer>
      find_by_id(const core::FreezerId& entity_id) override {
        if (auto staged = find_staged(entity_id); staged.has_value()) {
          return staged;
        }
        return load(entity_id);
      }

      [[nodiscard]] std::vector<core::Freezer>
      query(const Query<core::Freezer>& query_spec) override {
        std::string sql = "SELECT ";
        sql += k_freezer_columns;
        sql += " FROM freezers";
        std::vector<nlohmann::json> parameters;
        const auto defaults = query_spec.includes_tombstoned()
                                  ? std::vector<std::string>{}
                                  : std::vector<std::string>{"archived_at_micros IS NULL"};
        detail::SqliteDialect dialect;
        detail::append_where(sql, parameters, defaults, query_spec.predicates(),
                             detail::freezer_column_name, dialect);
        detail::append_order_limit(sql, parameters, query_spec, detail::freezer_column_name,
                                   dialect);

        Statement statement(transaction().handle(), sql);
        bind_parameters(statement.get(), parameters);

        std::vector<core::Freezer> results;
        while (statement.step_row()) {
          results.push_back(read_freezer(statement.get()));
        }
        return results;
      }

      void insert(const core::Freezer& entity, const MutationContext& context) override {
        stage_insert(entity, context);
      }

      void update(const core::Freezer& entity, const MutationContext& context) override {
        stage_update(entity, context);
      }

      void soft_delete(const core::FreezerId& entity_id, const MutationContext& context) override {
        auto entity = find_by_id(entity_id);
        if (!entity.has_value()) {
          throw NotFound("freezer not found");
        }
        entity->archived_at = now_timestamp();
        update(entity.value(), context);
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

      [[nodiscard]] std::optional<core::Freezer>
      load(const core::FreezerId& entity_id) const override {
        std::string sql = "SELECT ";
        sql += k_freezer_columns;
        sql += " FROM freezers WHERE id = ?";
        Statement statement(transaction().handle(), sql);
        bind_text(statement.get(), 1, entity_id.to_string());
        if (!statement.step_row()) {
          return std::nullopt;
        }
        return read_freezer(statement.get());
      }

      void validate(const core::Freezer& entity) const override {
        validate_freezer(entity);
      }

      [[nodiscard]] core::FreezerId id_of(const core::Freezer& entity) const override {
        return entity.id;
      }

      static void bind_entity(sqlite3_stmt* statement, const core::Freezer& entity) {
        bind_text(statement, 1, entity.id.to_string());
        bind_text(statement, 2, entity.lab_id.to_string());
        bind_text(statement, 3, entity.name);
        bind_text(statement, 4, entity.location);
        bind_text(statement, 5, entity.model);
        bind_optional_double(statement, 6, entity.temp_target_c);
        bind_text(statement, 7, entity.layout_root_id.to_string());
        bind_int64(statement, 8, entity.created_at.unix_micros());
        bind_optional_timestamp(statement, 9, entity.archived_at);
      }

      static void insert_pending(sqlite3* handle, const core::Freezer& entity) {
        Statement statement(handle, "INSERT INTO freezers "
                                    "(id, lab_id, name, location, model, temp_target_c, "
                                    "layout_root_id, created_at_micros, archived_at_micros) "
                                    "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)");
        bind_entity(statement.get(), entity);
        statement.step_done();
      }

      static void update_pending(sqlite3* handle, const core::Freezer& entity) {
        Statement statement(handle,
                            "UPDATE freezers SET id = ?, lab_id = ?, name = ?, location = ?, "
                            "model = ?, temp_target_c = ?, layout_root_id = ?, "
                            "created_at_micros = ?, archived_at_micros = ? WHERE id = ?");
        bind_entity(statement.get(), entity);
        bind_text(statement.get(), 10, entity.id.to_string());
        statement.step_done();
      }
    };

    class StorageContainerRepository final
        : public SqliteLayoutRepositoryBase<core::StorageContainer> {
    public:
      using SqliteLayoutRepositoryBase::SqliteLayoutRepositoryBase;

      [[nodiscard]] std::optional<core::StorageContainer>
      find_by_id(const core::StorageContainerId& entity_id) override {
        if (auto staged = find_staged(entity_id); staged.has_value()) {
          return staged;
        }
        return load(entity_id);
      }

      [[nodiscard]] std::vector<core::StorageContainer>
      query(const Query<core::StorageContainer>& query_spec) override {
        std::string sql = "SELECT ";
        sql += k_container_columns;
        sql += " FROM storage_containers";
        std::vector<nlohmann::json> parameters;
        const auto defaults = query_spec.includes_tombstoned()
                                  ? std::vector<std::string>{}
                                  : std::vector<std::string>{"archived_at_micros IS NULL"};
        detail::SqliteDialect dialect;
        detail::append_where(sql, parameters, defaults, query_spec.predicates(),
                             detail::container_column_name, dialect);
        detail::append_order_limit(sql, parameters, query_spec, detail::container_column_name,
                                   dialect);

        Statement statement(transaction().handle(), sql);
        bind_parameters(statement.get(), parameters);

        std::vector<core::StorageContainer> results;
        while (statement.step_row()) {
          results.push_back(read_container(statement.get()));
        }
        return results;
      }

      void insert(const core::StorageContainer& entity, const MutationContext& context) override {
        check_no_cycle(entity);
        stage_insert(entity, context);
      }

      void update(const core::StorageContainer& entity, const MutationContext& context) override {
        check_no_cycle(entity);
        stage_update(entity, context);
      }

      void soft_delete(const core::StorageContainerId& entity_id,
                       const MutationContext& context) override {
        auto entity = find_by_id(entity_id);
        if (!entity.has_value()) {
          throw NotFound("storage container not found");
        }
        entity->archived_at = now_timestamp();
        // soft_delete does not change parent_id; bypass cycle check via base helper.
        validate_storage_container(entity.value());
        stage_update_after_validation(entity.value(), context);
      }

    private:
      void stage_update_after_validation(const core::StorageContainer& entity,
                                         const MutationContext& context) {
        const auto entity_id = entity.id;
        const auto is_insert = pending().contains(entity_id) && pending().at(entity_id).is_insert;
        pending().insert_or_assign(entity_id,
                                   PendingEntity{.entity = entity, .is_insert = is_insert});
        transaction().note_mutation(
            std::string(EntityTraits<core::StorageContainer>::entity_name()), entity_id.to_string(),
            context);
      }

      void check_no_cycle(const core::StorageContainer& entity) {
        if (!entity.parent_id.has_value()) {
          return;
        }
        // Walk the prospective parent's ancestor chain; if we ever reach `entity.id`,
        // the proposed edge would form a cycle.
        std::set<core::StorageContainerId> visited;
        std::optional<core::StorageContainerId> cursor = entity.parent_id;
        while (cursor.has_value()) {
          if (cursor.value() == entity.id) {
            throw ConstraintViolation("storage container parent chain forms a cycle");
          }
          if (!visited.insert(cursor.value()).second) {
            // Pre-existing cycle in the data; bail out conservatively.
            throw ConstraintViolation("storage container parent chain forms a cycle");
          }
          const auto ancestor = lookup_for_cycle_check(cursor.value());
          if (!ancestor.has_value()) {
            return;
          }
          cursor = ancestor->parent_id;
        }
      }

      [[nodiscard]] std::optional<core::StorageContainer>
      lookup_for_cycle_check(const core::StorageContainerId& entity_id) {
        if (auto staged = find_staged(entity_id); staged.has_value()) {
          return staged;
        }
        return load(entity_id);
      }

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

      [[nodiscard]] std::optional<core::StorageContainer>
      load(const core::StorageContainerId& entity_id) const override {
        std::string sql = "SELECT ";
        sql += k_container_columns;
        sql += " FROM storage_containers WHERE id = ?";
        Statement statement(transaction().handle(), sql);
        bind_text(statement.get(), 1, entity_id.to_string());
        if (!statement.step_row()) {
          return std::nullopt;
        }
        return read_container(statement.get());
      }

      void validate(const core::StorageContainer& entity) const override {
        validate_storage_container(entity);
      }

      [[nodiscard]] core::StorageContainerId
      id_of(const core::StorageContainer& entity) const override {
        return entity.id;
      }

      [[nodiscard]] static std::string
      capacity_hint_dump(const std::optional<core::CapacityHint>& hint) {
        if (!hint.has_value()) {
          return "null";
        }
        nlohmann::json json = hint.value();
        return json.dump();
      }

      static void bind_entity(sqlite3_stmt* statement, const core::StorageContainer& entity) {
        bind_text(statement, 1, entity.id.to_string());
        bind_text(statement, 2, entity.lab_id.to_string());
        bind_optional_id(statement, 3, entity.parent_id);
        bind_text(statement, 4, std::string(core::to_string(entity.kind)));
        bind_text(statement, 5, entity.name);
        bind_text(statement, 6, entity.label);
        bind_int64(statement, 7, entity.ordering_index);
        bind_text(statement, 8, capacity_hint_dump(entity.capacity_hint));
        bind_int64(statement, 9, entity.created_at.unix_micros());
        bind_optional_timestamp(statement, 10, entity.archived_at);
      }

      static void insert_pending(sqlite3* handle, const core::StorageContainer& entity) {
        Statement statement(handle, "INSERT INTO storage_containers "
                                    "(id, lab_id, parent_id, kind, name, label, ordering_index, "
                                    "capacity_hint_json, created_at_micros, archived_at_micros) "
                                    "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
        bind_entity(statement.get(), entity);
        statement.step_done();
      }

      static void update_pending(sqlite3* handle, const core::StorageContainer& entity) {
        Statement statement(handle,
                            "UPDATE storage_containers SET id = ?, lab_id = ?, parent_id = ?, "
                            "kind = ?, name = ?, label = ?, ordering_index = ?, "
                            "capacity_hint_json = ?, created_at_micros = ?, "
                            "archived_at_micros = ? WHERE id = ?");
        bind_entity(statement.get(), entity);
        bind_text(statement.get(), 11, entity.id.to_string());
        statement.step_done();
      }
    };

  } // namespace

  void register_layout_repositories(SqliteBackend& backend) {
    backend.register_repository_factory<core::StorageContainer>([](SqliteTransaction& transaction) {
      return std::make_unique<StorageContainerRepository>(transaction);
    });
    backend.register_repository_factory<core::Freezer>([](SqliteTransaction& transaction) {
      return std::make_unique<FreezerRepository>(transaction);
    });
  }

} // namespace fmgr::storage
