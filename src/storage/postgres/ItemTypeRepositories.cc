// SPDX-License-Identifier: AGPL-3.0-or-later

#include "storage/postgres/ItemTypeRepositories.h"

#include "core/item_type.h"
#include "storage/ItemTypeTraits.h"
#include "storage/detail/ItemTypeColumns.h"
#include "storage/detail/QuerySqlBuilder.h"
#include "storage/postgres/PostgresRepoSupport.h"

#include <pqxx/pqxx>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace fmgr::storage {
  namespace {

    using detail::id_or_null;
    using detail::micros_or_null;
    using detail::pg_bind_params;
    using detail::pg_optional_id;
    using detail::pg_optional_timestamp;
    using detail::throw_pqxx_error;
    using detail::validate_cfd_shape;
    using detail::validate_item_type;

    [[nodiscard]] core::Timestamp now_timestamp() {
      const auto now =
          std::chrono::time_point_cast<std::chrono::microseconds>(std::chrono::system_clock::now());
      return core::Timestamp::from_unix_micros(now.time_since_epoch().count());
    }

    // ---- ItemType ----

    constexpr const char* item_type_columns =
        "id, lab_id, parent_id, name, created_at_micros, archived_at_micros";

    [[nodiscard]] core::ItemType read_item_type(pqxx::row_ref row) {
      return core::ItemType{
          .id = core::ItemTypeId::parse(row.at("id").as<std::string>()),
          .lab_id = core::LabId::parse(row.at("lab_id").as<std::string>()),
          .parent_id = pg_optional_id<core::ItemTypeId>(row, "parent_id"),
          .name = row.at("name").as<std::string>(),
          .created_at =
              core::Timestamp::from_unix_micros(row.at("created_at_micros").as<std::int64_t>()),
          .archived_at = pg_optional_timestamp(row, "archived_at_micros"),
      };
    }

    [[nodiscard]] pqxx::params bind_item_type(const core::ItemType& entity) {
      pqxx::params params;
      params.append(entity.id.to_string());
      params.append(entity.lab_id.to_string());
      params.append(id_or_null(entity.parent_id));
      params.append(entity.name);
      params.append(entity.created_at.unix_micros());
      params.append(micros_or_null(entity.archived_at));
      return params;
    }

    class ItemTypeRepository final : public IRepository<core::ItemType> {
    public:
      explicit ItemTypeRepository(PostgresTransaction& txn) : txn_(txn) {}

      [[nodiscard]] std::optional<core::ItemType>
      find_by_id(const core::ItemTypeId& entity_id) override {
        return load(entity_id);
      }

      [[nodiscard]] std::vector<core::ItemType>
      query(const Query<core::ItemType>& query_spec) override {
        std::string sql = std::string("SELECT ") + item_type_columns + " FROM item_types";
        std::vector<nlohmann::json> parameters;
        const auto defaults = query_spec.includes_tombstoned()
                                  ? std::vector<std::string>{}
                                  : std::vector<std::string>{"archived_at_micros IS NULL"};
        detail::PostgresDialect dialect;
        detail::append_where(sql, parameters, defaults, query_spec.predicates(),
                             detail::item_type_column_name, dialect);
        detail::append_order_limit(sql, parameters, query_spec, detail::item_type_column_name,
                                   dialect);
        try {
          const auto result = txn_.work().exec(sql, pg_bind_params(parameters));
          std::vector<core::ItemType> results;
          for (pqxx::row_ref row : result) {
            results.push_back(read_item_type(row));
          }
          return results;
        } catch (const pqxx::sql_error& err) {
          throw_pqxx_error(err);
        }
      }

      void insert(const core::ItemType& entity, const MutationContext& context) override {
        validate_item_type(entity);
        check_no_cycle(entity);
        try {
          txn_.work().exec("INSERT INTO item_types (id, lab_id, parent_id, name, "
                           "created_at_micros, archived_at_micros) VALUES ($1, $2, $3, $4, $5, $6)",
                           bind_item_type(entity));
        } catch (const pqxx::sql_error& err) {
          throw_pqxx_error(err);
        }
        txn_.note_mutation(std::string(EntityTraits<core::ItemType>::entity_name()),
                           entity.id.to_string(), context, "insert", detail::audit_after(entity));
      }

      void update(const core::ItemType& entity, const MutationContext& context) override {
        validate_item_type(entity);
        check_no_cycle(entity);
        const auto before = find_by_id(entity.id);
        write_update(entity, context, before);
      }

      void soft_delete(const core::ItemTypeId& entity_id, const MutationContext& context) override {
        auto entity = find_by_id(entity_id);
        if (!entity.has_value()) {
          throw NotFound("item type not found");
        }
        const auto before = entity;
        entity->archived_at = now_timestamp();
        // soft_delete leaves parent_id unchanged; the acyclic invariant holds.
        validate_item_type(entity.value());
        write_update(entity.value(), context, before);
      }

    private:
      void write_update(const core::ItemType& entity, const MutationContext& context,
                        const std::optional<core::ItemType>& before) {
        try {
          const auto result =
              txn_.work().exec("UPDATE item_types SET lab_id = $2, parent_id = $3, name = $4, "
                               "created_at_micros = $5, archived_at_micros = $6 WHERE id = $1",
                               bind_item_type(entity));
          if (result.affected_rows() == 0) {
            throw NotFound("item type not found");
          }
        } catch (const pqxx::sql_error& err) {
          throw_pqxx_error(err);
        }
        txn_.note_mutation(std::string(EntityTraits<core::ItemType>::entity_name()),
                           entity.id.to_string(), context, "soft_delete",
                           detail::audit_change(before, entity));
      }

      void check_no_cycle(const core::ItemType& entity) {
        if (!entity.parent_id.has_value()) {
          return;
        }
        std::set<core::ItemTypeId> visited;
        std::optional<core::ItemTypeId> cursor = entity.parent_id;
        while (cursor.has_value()) {
          // NOLINTNEXTLINE(bugprone-unchecked-optional-access): guaranteed by loop condition
          const core::ItemTypeId current = *cursor;
          if (current == entity.id) {
            throw ConstraintViolation("item type parent chain forms a cycle");
          }
          if (!visited.insert(current).second) {
            throw ConstraintViolation("item type parent chain forms a cycle");
          }
          const auto ancestor = load(current);
          if (!ancestor.has_value()) {
            return;
          }
          cursor = ancestor->parent_id;
        }
      }

      [[nodiscard]] std::optional<core::ItemType> load(const core::ItemTypeId& entity_id) {
        try {
          const auto result = txn_.work().exec(std::string("SELECT ") + item_type_columns +
                                                   " FROM item_types WHERE id = $1",
                                               pqxx::params{entity_id.to_string()});
          if (result.empty()) {
            return std::nullopt;
          }
          return read_item_type(result[0]);
        } catch (const pqxx::sql_error& err) {
          throw_pqxx_error(err);
        }
      }

      PostgresTransaction& txn_;
    };

    // ---- CustomFieldDefinition ----

    constexpr const char* cfd_columns =
        "id, lab_id, scope_kind, item_type_id, key, label, data_type, required, validation_json, "
        "indexed, is_phi, created_at_micros, archived_at_micros";

    [[nodiscard]] core::CustomFieldDefinition read_cfd(pqxx::row_ref row) {
      return core::CustomFieldDefinition{
          .id = core::CustomFieldDefinitionId::parse(row.at("id").as<std::string>()),
          .lab_id = core::LabId::parse(row.at("lab_id").as<std::string>()),
          .scope_kind = core::parse_scope_kind(row.at("scope_kind").as<std::string>()),
          .item_type_id = pg_optional_id<core::ItemTypeId>(row, "item_type_id"),
          .key = row.at("key").as<std::string>(),
          .label = row.at("label").as<std::string>(),
          .data_type = core::parse_field_data_type(row.at("data_type").as<std::string>()),
          .required = row.at("required").as<bool>(),
          .validation_json = row.at("validation_json").as<std::string>(),
          .indexed = row.at("indexed").as<bool>(),
          .is_phi = row.at("is_phi").as<bool>(),
          .created_at =
              core::Timestamp::from_unix_micros(row.at("created_at_micros").as<std::int64_t>()),
          .archived_at = pg_optional_timestamp(row, "archived_at_micros"),
      };
    }

    [[nodiscard]] pqxx::params bind_cfd(const core::CustomFieldDefinition& entity) {
      pqxx::params params;
      params.append(entity.id.to_string());
      params.append(entity.lab_id.to_string());
      params.append(std::string(core::to_string(entity.scope_kind)));
      params.append(id_or_null(entity.item_type_id));
      params.append(entity.key);
      params.append(entity.label);
      params.append(std::string(core::to_string(entity.data_type)));
      params.append(entity.required);
      params.append(entity.validation_json);
      params.append(entity.indexed);
      params.append(entity.is_phi);
      params.append(entity.created_at.unix_micros());
      params.append(micros_or_null(entity.archived_at));
      return params;
    }

    class CustomFieldDefinitionRepository final : public IRepository<core::CustomFieldDefinition> {
    public:
      explicit CustomFieldDefinitionRepository(PostgresTransaction& txn) : txn_(txn) {}

      [[nodiscard]] std::optional<core::CustomFieldDefinition>
      find_by_id(const core::CustomFieldDefinitionId& entity_id) override {
        return load(entity_id);
      }

      [[nodiscard]] std::vector<core::CustomFieldDefinition>
      query(const Query<core::CustomFieldDefinition>& query_spec) override {
        std::string sql = std::string("SELECT ") + cfd_columns + " FROM custom_field_definitions";
        std::vector<nlohmann::json> parameters;
        const auto defaults = query_spec.includes_tombstoned()
                                  ? std::vector<std::string>{}
                                  : std::vector<std::string>{"archived_at_micros IS NULL"};
        detail::PostgresDialect dialect;
        detail::append_where(sql, parameters, defaults, query_spec.predicates(),
                             detail::cfd_column_name, dialect);
        detail::append_order_limit(sql, parameters, query_spec, detail::cfd_column_name, dialect);
        try {
          const auto result = txn_.work().exec(sql, pg_bind_params(parameters));
          std::vector<core::CustomFieldDefinition> results;
          for (pqxx::row_ref row : result) {
            results.push_back(read_cfd(row));
          }
          return results;
        } catch (const pqxx::sql_error& err) {
          throw_pqxx_error(err);
        }
      }

      void insert(const core::CustomFieldDefinition& entity,
                  const MutationContext& context) override {
        validate(entity);
        try {
          txn_.work().exec(
              "INSERT INTO custom_field_definitions (id, lab_id, scope_kind, item_type_id, key, "
              "label, data_type, required, validation_json, indexed, is_phi, created_at_micros, "
              "archived_at_micros) "
              "VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9::jsonb, $10, $11, $12, $13)",
              bind_cfd(entity));
        } catch (const pqxx::sql_error& err) {
          throw_pqxx_error(err);
        }
        txn_.note_mutation(std::string(EntityTraits<core::CustomFieldDefinition>::entity_name()),
                           entity.id.to_string(), context, "insert", detail::audit_after(entity));
      }

      void update(const core::CustomFieldDefinition& entity,
                  const MutationContext& context) override {
        validate(entity);
        const auto before = find_by_id(entity.id);
        try {
          const auto result = txn_.work().exec(
              "UPDATE custom_field_definitions SET lab_id = $2, scope_kind = $3, item_type_id = "
              "$4, "
              "key = $5, label = $6, data_type = $7, required = $8, validation_json = $9::jsonb, "
              "indexed = $10, is_phi = $11, created_at_micros = $12, archived_at_micros = $13 "
              "WHERE id = $1",
              bind_cfd(entity));
          if (result.affected_rows() == 0) {
            throw NotFound("custom field definition not found");
          }
        } catch (const pqxx::sql_error& err) {
          throw_pqxx_error(err);
        }
        txn_.note_mutation(std::string(EntityTraits<core::CustomFieldDefinition>::entity_name()),
                           entity.id.to_string(), context, "update",
                           detail::audit_change(before, entity));
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
      void validate(const core::CustomFieldDefinition& cfd) {
        validate_cfd_shape(cfd);
        if (cfd.item_type_id.has_value()) {
          const auto result = txn_.work().exec(
              "SELECT 1 FROM item_types WHERE id = $1 AND lab_id = $2 AND archived_at_micros IS "
              "NULL LIMIT 1",
              pqxx::params{cfd.item_type_id->to_string(), cfd.lab_id.to_string()});
          if (result.empty()) {
            throw ConstraintViolation(
                "item_type_id does not reference a live ItemType in this lab");
          }
        }
      }

      [[nodiscard]] std::optional<core::CustomFieldDefinition>
      load(const core::CustomFieldDefinitionId& entity_id) {
        try {
          const auto result = txn_.work().exec(std::string("SELECT ") + cfd_columns +
                                                   " FROM custom_field_definitions WHERE id = $1",
                                               pqxx::params{entity_id.to_string()});
          if (result.empty()) {
            return std::nullopt;
          }
          return read_cfd(result[0]);
        } catch (const pqxx::sql_error& err) {
          throw_pqxx_error(err);
        }
      }

      PostgresTransaction& txn_;
    };

  } // namespace

  void register_item_type_repositories(PostgresBackend& backend) {
    backend.register_repository_factory<core::ItemType>(
        [](PostgresTransaction& txn) { return std::make_unique<ItemTypeRepository>(txn); });
    backend.register_repository_factory<core::CustomFieldDefinition>([](PostgresTransaction& txn) {
      return std::make_unique<CustomFieldDefinitionRepository>(txn);
    });
  }

} // namespace fmgr::storage
