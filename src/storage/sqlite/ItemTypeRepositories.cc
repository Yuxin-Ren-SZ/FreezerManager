// SPDX-License-Identifier: AGPL-3.0-or-later

#include "storage/sqlite/ItemTypeRepositories.h"

#include "core/item_type.h"
#include "storage/ItemTypeTraits.h"

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
          throw_sqlite_error(result, handle_, "prepare sqlite item type statement");
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
        throw_sqlite_error(result, handle_, "step sqlite item type statement");
      }

      void step_done() const {
        const auto result = sqlite3_step(statement_);
        if (result != SQLITE_DONE) {
          throw_sqlite_error(result, handle_, "execute sqlite item type statement");
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

    void bind_bool(sqlite3_stmt* statement, int index, bool value) {
      const auto result = sqlite3_bind_int(statement, index, value ? 1 : 0);
      if (result != SQLITE_OK) {
        throw ConstraintViolation("failed to bind sqlite bool parameter");
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

    [[nodiscard]] std::string json_path(const std::vector<std::string>& segments) {
      std::string path = "$";
      for (const auto& segment : segments) {
        path += ".";
        path += segment;
      }
      return path;
    }

    void bind_parameters(sqlite3_stmt* statement, const std::vector<nlohmann::json>& parameters) {
      int index = 1;
      for (const auto& parameter : parameters) {
        bind_json_parameter(statement, index, parameter);
        ++index;
      }
    }

    template <typename Entity, typename ColumnName>
    void append_generic_predicates(std::string& sql, std::vector<nlohmann::json>& parameters,
                                   const std::vector<std::string>& default_predicates,
                                   const std::vector<Predicate<Entity>>& predicates,
                                   ColumnName column_name) {
      std::vector<std::string> clauses = default_predicates;
      for (const auto& predicate : predicates) {
        const auto column = column_name(predicate.field);
        switch (predicate.op) {
        case PredicateOperator::Equal:
          clauses.push_back(column + " = ?");
          parameters.push_back(predicate.value);
          break;
        case PredicateOperator::GreaterThanOrEqual:
          clauses.push_back(column + " >= ?");
          parameters.push_back(predicate.value);
          break;
        case PredicateOperator::LessThanOrEqual:
          clauses.push_back(column + " <= ?");
          parameters.push_back(predicate.value);
          break;
        case PredicateOperator::Between:
          clauses.push_back(column + " BETWEEN ? AND ?");
          parameters.push_back(predicate.lower);
          parameters.push_back(predicate.upper);
          break;
        case PredicateOperator::In: {
          std::string clause = column + " IN (";
          for (std::size_t index = 0; index < predicate.values.size(); ++index) {
            if (index != 0) {
              clause += ", ";
            }
            clause += "?";
            parameters.push_back(predicate.values.at(index));
          }
          clause += ")";
          clauses.push_back(std::move(clause));
          break;
        }
        case PredicateOperator::JsonPathEqual:
          clauses.push_back("json_extract(" + column + ", ?) = ?");
          parameters.emplace_back(json_path(predicate.json_path));
          parameters.push_back(predicate.value);
          break;
        }
      }
      if (!clauses.empty()) {
        sql += " WHERE ";
        for (std::size_t index = 0; index < clauses.size(); ++index) {
          if (index != 0) {
            sql += " AND ";
          }
          sql += clauses.at(index);
        }
      }
    }

    template <typename Entity, typename ColumnName>
    void append_query_tail(std::string& sql, std::vector<nlohmann::json>& parameters,
                           const Query<Entity>& query_spec, ColumnName column_name) {
      if (!query_spec.sorts().empty()) {
        sql += " ORDER BY ";
        for (std::size_t index = 0; index < query_spec.sorts().size(); ++index) {
          if (index != 0) {
            sql += ", ";
          }
          const auto sort = query_spec.sorts().at(index);
          sql += column_name(sort.field);
          sql += sort.direction == SortDirection::Ascending ? " ASC" : " DESC";
        }
      }
      if (query_spec.limit_count().has_value()) {
        sql += " LIMIT ?";
        parameters.emplace_back(static_cast<std::int64_t>(query_spec.limit_count().value()));
      }
      if (query_spec.offset_count().has_value()) {
        if (!query_spec.limit_count().has_value()) {
          sql += " LIMIT -1";
        }
        sql += " OFFSET ?";
        parameters.emplace_back(static_cast<std::int64_t>(query_spec.offset_count().value()));
      }
    }

    // ---- ItemType column mapping ----

    [[nodiscard]] std::string item_type_column_name(core::ItemType::Field field) {
      switch (field) {
      case core::ItemType::Field::Id:
        return "id";
      case core::ItemType::Field::LabId:
        return "lab_id";
      case core::ItemType::Field::ParentId:
        return "parent_id";
      case core::ItemType::Field::Name:
        return "name";
      case core::ItemType::Field::CreatedAt:
        return "created_at_micros";
      case core::ItemType::Field::ArchivedAt:
        return "archived_at_micros";
      }
      throw ConstraintViolation("unknown item type field");
    }

    constexpr std::string_view k_item_type_columns =
        "id, lab_id, parent_id, name, created_at_micros, archived_at_micros";

    [[nodiscard]] core::ItemType read_item_type(sqlite3_stmt* statement) {
      return core::ItemType{
          .id = core::ItemTypeId::parse(column_text(statement, 0)),
          .lab_id = core::LabId::parse(column_text(statement, 1)),
          .parent_id = column_optional_id<core::ItemTypeId>(statement, 2),
          .name = column_text(statement, 3),
          .created_at = core::Timestamp::from_unix_micros(sqlite3_column_int64(statement, 4)),
          .archived_at = column_optional_timestamp(statement, 5),
      };
    }

    // ---- CustomFieldDefinition column mapping ----

    [[nodiscard]] std::string cfd_column_name(core::CustomFieldDefinition::Field field) {
      switch (field) {
      case core::CustomFieldDefinition::Field::Id:
        return "id";
      case core::CustomFieldDefinition::Field::LabId:
        return "lab_id";
      case core::CustomFieldDefinition::Field::ScopeKind:
        return "scope_kind";
      case core::CustomFieldDefinition::Field::ItemTypeId:
        return "item_type_id";
      case core::CustomFieldDefinition::Field::Key:
        return "key";
      case core::CustomFieldDefinition::Field::Label:
        return "label";
      case core::CustomFieldDefinition::Field::DataType:
        return "data_type";
      case core::CustomFieldDefinition::Field::Required:
        return "required";
      case core::CustomFieldDefinition::Field::ValidationJson:
        return "validation_json";
      case core::CustomFieldDefinition::Field::Indexed:
        return "indexed";
      case core::CustomFieldDefinition::Field::IsPhi:
        return "is_phi";
      case core::CustomFieldDefinition::Field::CreatedAt:
        return "created_at_micros";
      case core::CustomFieldDefinition::Field::ArchivedAt:
        return "archived_at_micros";
      }
      throw ConstraintViolation("unknown custom field definition field");
    }

    constexpr std::string_view k_cfd_columns =
        "id, lab_id, scope_kind, item_type_id, key, label, data_type, "
        "required, validation_json, indexed, is_phi, created_at_micros, archived_at_micros";

    [[nodiscard]] core::CustomFieldDefinition read_cfd(sqlite3_stmt* statement) {
      return core::CustomFieldDefinition{
          .id = core::CustomFieldDefinitionId::parse(column_text(statement, 0)),
          .lab_id = core::LabId::parse(column_text(statement, 1)),
          .scope_kind = core::parse_scope_kind(column_text(statement, 2)),
          .item_type_id = column_optional_id<core::ItemTypeId>(statement, 3),
          .key = column_text(statement, 4),
          .label = column_text(statement, 5),
          .data_type = core::parse_field_data_type(column_text(statement, 6)),
          .required = sqlite3_column_int(statement, 7) != 0,
          .validation_json = column_text(statement, 8),
          .indexed = sqlite3_column_int(statement, 9) != 0,
          .is_phi = sqlite3_column_int(statement, 10) != 0,
          .created_at = core::Timestamp::from_unix_micros(sqlite3_column_int64(statement, 11)),
          .archived_at = column_optional_timestamp(statement, 12),
      };
    }

    // ---- Validation ----

    void validate_item_type(const core::ItemType& item_type) {
      if (item_type.name.empty()) {
        throw ConstraintViolation("item type name is required");
      }
    }

    void validate_cfd(const core::CustomFieldDefinition& cfd,
                      const SqliteTransaction& transaction) {
      if (cfd.key.empty()) {
        throw ConstraintViolation("custom field key is required");
      }
      if (cfd.label.empty()) {
        throw ConstraintViolation("custom field label is required");
      }
      if (cfd.is_phi && cfd.indexed) {
        throw ConstraintViolation("PHI fields may not be indexed (see L10.3)");
      }
      if (cfd.item_type_id.has_value()) {
        Statement statement(transaction.handle(),
                            "SELECT 1 FROM item_types "
                            "WHERE id = ? AND lab_id = ? AND archived_at_micros IS NULL LIMIT 1");
        bind_text(statement.get(), 1, cfd.item_type_id->to_string());
        bind_text(statement.get(), 2, cfd.lab_id.to_string());
        if (!statement.step_row()) {
          throw ConstraintViolation("item_type_id does not reference a live ItemType in this lab");
        }
      }
    }

    // ---- Base repository template ----

    template <typename Entity> class SqliteItemTypeRepositoryBase : public IRepository<Entity> {
    public:
      explicit SqliteItemTypeRepositoryBase(SqliteTransaction& transaction)
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

    // ---- ItemTypeRepository ----

    class ItemTypeRepository final : public SqliteItemTypeRepositoryBase<core::ItemType> {
    public:
      using SqliteItemTypeRepositoryBase::SqliteItemTypeRepositoryBase;

      [[nodiscard]] std::optional<core::ItemType>
      find_by_id(const core::ItemTypeId& entity_id) override {
        if (auto staged = find_staged(entity_id); staged.has_value()) {
          return staged;
        }
        return load(entity_id);
      }

      [[nodiscard]] std::vector<core::ItemType>
      query(const Query<core::ItemType>& query_spec) override {
        std::string sql = "SELECT ";
        sql += k_item_type_columns;
        sql += " FROM item_types";
        std::vector<nlohmann::json> parameters;
        const auto defaults = query_spec.includes_tombstoned()
                                  ? std::vector<std::string>{}
                                  : std::vector<std::string>{"archived_at_micros IS NULL"};
        append_generic_predicates(sql, parameters, defaults, query_spec.predicates(),
                                  item_type_column_name);
        append_query_tail(sql, parameters, query_spec, item_type_column_name);

        Statement statement(transaction().handle(), sql);
        bind_parameters(statement.get(), parameters);

        std::vector<core::ItemType> results;
        while (statement.step_row()) {
          results.push_back(read_item_type(statement.get()));
        }
        return results;
      }

      void insert(const core::ItemType& entity, const MutationContext& context) override {
        check_no_cycle(entity);
        stage_insert(entity, context);
      }

      void update(const core::ItemType& entity, const MutationContext& context) override {
        check_no_cycle(entity);
        stage_update(entity, context);
      }

      void soft_delete(const core::ItemTypeId& entity_id, const MutationContext& context) override {
        auto entity = find_by_id(entity_id);
        if (!entity.has_value()) {
          throw NotFound("item type not found");
        }
        entity->archived_at = now_timestamp();
        // soft_delete does not change parent_id; bypass cycle check via direct staging.
        validate_item_type(entity.value());
        stage_update_after_validation(entity.value(), context);
      }

    private:
      void stage_update_after_validation(const core::ItemType& entity,
                                         const MutationContext& context) {
        const auto entity_id = entity.id;
        const auto is_insert = pending().contains(entity_id) && pending().at(entity_id).is_insert;
        pending().insert_or_assign(entity_id,
                                   PendingEntity{.entity = entity, .is_insert = is_insert});
        transaction().note_mutation(std::string(EntityTraits<core::ItemType>::entity_name()),
                                    entity_id.to_string(), context);
      }

      void check_no_cycle(const core::ItemType& entity) {
        if (!entity.parent_id.has_value()) {
          return;
        }
        std::set<core::ItemTypeId> visited;
        std::optional<core::ItemTypeId> cursor = entity.parent_id;
        while (cursor.has_value()) {
          if (cursor.value() == entity.id) {
            throw ConstraintViolation("item type parent chain forms a cycle");
          }
          if (!visited.insert(cursor.value()).second) {
            throw ConstraintViolation("item type parent chain forms a cycle");
          }
          const auto ancestor = lookup_for_cycle_check(cursor.value());
          if (!ancestor.has_value()) {
            return;
          }
          cursor = ancestor->parent_id;
        }
      }

      [[nodiscard]] std::optional<core::ItemType>
      lookup_for_cycle_check(const core::ItemTypeId& entity_id) {
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

      [[nodiscard]] std::optional<core::ItemType>
      load(const core::ItemTypeId& entity_id) const override {
        std::string sql = "SELECT ";
        sql += k_item_type_columns;
        sql += " FROM item_types WHERE id = ?";
        Statement statement(transaction().handle(), sql);
        bind_text(statement.get(), 1, entity_id.to_string());
        if (!statement.step_row()) {
          return std::nullopt;
        }
        return read_item_type(statement.get());
      }

      void validate(const core::ItemType& entity) const override {
        validate_item_type(entity);
      }

      [[nodiscard]] core::ItemTypeId id_of(const core::ItemType& entity) const override {
        return entity.id;
      }

      static void bind_entity(sqlite3_stmt* statement, const core::ItemType& entity) {
        bind_text(statement, 1, entity.id.to_string());
        bind_text(statement, 2, entity.lab_id.to_string());
        bind_optional_id(statement, 3, entity.parent_id);
        bind_text(statement, 4, entity.name);
        bind_int64(statement, 5, entity.created_at.unix_micros());
        bind_optional_timestamp(statement, 6, entity.archived_at);
      }

      static void insert_pending(sqlite3* handle, const core::ItemType& entity) {
        Statement statement(handle, "INSERT INTO item_types "
                                    "(id, lab_id, parent_id, name, "
                                    "created_at_micros, archived_at_micros) "
                                    "VALUES (?, ?, ?, ?, ?, ?)");
        bind_entity(statement.get(), entity);
        statement.step_done();
      }

      static void update_pending(sqlite3* handle, const core::ItemType& entity) {
        Statement statement(handle, "UPDATE item_types SET id = ?, lab_id = ?, parent_id = ?, "
                                    "name = ?, created_at_micros = ?, archived_at_micros = ? "
                                    "WHERE id = ?");
        bind_entity(statement.get(), entity);
        bind_text(statement.get(), 7, entity.id.to_string());
        statement.step_done();
      }
    };

    // ---- CustomFieldDefinitionRepository ----

    class CustomFieldDefinitionRepository final
        : public SqliteItemTypeRepositoryBase<core::CustomFieldDefinition> {
    public:
      using SqliteItemTypeRepositoryBase::SqliteItemTypeRepositoryBase;

      [[nodiscard]] std::optional<core::CustomFieldDefinition>
      find_by_id(const core::CustomFieldDefinitionId& entity_id) override {
        if (auto staged = find_staged(entity_id); staged.has_value()) {
          return staged;
        }
        return load(entity_id);
      }

      [[nodiscard]] std::vector<core::CustomFieldDefinition>
      query(const Query<core::CustomFieldDefinition>& query_spec) override {
        std::string sql = "SELECT ";
        sql += k_cfd_columns;
        sql += " FROM custom_field_definitions";
        std::vector<nlohmann::json> parameters;
        const auto defaults = query_spec.includes_tombstoned()
                                  ? std::vector<std::string>{}
                                  : std::vector<std::string>{"archived_at_micros IS NULL"};
        append_generic_predicates(sql, parameters, defaults, query_spec.predicates(),
                                  cfd_column_name);
        append_query_tail(sql, parameters, query_spec, cfd_column_name);

        Statement statement(transaction().handle(), sql);
        bind_parameters(statement.get(), parameters);

        std::vector<core::CustomFieldDefinition> results;
        while (statement.step_row()) {
          results.push_back(read_cfd(statement.get()));
        }
        return results;
      }

      void insert(const core::CustomFieldDefinition& entity,
                  const MutationContext& context) override {
        stage_insert(entity, context);
      }

      void update(const core::CustomFieldDefinition& entity,
                  const MutationContext& context) override {
        stage_update(entity, context);
      }

      void soft_delete(const core::CustomFieldDefinitionId& entity_id,
                       const MutationContext& context) override {
        auto entity = find_by_id(entity_id);
        if (!entity.has_value()) {
          throw NotFound("custom field definition not found");
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

      [[nodiscard]] std::optional<core::CustomFieldDefinition>
      load(const core::CustomFieldDefinitionId& entity_id) const override {
        std::string sql = "SELECT ";
        sql += k_cfd_columns;
        sql += " FROM custom_field_definitions WHERE id = ?";
        Statement statement(transaction().handle(), sql);
        bind_text(statement.get(), 1, entity_id.to_string());
        if (!statement.step_row()) {
          return std::nullopt;
        }
        return read_cfd(statement.get());
      }

      void validate(const core::CustomFieldDefinition& entity) const override {
        validate_cfd(entity, transaction());
      }

      [[nodiscard]] core::CustomFieldDefinitionId
      id_of(const core::CustomFieldDefinition& entity) const override {
        return entity.id;
      }

      static void bind_entity(sqlite3_stmt* statement, const core::CustomFieldDefinition& entity) {
        bind_text(statement, 1, entity.id.to_string());
        bind_text(statement, 2, entity.lab_id.to_string());
        bind_text(statement, 3, std::string(core::to_string(entity.scope_kind)));
        bind_optional_id(statement, 4, entity.item_type_id);
        bind_text(statement, 5, entity.key);
        bind_text(statement, 6, entity.label);
        bind_text(statement, 7, std::string(core::to_string(entity.data_type)));
        bind_bool(statement, 8, entity.required);
        bind_text(statement, 9, entity.validation_json);
        bind_bool(statement, 10, entity.indexed);
        bind_bool(statement, 11, entity.is_phi);
        bind_int64(statement, 12, entity.created_at.unix_micros());
        bind_optional_timestamp(statement, 13, entity.archived_at);
      }

      static void insert_pending(sqlite3* handle, const core::CustomFieldDefinition& entity) {
        Statement statement(handle, "INSERT INTO custom_field_definitions "
                                    "(id, lab_id, scope_kind, item_type_id, key, label, data_type, "
                                    "required, validation_json, indexed, is_phi, "
                                    "created_at_micros, archived_at_micros) "
                                    "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
        bind_entity(statement.get(), entity);
        statement.step_done();
      }

      static void update_pending(sqlite3* handle, const core::CustomFieldDefinition& entity) {
        Statement statement(handle, "UPDATE custom_field_definitions SET "
                                    "id = ?, lab_id = ?, scope_kind = ?, item_type_id = ?, "
                                    "key = ?, label = ?, data_type = ?, required = ?, "
                                    "validation_json = ?, indexed = ?, is_phi = ?, "
                                    "created_at_micros = ?, archived_at_micros = ? WHERE id = ?");
        bind_entity(statement.get(), entity);
        bind_text(statement.get(), 14, entity.id.to_string());
        statement.step_done();
      }
    };

  } // namespace

  void register_item_type_repositories(SqliteBackend& backend) {
    backend.register_repository_factory<core::ItemType>([](SqliteTransaction& transaction) {
      return std::make_unique<ItemTypeRepository>(transaction);
    });
    backend.register_repository_factory<core::CustomFieldDefinition>(
        [](SqliteTransaction& transaction) {
          return std::make_unique<CustomFieldDefinitionRepository>(transaction);
        });
  }

} // namespace fmgr::storage
