// SPDX-License-Identifier: AGPL-3.0-or-later

#include "storage/sqlite/BoxGeometryRepositories.h"

#include "core/box.h"
#include "storage/BoxGeometryTraits.h"

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
          throw_sqlite_error(result, handle_, "prepare sqlite box geometry statement");
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
        throw_sqlite_error(result, handle_, "step sqlite box geometry statement");
      }

      void step_done() const {
        const auto result = sqlite3_step(statement_);
        if (result != SQLITE_DONE) {
          throw_sqlite_error(result, handle_, "execute sqlite box geometry statement");
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

    void bind_bool(sqlite3_stmt* statement, int index, bool value) {
      const auto result = sqlite3_bind_int(statement, index, value ? 1 : 0);
      if (result != SQLITE_OK) {
        throw ConstraintViolation("failed to bind sqlite bool parameter");
      }
    }

    void bind_null(sqlite3_stmt* statement, int index) {
      const auto result = sqlite3_bind_null(statement, index);
      if (result != SQLITE_OK) {
        throw ConstraintViolation("failed to bind sqlite null parameter");
      }
    }

    void bind_optional_int64(sqlite3_stmt* statement, int index,
                             const std::optional<std::int32_t>& value) {
      if (value.has_value()) {
        bind_int64(statement, index, value.value());
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

    [[nodiscard]] std::optional<core::Timestamp> column_optional_timestamp(sqlite3_stmt* statement,
                                                                           int column) {
      if (sqlite3_column_type(statement, column) == SQLITE_NULL) {
        return std::nullopt;
      }
      return core::Timestamp::from_unix_micros(sqlite3_column_int64(statement, column));
    }

    [[nodiscard]] std::optional<std::int32_t> column_optional_int(sqlite3_stmt* statement,
                                                                  int column) {
      if (sqlite3_column_type(statement, column) == SQLITE_NULL) {
        return std::nullopt;
      }
      return sqlite3_column_int(statement, column);
    }

    [[nodiscard]] core::Timestamp now_timestamp() {
      const auto now =
          std::chrono::time_point_cast<std::chrono::microseconds>(std::chrono::system_clock::now());
      return core::Timestamp::from_unix_micros(now.time_since_epoch().count());
    }

    [[nodiscard]] std::string
    dimensions_dump(const std::optional<core::OuterDimensionsMm>& dimensions) {
      if (!dimensions.has_value()) {
        return "null";
      }
      const nlohmann::json json = dimensions.value();
      return json.dump();
    }

    [[nodiscard]] std::optional<core::OuterDimensionsMm> parse_dimensions(std::string_view text) {
      const auto json = nlohmann::json::parse(text);
      if (json.is_null()) {
        return std::nullopt;
      }
      return json.get<core::OuterDimensionsMm>();
    }

    void validate_container_type(const core::ContainerType& container_type) {
      if (container_type.name.empty()) {
        throw ConstraintViolation("container type name is required");
      }
      if (container_type.size_class.empty()) {
        throw ConstraintViolation("container type size_class is required");
      }
      if (container_type.outer_dimensions_mm.has_value()) {
        const auto& dimensions = container_type.outer_dimensions_mm.value();
        if (dimensions.width <= 0.0 || dimensions.height <= 0.0 || dimensions.depth <= 0.0) {
          throw ConstraintViolation("container type dimensions must be positive");
        }
      }
    }

    void validate_box_type_shape(const core::BoxType& box_type) {
      if (box_type.name.empty()) {
        throw ConstraintViolation("box type name is required");
      }
      std::set<std::string> labels;
      for (const auto& position : box_type.positions) {
        if (position.label.empty()) {
          throw ConstraintViolation("box type position label is required");
        }
        if (!labels.insert(position.label).second) {
          throw ConstraintViolation("box type position labels must be unique");
        }
        if (position.row < 0 || position.col < 0 || (position.z.has_value() && *position.z < 0)) {
          throw ConstraintViolation("box type position coordinates must be non-negative");
        }
        if (position.accepts.empty()) {
          throw ConstraintViolation("box type position accepts must be non-empty");
        }
        std::set<std::string> accepts;
        for (const auto& size_class : position.accepts) {
          if (size_class.empty()) {
            throw ConstraintViolation("box type position accepts must be non-empty");
          }
          if (!accepts.insert(size_class).second) {
            throw ConstraintViolation("box type position accepts must be unique");
          }
        }
      }
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

    [[nodiscard]] std::string container_type_column_name(core::ContainerType::Field field) {
      switch (field) {
      case core::ContainerType::Field::Id:
        return "id";
      case core::ContainerType::Field::LabId:
        return "lab_id";
      case core::ContainerType::Field::Name:
        return "name";
      case core::ContainerType::Field::SizeClass:
        return "size_class";
      case core::ContainerType::Field::OuterDimensionsMm:
        return "outer_dimensions_json";
      case core::ContainerType::Field::Material:
        return "material";
      case core::ContainerType::Field::SupplierSku:
        return "supplier_sku";
      case core::ContainerType::Field::CreatedAt:
        return "created_at_micros";
      case core::ContainerType::Field::ArchivedAt:
        return "archived_at_micros";
      }
      throw ConstraintViolation("unknown container type field");
    }

    [[nodiscard]] std::string box_type_column_name(core::BoxType::Field field) {
      switch (field) {
      case core::BoxType::Field::Id:
        return "id";
      case core::BoxType::Field::LabId:
        return "lab_id";
      case core::BoxType::Field::Name:
        return "name";
      case core::BoxType::Field::Manufacturer:
        return "manufacturer";
      case core::BoxType::Field::Sku:
        return "sku";
      case core::BoxType::Field::Positions:
        throw UnsupportedOperation("box type position queries are not supported");
      case core::BoxType::Field::CreatedAt:
        return "created_at_micros";
      case core::BoxType::Field::ArchivedAt:
        return "archived_at_micros";
      }
      throw ConstraintViolation("unknown box type field");
    }

    [[nodiscard]] core::ContainerType read_container_type(sqlite3_stmt* statement) {
      return core::ContainerType{
          .id = core::ContainerTypeId::parse(column_text(statement, 0)),
          .lab_id = core::LabId::parse(column_text(statement, 1)),
          .name = column_text(statement, 2),
          .size_class = column_text(statement, 3),
          .outer_dimensions_mm = parse_dimensions(column_text(statement, 4)),
          .material = column_text(statement, 5),
          .supplier_sku = column_text(statement, 6),
          .created_at = core::Timestamp::from_unix_micros(sqlite3_column_int64(statement, 7)),
          .archived_at = column_optional_timestamp(statement, 8),
      };
    }

    [[nodiscard]] core::Position read_position(sqlite3_stmt* statement) {
      return core::Position{
          .label = column_text(statement, 0),
          .row = sqlite3_column_int(statement, 1),
          .col = sqlite3_column_int(statement, 2),
          .z = column_optional_int(statement, 3),
      };
    }

    constexpr std::string_view k_container_type_columns =
        "id, lab_id, name, size_class, outer_dimensions_json, material, supplier_sku, "
        "created_at_micros, archived_at_micros";

    constexpr std::string_view k_box_type_columns =
        "id, lab_id, name, manufacturer, sku, created_at_micros, archived_at_micros";

    template <typename Entity> class SqliteBoxGeometryRepositoryBase : public IRepository<Entity> {
    public:
      explicit SqliteBoxGeometryRepositoryBase(SqliteTransaction& transaction)
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

    class ContainerTypeRepository final
        : public SqliteBoxGeometryRepositoryBase<core::ContainerType> {
    public:
      using SqliteBoxGeometryRepositoryBase::SqliteBoxGeometryRepositoryBase;

      [[nodiscard]] std::optional<core::ContainerType>
      find_by_id(const core::ContainerTypeId& entity_id) override {
        if (auto staged = find_staged(entity_id); staged.has_value()) {
          return staged;
        }
        return load(entity_id);
      }

      [[nodiscard]] std::vector<core::ContainerType>
      query(const Query<core::ContainerType>& query_spec) override {
        std::string sql = "SELECT ";
        sql += k_container_type_columns;
        sql += " FROM container_types";
        std::vector<nlohmann::json> parameters;
        const auto defaults = query_spec.includes_tombstoned()
                                  ? std::vector<std::string>{}
                                  : std::vector<std::string>{"archived_at_micros IS NULL"};
        append_generic_predicates(sql, parameters, defaults, query_spec.predicates(),
                                  container_type_column_name);
        append_query_tail(sql, parameters, query_spec, container_type_column_name);

        Statement statement(transaction().handle(), sql);
        bind_parameters(statement.get(), parameters);

        std::vector<core::ContainerType> results;
        while (statement.step_row()) {
          results.push_back(read_container_type(statement.get()));
        }
        return results;
      }

      void insert(const core::ContainerType& entity, const MutationContext& context) override {
        stage_insert(entity, context);
      }

      void update(const core::ContainerType& entity, const MutationContext& context) override {
        stage_update(entity, context);
      }

      void soft_delete(const core::ContainerTypeId& entity_id,
                       const MutationContext& context) override {
        auto entity = find_by_id(entity_id);
        if (!entity.has_value()) {
          throw NotFound("container type not found");
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

      [[nodiscard]] std::optional<core::ContainerType>
      load(const core::ContainerTypeId& entity_id) const override {
        std::string sql = "SELECT ";
        sql += k_container_type_columns;
        sql += " FROM container_types WHERE id = ?";
        Statement statement(transaction().handle(), sql);
        bind_text(statement.get(), 1, entity_id.to_string());
        if (!statement.step_row()) {
          return std::nullopt;
        }
        return read_container_type(statement.get());
      }

      void validate(const core::ContainerType& entity) const override {
        validate_container_type(entity);
      }

      [[nodiscard]] core::ContainerTypeId id_of(const core::ContainerType& entity) const override {
        return entity.id;
      }

      static void bind_entity(sqlite3_stmt* statement, const core::ContainerType& entity) {
        bind_text(statement, 1, entity.id.to_string());
        bind_text(statement, 2, entity.lab_id.to_string());
        bind_text(statement, 3, entity.name);
        bind_text(statement, 4, entity.size_class);
        bind_text(statement, 5, dimensions_dump(entity.outer_dimensions_mm));
        bind_text(statement, 6, entity.material);
        bind_text(statement, 7, entity.supplier_sku);
        bind_int64(statement, 8, entity.created_at.unix_micros());
        bind_optional_timestamp(statement, 9, entity.archived_at);
      }

      static void insert_pending(sqlite3* handle, const core::ContainerType& entity) {
        Statement statement(handle, "INSERT INTO container_types "
                                    "(id, lab_id, name, size_class, outer_dimensions_json, "
                                    "material, supplier_sku, created_at_micros, "
                                    "archived_at_micros) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)");
        bind_entity(statement.get(), entity);
        statement.step_done();
      }

      static void update_pending(sqlite3* handle, const core::ContainerType& entity) {
        Statement statement(handle,
                            "UPDATE container_types SET id = ?, lab_id = ?, name = ?, "
                            "size_class = ?, outer_dimensions_json = ?, material = ?, "
                            "supplier_sku = ?, created_at_micros = ?, archived_at_micros = ? "
                            "WHERE id = ?");
        bind_entity(statement.get(), entity);
        bind_text(statement.get(), 10, entity.id.to_string());
        statement.step_done();
      }
    };

    class BoxTypeRepository final : public SqliteBoxGeometryRepositoryBase<core::BoxType> {
    public:
      using SqliteBoxGeometryRepositoryBase::SqliteBoxGeometryRepositoryBase;

      [[nodiscard]] std::optional<core::BoxType>
      find_by_id(const core::BoxTypeId& entity_id) override {
        if (auto staged = find_staged(entity_id); staged.has_value()) {
          return staged;
        }
        return load(entity_id);
      }

      [[nodiscard]] std::vector<core::BoxType>
      query(const Query<core::BoxType>& query_spec) override {
        std::string sql = "SELECT ";
        sql += k_box_type_columns;
        sql += " FROM box_types";
        std::vector<nlohmann::json> parameters;
        const auto defaults = query_spec.includes_tombstoned()
                                  ? std::vector<std::string>{}
                                  : std::vector<std::string>{"archived_at_micros IS NULL"};
        append_generic_predicates(sql, parameters, defaults, query_spec.predicates(),
                                  box_type_column_name);
        append_query_tail(sql, parameters, query_spec, box_type_column_name);

        Statement statement(transaction().handle(), sql);
        bind_parameters(statement.get(), parameters);

        std::vector<core::BoxType> results;
        while (statement.step_row()) {
          results.push_back(read_box_type(statement.get()));
        }
        return results;
      }

      void insert(const core::BoxType& entity, const MutationContext& context) override {
        stage_insert(entity, context);
      }

      void update(const core::BoxType& entity, const MutationContext& context) override {
        stage_update(entity, context);
      }

      void soft_delete(const core::BoxTypeId& entity_id, const MutationContext& context) override {
        auto entity = find_by_id(entity_id);
        if (!entity.has_value()) {
          throw NotFound("box type not found");
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
          replace_positions(handle, pending_entity.entity);
        }
      }

      [[nodiscard]] std::optional<core::BoxType>
      load(const core::BoxTypeId& entity_id) const override {
        std::string sql = "SELECT ";
        sql += k_box_type_columns;
        sql += " FROM box_types WHERE id = ?";
        Statement statement(transaction().handle(), sql);
        bind_text(statement.get(), 1, entity_id.to_string());
        if (!statement.step_row()) {
          return std::nullopt;
        }
        return read_box_type(statement.get());
      }

      void validate(const core::BoxType& entity) const override {
        validate_box_type_shape(entity);
        for (const auto& position : entity.positions) {
          for (const auto& size_class : position.accepts) {
            if (!live_size_class_exists(entity.lab_id, size_class)) {
              throw ConstraintViolation("box type position accepts unknown size_class");
            }
          }
        }
      }

      [[nodiscard]] core::BoxTypeId id_of(const core::BoxType& entity) const override {
        return entity.id;
      }

      [[nodiscard]] bool live_size_class_exists(core::LabId lab_id,
                                                const std::string& size_class) const {
        Statement statement(transaction().handle(),
                            "SELECT 1 FROM container_types "
                            "WHERE lab_id = ? AND size_class = ? AND archived_at_micros IS NULL "
                            "LIMIT 1");
        bind_text(statement.get(), 1, lab_id.to_string());
        bind_text(statement.get(), 2, size_class);
        return statement.step_row();
      }

      [[nodiscard]] std::vector<std::string> load_accepts(const core::BoxTypeId& box_type_id,
                                                          const std::string& label) const {
        Statement statement(transaction().handle(),
                            "SELECT size_class FROM box_type_position_accepts "
                            "WHERE box_type_id = ? AND position_label = ? ORDER BY size_class");
        bind_text(statement.get(), 1, box_type_id.to_string());
        bind_text(statement.get(), 2, label);
        std::vector<std::string> accepts;
        while (statement.step_row()) {
          accepts.push_back(column_text(statement.get(), 0));
        }
        return accepts;
      }

      [[nodiscard]] std::vector<core::Position>
      load_positions(const core::BoxTypeId& box_type_id) const {
        Statement statement(transaction().handle(),
                            "SELECT label, row_index, col_index, z_index "
                            "FROM box_type_positions WHERE box_type_id = ? "
                            "ORDER BY row_index, col_index, z_index, label");
        bind_text(statement.get(), 1, box_type_id.to_string());
        std::vector<core::Position> positions;
        while (statement.step_row()) {
          auto position = read_position(statement.get());
          position.accepts = load_accepts(box_type_id, position.label);
          positions.push_back(std::move(position));
        }
        return positions;
      }

      [[nodiscard]] core::BoxType read_box_type(sqlite3_stmt* statement) const {
        const auto box_type_id = core::BoxTypeId::parse(column_text(statement, 0));
        return core::BoxType{
            .id = box_type_id,
            .lab_id = core::LabId::parse(column_text(statement, 1)),
            .name = column_text(statement, 2),
            .manufacturer = column_text(statement, 3),
            .sku = column_text(statement, 4),
            .positions = load_positions(box_type_id),
            .created_at = core::Timestamp::from_unix_micros(sqlite3_column_int64(statement, 5)),
            .archived_at = column_optional_timestamp(statement, 6),
        };
      }

      static void bind_entity(sqlite3_stmt* statement, const core::BoxType& entity) {
        bind_text(statement, 1, entity.id.to_string());
        bind_text(statement, 2, entity.lab_id.to_string());
        bind_text(statement, 3, entity.name);
        bind_text(statement, 4, entity.manufacturer);
        bind_text(statement, 5, entity.sku);
        bind_int64(statement, 6, entity.created_at.unix_micros());
        bind_optional_timestamp(statement, 7, entity.archived_at);
      }

      static void insert_pending(sqlite3* handle, const core::BoxType& entity) {
        Statement statement(handle, "INSERT INTO box_types "
                                    "(id, lab_id, name, manufacturer, sku, created_at_micros, "
                                    "archived_at_micros) VALUES (?, ?, ?, ?, ?, ?, ?)");
        bind_entity(statement.get(), entity);
        statement.step_done();
      }

      static void update_pending(sqlite3* handle, const core::BoxType& entity) {
        Statement statement(handle, "UPDATE box_types SET id = ?, lab_id = ?, name = ?, "
                                    "manufacturer = ?, sku = ?, created_at_micros = ?, "
                                    "archived_at_micros = ? WHERE id = ?");
        bind_entity(statement.get(), entity);
        bind_text(statement.get(), 8, entity.id.to_string());
        statement.step_done();
      }

      static void replace_positions(sqlite3* handle, const core::BoxType& entity) {
        {
          Statement statement(handle, "DELETE FROM box_type_positions WHERE box_type_id = ?");
          bind_text(statement.get(), 1, entity.id.to_string());
          statement.step_done();
        }

        for (const auto& position : entity.positions) {
          Statement position_statement(handle,
                                       "INSERT INTO box_type_positions "
                                       "(box_type_id, label, row_index, col_index, z_index) "
                                       "VALUES (?, ?, ?, ?, ?)");
          bind_text(position_statement.get(), 1, entity.id.to_string());
          bind_text(position_statement.get(), 2, position.label);
          bind_int64(position_statement.get(), 3, position.row);
          bind_int64(position_statement.get(), 4, position.col);
          bind_optional_int64(position_statement.get(), 5, position.z);
          position_statement.step_done();

          for (const auto& size_class : position.accepts) {
            Statement accepts_statement(handle, "INSERT INTO box_type_position_accepts "
                                                "(box_type_id, position_label, size_class) "
                                                "VALUES (?, ?, ?)");
            bind_text(accepts_statement.get(), 1, entity.id.to_string());
            bind_text(accepts_statement.get(), 2, position.label);
            bind_text(accepts_statement.get(), 3, size_class);
            accepts_statement.step_done();
          }
        }
      }
    };

  } // namespace

  void register_box_geometry_repositories(SqliteBackend& backend) {
    backend.register_repository_factory<core::ContainerType>([](SqliteTransaction& transaction) {
      return std::make_unique<ContainerTypeRepository>(transaction);
    });
    backend.register_repository_factory<core::BoxType>([](SqliteTransaction& transaction) {
      return std::make_unique<BoxTypeRepository>(transaction);
    });
  }

} // namespace fmgr::storage
