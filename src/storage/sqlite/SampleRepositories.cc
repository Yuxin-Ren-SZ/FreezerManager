// SPDX-License-Identifier: AGPL-3.0-or-later

#include "storage/sqlite/SampleRepositories.h"

#include "core/sample.h"
#include "storage/SampleTraits.h"

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

    // ---- SQLite helpers ----

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
          throw_sqlite_error(result, handle_, "prepare sqlite sample statement");
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
        throw_sqlite_error(result, handle_, "step sqlite sample statement");
      }

      void step_done() const {
        const auto result = sqlite3_step(statement_);
        if (result != SQLITE_DONE) {
          throw_sqlite_error(result, handle_, "execute sqlite sample statement");
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

    void bind_optional_int64(sqlite3_stmt* statement, int index,
                             const std::optional<std::int64_t>& value) {
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

    template <typename StrongId>
    [[nodiscard]] std::optional<StrongId> column_optional_id(sqlite3_stmt* statement, int column) {
      const auto text = column_optional_text(statement, column);
      if (!text.has_value()) {
        return std::nullopt;
      }
      return StrongId::parse(text.value());
    }

    [[nodiscard]] std::optional<std::int64_t> column_optional_int64(sqlite3_stmt* statement,
                                                                    int column) {
      if (sqlite3_column_type(statement, column) == SQLITE_NULL) {
        return std::nullopt;
      }
      return sqlite3_column_int64(statement, column);
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

    [[nodiscard]] std::string json_path(const std::vector<std::string>& segments) {
      std::string path = "$";
      for (const auto& segment : segments) {
        path += ".";
        path += segment;
      }
      return path;
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

    void bind_parameters(sqlite3_stmt* statement, const std::vector<nlohmann::json>& parameters) {
      int index = 1;
      for (const auto& parameter : parameters) {
        bind_json_parameter(statement, index, parameter);
        ++index;
      }
    }

    // ---- Query tail builder (shared by all entity repos) ----

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
          for (std::size_t i = 0; i < predicate.values.size(); ++i) {
            if (i != 0) {
              clause += ", ";
            }
            clause += "?";
            parameters.push_back(predicate.values.at(i));
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
        for (std::size_t i = 0; i < clauses.size(); ++i) {
          if (i != 0) {
            sql += " AND ";
          }
          sql += clauses.at(i);
        }
      }
    }

    template <typename Entity, typename ColumnName>
    void append_query_tail(std::string& sql, std::vector<nlohmann::json>& parameters,
                           const Query<Entity>& query_spec, ColumnName column_name) {
      if (!query_spec.sorts().empty()) {
        sql += " ORDER BY ";
        for (std::size_t i = 0; i < query_spec.sorts().size(); ++i) {
          if (i != 0) {
            sql += ", ";
          }
          const auto sort = query_spec.sorts().at(i);
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

    // ---- Base template for Sample and Project ----

    template <typename Entity> class SqliteSampleRepositoryBase : public IRepository<Entity> {
    public:
      explicit SqliteSampleRepositoryBase(SqliteTransaction& transaction)
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

    // ---- Sample column mapping ----

    [[nodiscard]] std::string sample_column_name(core::Sample::Field field) {
      switch (field) {
      case core::Sample::Field::Id:
        return "id";
      case core::Sample::Field::LabId:
        return "lab_id";
      case core::Sample::Field::ItemTypeId:
        return "item_type_id";
      case core::Sample::Field::Name:
        return "name";
      case core::Sample::Field::Barcode:
        return "barcode";
      case core::Sample::Field::ContainerTypeId:
        return "container_type_id";
      case core::Sample::Field::BoxId:
        return "box_id";
      case core::Sample::Field::PositionLabel:
        return "position_label";
      case core::Sample::Field::VolumeValue:
        return "volume_value";
      case core::Sample::Field::VolumeUnit:
        return "volume_unit";
      case core::Sample::Field::MassValue:
        return "mass_value";
      case core::Sample::Field::MassUnit:
        return "mass_unit";
      case core::Sample::Field::Status:
        return "status";
      case core::Sample::Field::ParentSampleId:
        return "parent_sample_id";
      case core::Sample::Field::CreatedBy:
        return "created_by";
      case core::Sample::Field::CreatedAt:
        return "created_at_micros";
      case core::Sample::Field::LastModifiedBy:
        return "last_modified_by";
      case core::Sample::Field::LastModifiedAt:
        return "last_modified_at_micros";
      case core::Sample::Field::CustomFieldsJson:
        return "custom_fields_json";
      case core::Sample::Field::PhiFieldsEncJson:
        return "phi_fields_enc_json";
      }
      throw ConstraintViolation("unknown sample field");
    }

    constexpr std::string_view k_sample_columns =
        "id, lab_id, item_type_id, name, barcode, container_type_id, box_id, position_label, "
        "volume_value, volume_unit, mass_value, mass_unit, status, parent_sample_id, "
        "created_by, created_at_micros, last_modified_by, last_modified_at_micros, "
        "custom_fields_json, phi_fields_enc_json";

    [[nodiscard]] core::Sample read_sample(sqlite3_stmt* statement) {
      return core::Sample{
          .id = core::SampleId::parse(column_text(statement, 0)),
          .lab_id = core::LabId::parse(column_text(statement, 1)),
          .item_type_id = core::ItemTypeId::parse(column_text(statement, 2)),
          .name = column_text(statement, 3),
          .barcode = column_optional_text(statement, 4),
          .container_type_id = column_optional_id<core::ContainerTypeId>(statement, 5),
          .box_id = column_optional_id<core::BoxId>(statement, 6),
          .position_label = column_optional_text(statement, 7),
          .volume_value = column_optional_int64(statement, 8),
          .volume_unit = [&]() -> std::optional<core::VolumeUnit> {
            const auto text = column_optional_text(statement, 9);
            if (!text.has_value()) {
              return std::nullopt;
            }
            return core::parse_volume_unit(text.value());
          }(),
          .mass_value = column_optional_int64(statement, 10),
          .mass_unit = [&]() -> std::optional<core::MassUnit> {
            const auto text = column_optional_text(statement, 11);
            if (!text.has_value()) {
              return std::nullopt;
            }
            return core::parse_mass_unit(text.value());
          }(),
          .status = core::parse_sample_status(column_text(statement, 12)),
          .parent_sample_id = column_optional_id<core::SampleId>(statement, 13),
          .created_by = core::UserId::parse(column_text(statement, 14)),
          .created_at = core::Timestamp::from_unix_micros(sqlite3_column_int64(statement, 15)),
          .last_modified_by = core::UserId::parse(column_text(statement, 16)),
          .last_modified_at =
              core::Timestamp::from_unix_micros(sqlite3_column_int64(statement, 17)),
          .custom_fields_json = column_text(statement, 18),
          .phi_fields_enc_json = column_text(statement, 19),
      };
    }

    // ---- Sample validation ----

    void validate_sample(const core::Sample& sample, const SqliteTransaction& transaction) {
      if (sample.name.empty()) {
        throw ConstraintViolation("sample name is required");
      }
      // Position-pair invariant (also enforced by DB CHECK).
      if (sample.box_id.has_value() != sample.position_label.has_value()) {
        throw ConstraintViolation("box_id and position_label must both be set or both null");
      }
      if (sample.box_id.has_value()) {
        // Box must exist in same lab and not be archived.
        {
          Statement stmt(transaction.handle(),
                         "SELECT 1 FROM boxes "
                         "WHERE id = ? AND lab_id = ? AND archived_at_micros IS NULL LIMIT 1");
          bind_text(stmt.get(), 1, sample.box_id->to_string());
          bind_text(stmt.get(), 2, sample.lab_id.to_string());
          if (!stmt.step_row()) {
            throw ConstraintViolation("box_id does not reference a live Box in this lab");
          }
        }
        // Position label must exist in the box's BoxType.
        {
          Statement stmt(transaction.handle(), "SELECT 1 FROM box_type_positions btp "
                                               "JOIN boxes b ON b.box_type_id = btp.box_type_id "
                                               "WHERE b.id = ? AND btp.label = ? LIMIT 1");
          bind_text(stmt.get(), 1, sample.box_id->to_string());
          bind_text(stmt.get(), 2, sample.position_label.value());
          if (!stmt.step_row()) {
            throw ConstraintViolation("position_label does not exist in this box's BoxType");
          }
        }
        // If a container type is given, its size_class must be accepted at the position.
        // box_type_position_accepts uses composite key (box_type_id, position_label).
        if (sample.container_type_id.has_value()) {
          Statement stmt(transaction.handle(),
                         "SELECT 1 FROM box_type_position_accepts bpa "
                         "JOIN boxes b ON b.box_type_id = bpa.box_type_id "
                         "JOIN container_types ct ON ct.size_class = bpa.size_class "
                         "WHERE b.id = ? AND bpa.position_label = ? AND ct.id = ? LIMIT 1");
          bind_text(stmt.get(), 1, sample.box_id->to_string());
          bind_text(stmt.get(), 2, sample.position_label.value());
          bind_text(stmt.get(), 3, sample.container_type_id->to_string());
          if (!stmt.step_row()) {
            throw ConstraintViolation(
                "container_type size_class is not accepted at this box position");
          }
        }
      }
      // item_type_id must reference a live ItemType in same lab.
      {
        Statement stmt(transaction.handle(),
                       "SELECT 1 FROM item_types "
                       "WHERE id = ? AND lab_id = ? AND archived_at_micros IS NULL LIMIT 1");
        bind_text(stmt.get(), 1, sample.item_type_id.to_string());
        bind_text(stmt.get(), 2, sample.lab_id.to_string());
        if (!stmt.step_row()) {
          throw ConstraintViolation("item_type_id does not reference a live ItemType in this lab");
        }
      }
    }

    // ---- SampleRepository ----

    class SampleRepository final : public SqliteSampleRepositoryBase<core::Sample> {
    public:
      using SqliteSampleRepositoryBase::SqliteSampleRepositoryBase;

      [[nodiscard]] std::optional<core::Sample>
      find_by_id(const core::SampleId& entity_id) override {
        if (auto staged = find_staged(entity_id); staged.has_value()) {
          return staged;
        }
        return load(entity_id);
      }

      [[nodiscard]] std::vector<core::Sample>
      query(const Query<core::Sample>& query_spec) override {
        std::string sql = "SELECT ";
        sql += k_sample_columns;
        sql += " FROM samples";
        std::vector<nlohmann::json> parameters;
        // Sample tombstone uses status != 'tombstoned', not archived_at_micros IS NULL.
        const auto defaults = query_spec.includes_tombstoned()
                                  ? std::vector<std::string>{}
                                  : std::vector<std::string>{"status != 'tombstoned'"};
        append_generic_predicates(sql, parameters, defaults, query_spec.predicates(),
                                  sample_column_name);
        append_query_tail(sql, parameters, query_spec, sample_column_name);

        Statement statement(transaction().handle(), sql);
        bind_parameters(statement.get(), parameters);

        std::vector<core::Sample> results;
        while (statement.step_row()) {
          results.push_back(read_sample(statement.get()));
        }
        return results;
      }

      void insert(const core::Sample& entity, const MutationContext& context) override {
        stage_insert(entity, context);
      }

      void update(const core::Sample& entity, const MutationContext& context) override {
        stage_update(entity, context);
      }

      void soft_delete(const core::SampleId& entity_id, const MutationContext& context) override {
        auto entity = find_by_id(entity_id);
        if (!entity.has_value()) {
          throw NotFound("sample not found");
        }
        const auto now = now_timestamp();
        entity->status = core::SampleStatus::Tombstoned;
        entity->last_modified_by = context.actor_user_id;
        entity->last_modified_at = now;
        // Bypass validate() since tombstone does not change structural fields.
        const auto entity_id2 = entity->id;
        const auto is_insert = pending().contains(entity_id2) && pending().at(entity_id2).is_insert;
        pending().insert_or_assign(entity_id2,
                                   PendingEntity{.entity = entity.value(), .is_insert = is_insert});
        transaction().note_mutation(std::string(EntityTraits<core::Sample>::entity_name()),
                                    entity_id2.to_string(), context);
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

      [[nodiscard]] std::optional<core::Sample>
      load(const core::SampleId& entity_id) const override {
        std::string sql = "SELECT ";
        sql += k_sample_columns;
        sql += " FROM samples WHERE id = ?";
        Statement statement(transaction().handle(), sql);
        bind_text(statement.get(), 1, entity_id.to_string());
        if (!statement.step_row()) {
          return std::nullopt;
        }
        return read_sample(statement.get());
      }

      void validate(const core::Sample& entity) const override {
        validate_sample(entity, transaction());
      }

      [[nodiscard]] core::SampleId id_of(const core::Sample& entity) const override {
        return entity.id;
      }

      static void bind_entity(sqlite3_stmt* statement, const core::Sample& entity) {
        bind_text(statement, 1, entity.id.to_string());
        bind_text(statement, 2, entity.lab_id.to_string());
        bind_text(statement, 3, entity.item_type_id.to_string());
        bind_text(statement, 4, entity.name);
        bind_optional_text(statement, 5, entity.barcode);
        bind_optional_id(statement, 6, entity.container_type_id);
        bind_optional_id(statement, 7, entity.box_id);
        bind_optional_text(statement, 8, entity.position_label);
        bind_optional_int64(statement, 9, entity.volume_value);
        if (entity.volume_unit.has_value()) {
          bind_text(statement, 10, std::string(core::to_string(entity.volume_unit.value())));
        } else {
          bind_null(statement, 10);
        }
        bind_optional_int64(statement, 11, entity.mass_value);
        if (entity.mass_unit.has_value()) {
          bind_text(statement, 12, std::string(core::to_string(entity.mass_unit.value())));
        } else {
          bind_null(statement, 12);
        }
        bind_text(statement, 13, std::string(core::to_string(entity.status)));
        bind_optional_id(statement, 14, entity.parent_sample_id);
        bind_text(statement, 15, entity.created_by.to_string());
        bind_int64(statement, 16, entity.created_at.unix_micros());
        bind_text(statement, 17, entity.last_modified_by.to_string());
        bind_int64(statement, 18, entity.last_modified_at.unix_micros());
        bind_text(statement, 19, entity.custom_fields_json);
        bind_text(statement, 20, entity.phi_fields_enc_json);
      }

      static void insert_pending(sqlite3* handle, const core::Sample& entity) {
        Statement statement(handle,
                            "INSERT INTO samples "
                            "(id, lab_id, item_type_id, name, barcode, container_type_id, "
                            "box_id, position_label, volume_value, volume_unit, "
                            "mass_value, mass_unit, status, parent_sample_id, "
                            "created_by, created_at_micros, last_modified_by, "
                            "last_modified_at_micros, custom_fields_json, phi_fields_enc_json) "
                            "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
        bind_entity(statement.get(), entity);
        statement.step_done();
      }

      static void update_pending(sqlite3* handle, const core::Sample& entity) {
        Statement statement(
            handle, "UPDATE samples SET "
                    "id = ?, lab_id = ?, item_type_id = ?, name = ?, barcode = ?, "
                    "container_type_id = ?, box_id = ?, position_label = ?, "
                    "volume_value = ?, volume_unit = ?, mass_value = ?, mass_unit = ?, "
                    "status = ?, parent_sample_id = ?, created_by = ?, created_at_micros = ?, "
                    "last_modified_by = ?, last_modified_at_micros = ?, "
                    "custom_fields_json = ?, phi_fields_enc_json = ? WHERE id = ?");
        bind_entity(statement.get(), entity);
        bind_text(statement.get(), 21, entity.id.to_string());
        statement.step_done();
      }
    };

    // ---- Project column mapping ----

    [[nodiscard]] std::string project_column_name(core::Project::Field field) {
      switch (field) {
      case core::Project::Field::Id:
        return "id";
      case core::Project::Field::LabId:
        return "lab_id";
      case core::Project::Field::Name:
        return "name";
      case core::Project::Field::OwnerUserId:
        return "owner_user_id";
      case core::Project::Field::CreatedAt:
        return "created_at_micros";
      case core::Project::Field::ArchivedAt:
        return "archived_at_micros";
      }
      throw ConstraintViolation("unknown project field");
    }

    constexpr std::string_view k_project_columns =
        "id, lab_id, name, owner_user_id, created_at_micros, archived_at_micros";

    [[nodiscard]] core::Project read_project(sqlite3_stmt* statement) {
      return core::Project{
          .id = core::ProjectId::parse(column_text(statement, 0)),
          .lab_id = core::LabId::parse(column_text(statement, 1)),
          .name = column_text(statement, 2),
          .owner_user_id = core::UserId::parse(column_text(statement, 3)),
          .created_at = core::Timestamp::from_unix_micros(sqlite3_column_int64(statement, 4)),
          .archived_at = column_optional_timestamp(statement, 5),
      };
    }

    // ---- ProjectRepository ----

    class ProjectRepository final : public SqliteSampleRepositoryBase<core::Project> {
    public:
      using SqliteSampleRepositoryBase::SqliteSampleRepositoryBase;

      [[nodiscard]] std::optional<core::Project>
      find_by_id(const core::ProjectId& entity_id) override {
        if (auto staged = find_staged(entity_id); staged.has_value()) {
          return staged;
        }
        return load(entity_id);
      }

      [[nodiscard]] std::vector<core::Project>
      query(const Query<core::Project>& query_spec) override {
        std::string sql = "SELECT ";
        sql += k_project_columns;
        sql += " FROM projects";
        std::vector<nlohmann::json> parameters;
        const auto defaults = query_spec.includes_tombstoned()
                                  ? std::vector<std::string>{}
                                  : std::vector<std::string>{"archived_at_micros IS NULL"};
        append_generic_predicates(sql, parameters, defaults, query_spec.predicates(),
                                  project_column_name);
        append_query_tail(sql, parameters, query_spec, project_column_name);

        Statement statement(transaction().handle(), sql);
        bind_parameters(statement.get(), parameters);

        std::vector<core::Project> results;
        while (statement.step_row()) {
          results.push_back(read_project(statement.get()));
        }
        return results;
      }

      void insert(const core::Project& entity, const MutationContext& context) override {
        stage_insert(entity, context);
      }

      void update(const core::Project& entity, const MutationContext& context) override {
        stage_update(entity, context);
      }

      void soft_delete(const core::ProjectId& entity_id, const MutationContext& context) override {
        auto entity = find_by_id(entity_id);
        if (!entity.has_value()) {
          throw NotFound("project not found");
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

      [[nodiscard]] std::optional<core::Project>
      load(const core::ProjectId& entity_id) const override {
        std::string sql = "SELECT ";
        sql += k_project_columns;
        sql += " FROM projects WHERE id = ?";
        Statement statement(transaction().handle(), sql);
        bind_text(statement.get(), 1, entity_id.to_string());
        if (!statement.step_row()) {
          return std::nullopt;
        }
        return read_project(statement.get());
      }

      void validate(const core::Project& entity) const override {
        if (entity.name.empty()) {
          throw ConstraintViolation("project name is required");
        }
      }

      [[nodiscard]] core::ProjectId id_of(const core::Project& entity) const override {
        return entity.id;
      }

      static void bind_entity(sqlite3_stmt* statement, const core::Project& entity) {
        bind_text(statement, 1, entity.id.to_string());
        bind_text(statement, 2, entity.lab_id.to_string());
        bind_text(statement, 3, entity.name);
        bind_text(statement, 4, entity.owner_user_id.to_string());
        bind_int64(statement, 5, entity.created_at.unix_micros());
        bind_optional_timestamp(statement, 6, entity.archived_at);
      }

      static void insert_pending(sqlite3* handle, const core::Project& entity) {
        Statement statement(handle, "INSERT INTO projects "
                                    "(id, lab_id, name, owner_user_id, "
                                    "created_at_micros, archived_at_micros) "
                                    "VALUES (?, ?, ?, ?, ?, ?)");
        bind_entity(statement.get(), entity);
        statement.step_done();
      }

      static void update_pending(sqlite3* handle, const core::Project& entity) {
        Statement statement(handle, "UPDATE projects SET "
                                    "id = ?, lab_id = ?, name = ?, owner_user_id = ?, "
                                    "created_at_micros = ?, archived_at_micros = ? WHERE id = ?");
        bind_entity(statement.get(), entity);
        bind_text(statement.get(), 7, entity.id.to_string());
        statement.step_done();
      }
    };

    // ---- SampleProject column mapping ----

    [[nodiscard]] std::string sp_column_name(core::SampleProject::Field field) {
      switch (field) {
      case core::SampleProject::Field::SampleId:
        return "sample_id";
      case core::SampleProject::Field::ProjectId:
        return "project_id";
      }
      throw ConstraintViolation("unknown sample_project field");
    }

    constexpr std::string_view k_sp_columns = "sample_id, project_id";

    [[nodiscard]] core::SampleProject read_sp(sqlite3_stmt* statement) {
      return core::SampleProject{
          .sample_id = core::SampleId::parse(column_text(statement, 0)),
          .project_id = core::ProjectId::parse(column_text(statement, 1)),
      };
    }

    // ---- SampleProjectRepository ----
    // Uses an explicit pending-inserts map and pending-deletes set because
    // the composite key cannot use the base template's to_string() protocol.

    class SampleProjectRepository final : public IRepository<core::SampleProject> {
    public:
      explicit SampleProjectRepository(SqliteTransaction& transaction) : transaction_(transaction) {
        transaction_.add_commit_hook([this](sqlite3* handle) { flush(handle); });
      }

      [[nodiscard]] std::optional<core::SampleProject>
      find_by_id(const core::SampleProjectId& entity_id) override {
        if (pending_deletes_.contains(entity_id)) {
          return std::nullopt;
        }
        const auto iter = pending_inserts_.find(entity_id);
        if (iter != pending_inserts_.end()) {
          return iter->second;
        }
        return load(entity_id);
      }

      [[nodiscard]] std::vector<core::SampleProject>
      query(const Query<core::SampleProject>& query_spec) override {
        std::string sql = "SELECT ";
        sql += k_sp_columns;
        sql += " FROM sample_projects";
        std::vector<nlohmann::json> parameters;
        // SampleProject has no tombstone; includes_tombstoned() is irrelevant.
        append_generic_predicates(sql, parameters, {}, query_spec.predicates(), sp_column_name);
        append_query_tail(sql, parameters, query_spec, sp_column_name);

        Statement statement(transaction_.handle(), sql);
        bind_parameters(statement.get(), parameters);

        std::vector<core::SampleProject> results;
        while (statement.step_row()) {
          auto sp = read_sp(statement.get());
          if (!pending_deletes_.contains(sp.id())) {
            results.push_back(sp);
          }
        }
        // Append in-memory inserts not yet in the DB.
        for (const auto& [sp_id, sp] : pending_inserts_) {
          results.push_back(sp);
        }
        return results;
      }

      void insert(const core::SampleProject& entity, const MutationContext& context) override {
        const auto sp_id = entity.id();
        if (pending_inserts_.contains(sp_id) || load(sp_id).has_value()) {
          throw UniqueViolation("sample_project link already exists");
        }
        pending_inserts_.insert_or_assign(sp_id, entity);
        transaction_.note_mutation(
            "sample_project", entity.sample_id.to_string() + ":" + entity.project_id.to_string(),
            context);
      }

      void update(const core::SampleProject& /*entity*/,
                  const MutationContext& /*context*/) override {
        throw UnsupportedOperation("SampleProject links cannot be updated; remove and re-add");
      }

      void soft_delete(const core::SampleProjectId& entity_id,
                       const MutationContext& context) override {
        if (pending_deletes_.contains(entity_id)) {
          throw NotFound("sample_project link not found");
        }
        const bool in_pending = pending_inserts_.contains(entity_id);
        const bool in_db = !in_pending && load(entity_id).has_value();
        if (!in_pending && !in_db) {
          throw NotFound("sample_project link not found");
        }
        pending_inserts_.erase(entity_id);
        if (in_db) {
          pending_deletes_.insert(entity_id);
          transaction_.note_mutation(
              "sample_project",
              entity_id.sample_id.to_string() + ":" + entity_id.project_id.to_string(), context);
        }
      }

    private:
      [[nodiscard]] std::optional<core::SampleProject>
      load(const core::SampleProjectId& entity_id) const {
        Statement statement(transaction_.handle(),
                            "SELECT sample_id, project_id FROM sample_projects "
                            "WHERE sample_id = ? AND project_id = ? LIMIT 1");
        bind_text(statement.get(), 1, entity_id.sample_id.to_string());
        bind_text(statement.get(), 2, entity_id.project_id.to_string());
        if (!statement.step_row()) {
          return std::nullopt;
        }
        return read_sp(statement.get());
      }

      void flush(sqlite3* handle) {
        for (const auto& sp_id : pending_deletes_) {
          Statement statement(handle, "DELETE FROM sample_projects "
                                      "WHERE sample_id = ? AND project_id = ?");
          bind_text(statement.get(), 1, sp_id.sample_id.to_string());
          bind_text(statement.get(), 2, sp_id.project_id.to_string());
          statement.step_done();
        }
        for (const auto& [unused_id, sp] : pending_inserts_) {
          (void)unused_id;
          Statement statement(handle, "INSERT INTO sample_projects (sample_id, project_id) "
                                      "VALUES (?, ?)");
          bind_text(statement.get(), 1, sp.sample_id.to_string());
          bind_text(statement.get(), 2, sp.project_id.to_string());
          statement.step_done();
        }
      }

      SqliteTransaction& transaction_;
      std::map<core::SampleProjectId, core::SampleProject> pending_inserts_;
      std::set<core::SampleProjectId> pending_deletes_;
    };

    // ---- CheckoutEvent column mapping ----

    [[nodiscard]] std::string event_column_name(core::CheckoutEvent::Field field) {
      switch (field) {
      case core::CheckoutEvent::Field::Id:
        return "id";
      case core::CheckoutEvent::Field::SampleId:
        return "sample_id";
      case core::CheckoutEvent::Field::LabId:
        return "lab_id";
      case core::CheckoutEvent::Field::UserId:
        return "user_id";
      case core::CheckoutEvent::Field::Action:
        return "action";
      case core::CheckoutEvent::Field::Reason:
        return "reason";
      case core::CheckoutEvent::Field::At:
        return "at_micros";
      case core::CheckoutEvent::Field::VolumeDelta:
        return "volume_delta";
      case core::CheckoutEvent::Field::VolumeUnit:
        return "volume_unit";
      case core::CheckoutEvent::Field::LocationAfter:
        return "location_after";
      }
      throw ConstraintViolation("unknown checkout_event field");
    }

    constexpr std::string_view k_event_columns =
        "id, sample_id, lab_id, user_id, action, reason, at_micros, "
        "volume_delta, volume_unit, location_after";

    [[nodiscard]] core::CheckoutEvent read_event(sqlite3_stmt* statement) {
      return core::CheckoutEvent{
          .id = core::CheckoutEventId::parse(column_text(statement, 0)),
          .sample_id = core::SampleId::parse(column_text(statement, 1)),
          .lab_id = core::LabId::parse(column_text(statement, 2)),
          .user_id = core::UserId::parse(column_text(statement, 3)),
          .action = core::parse_checkout_action(column_text(statement, 4)),
          .reason = column_optional_text(statement, 5),
          .at = core::Timestamp::from_unix_micros(sqlite3_column_int64(statement, 6)),
          .volume_delta = column_optional_int64(statement, 7),
          .volume_unit = [&]() -> std::optional<core::VolumeUnit> {
            const auto text = column_optional_text(statement, 8);
            if (!text.has_value()) {
              return std::nullopt;
            }
            return core::parse_volume_unit(text.value());
          }(),
          .location_after = column_optional_text(statement, 9),
      };
    }

    // ---- CheckoutEventRepository ----
    // Append-only: update() and soft_delete() throw UnsupportedOperation.

    class CheckoutEventRepository final : public IRepository<core::CheckoutEvent> {
    public:
      explicit CheckoutEventRepository(SqliteTransaction& transaction) : transaction_(transaction) {
        transaction_.add_commit_hook([this](sqlite3* handle) { flush(handle); });
      }

      [[nodiscard]] std::optional<core::CheckoutEvent>
      find_by_id(const core::CheckoutEventId& entity_id) override {
        const auto iter = pending_.find(entity_id);
        if (iter != pending_.end()) {
          return iter->second;
        }
        return load(entity_id);
      }

      [[nodiscard]] std::vector<core::CheckoutEvent>
      query(const Query<core::CheckoutEvent>& query_spec) override {
        std::string sql = "SELECT ";
        sql += k_event_columns;
        sql += " FROM checkout_events";
        std::vector<nlohmann::json> parameters;
        // CheckoutEvent has no tombstone; includes_tombstoned() is irrelevant.
        append_generic_predicates(sql, parameters, {}, query_spec.predicates(), event_column_name);
        append_query_tail(sql, parameters, query_spec, event_column_name);

        Statement statement(transaction_.handle(), sql);
        bind_parameters(statement.get(), parameters);

        std::vector<core::CheckoutEvent> results;
        while (statement.step_row()) {
          results.push_back(read_event(statement.get()));
        }
        // Append in-memory inserts not yet flushed.
        for (const auto& [unused_id, event] : pending_) {
          results.push_back(event);
        }
        return results;
      }

      void insert(const core::CheckoutEvent& entity, const MutationContext& context) override {
        if (pending_.contains(entity.id) || load(entity.id).has_value()) {
          throw UniqueViolation("checkout_event id already exists");
        }
        pending_.insert_or_assign(entity.id, entity);
        transaction_.note_mutation("checkout_event", entity.id.to_string(), context);
      }

      void update(const core::CheckoutEvent& /*entity*/,
                  const MutationContext& /*context*/) override {
        throw UnsupportedOperation("CheckoutEvent rows are immutable (append-only audit)");
      }

      void soft_delete(const core::CheckoutEventId& /*entity_id*/,
                       const MutationContext& /*context*/) override {
        throw UnsupportedOperation("CheckoutEvent rows are immutable (append-only audit)");
      }

    private:
      [[nodiscard]] std::optional<core::CheckoutEvent>
      load(const core::CheckoutEventId& entity_id) const {
        std::string sql = "SELECT ";
        sql += k_event_columns;
        sql += " FROM checkout_events WHERE id = ?";
        Statement statement(transaction_.handle(), sql);
        bind_text(statement.get(), 1, entity_id.to_string());
        if (!statement.step_row()) {
          return std::nullopt;
        }
        return read_event(statement.get());
      }

      void flush(sqlite3* handle) {
        for (const auto& [unused_id, event] : pending_) {
          (void)unused_id;
          Statement statement(handle, "INSERT INTO checkout_events "
                                      "(id, sample_id, lab_id, user_id, action, reason, at_micros, "
                                      "volume_delta, volume_unit, location_after) "
                                      "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
          bind_text(statement.get(), 1, event.id.to_string());
          bind_text(statement.get(), 2, event.sample_id.to_string());
          bind_text(statement.get(), 3, event.lab_id.to_string());
          bind_text(statement.get(), 4, event.user_id.to_string());
          bind_text(statement.get(), 5, std::string(core::to_string(event.action)));
          bind_optional_text(statement.get(), 6, event.reason);
          bind_int64(statement.get(), 7, event.at.unix_micros());
          bind_optional_int64(statement.get(), 8, event.volume_delta);
          if (event.volume_unit.has_value()) {
            bind_text(statement.get(), 9, std::string(core::to_string(event.volume_unit.value())));
          } else {
            bind_null(statement.get(), 9);
          }
          bind_optional_text(statement.get(), 10, event.location_after);
          statement.step_done();
        }
      }

      SqliteTransaction& transaction_;
      std::map<core::CheckoutEventId, core::CheckoutEvent> pending_;
    };

  } // namespace

  void register_sample_repositories(SqliteBackend& backend) {
    backend.register_repository_factory<core::Sample>([](SqliteTransaction& transaction) {
      return std::make_unique<SampleRepository>(transaction);
    });
    backend.register_repository_factory<core::Project>([](SqliteTransaction& transaction) {
      return std::make_unique<ProjectRepository>(transaction);
    });
    backend.register_repository_factory<core::SampleProject>([](SqliteTransaction& transaction) {
      return std::make_unique<SampleProjectRepository>(transaction);
    });
    backend.register_repository_factory<core::CheckoutEvent>([](SqliteTransaction& transaction) {
      return std::make_unique<CheckoutEventRepository>(transaction);
    });
  }

} // namespace fmgr::storage
