// SPDX-License-Identifier: AGPL-3.0-or-later

#include "storage/postgres/LayoutRepositories.h"

#include "core/freezer.h"
#include "storage/FreezerTraits.h"
#include "storage/detail/LayoutColumns.h"
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

    using detail::micros_or_null;
    using detail::pg_bind_params;
    using detail::pg_optional_id;
    using detail::pg_optional_timestamp;
    using detail::throw_pqxx_error;
    using detail::validate_freezer;
    using detail::validate_storage_container;

    [[nodiscard]] core::Timestamp now_timestamp() {
      const auto now =
          std::chrono::time_point_cast<std::chrono::microseconds>(std::chrono::system_clock::now());
      return core::Timestamp::from_unix_micros(now.time_since_epoch().count());
    }

    [[nodiscard]] std::optional<double> pg_optional_double(pqxx::row_ref row, const char* column) {
      const auto field = row.at(column);
      if (field.is_null()) {
        return std::nullopt;
      }
      return field.as<double>();
    }

    // ---- Freezer ----

    constexpr const char* freezer_columns = "id, lab_id, name, location, model, temp_target_c, "
                                            "layout_root_id, created_at_micros, archived_at_micros";

    [[nodiscard]] core::Freezer read_freezer(pqxx::row_ref row) {
      return core::Freezer{
          .id = core::FreezerId::parse(row.at("id").as<std::string>()),
          .lab_id = core::LabId::parse(row.at("lab_id").as<std::string>()),
          .name = row.at("name").as<std::string>(),
          .location = row.at("location").as<std::string>(),
          .model = row.at("model").as<std::string>(),
          .temp_target_c = pg_optional_double(row, "temp_target_c"),
          .layout_root_id =
              core::StorageContainerId::parse(row.at("layout_root_id").as<std::string>()),
          .created_at =
              core::Timestamp::from_unix_micros(row.at("created_at_micros").as<std::int64_t>()),
          .archived_at = pg_optional_timestamp(row, "archived_at_micros"),
      };
    }

    [[nodiscard]] pqxx::params bind_freezer(const core::Freezer& entity) {
      pqxx::params params;
      params.append(entity.id.to_string());
      params.append(entity.lab_id.to_string());
      params.append(entity.name);
      params.append(entity.location);
      params.append(entity.model);
      params.append(entity.temp_target_c); // optional<double> binds NULL if empty
      params.append(entity.layout_root_id.to_string());
      params.append(entity.created_at.unix_micros());
      params.append(micros_or_null(entity.archived_at));
      return params;
    }

    class FreezerRepository final : public IRepository<core::Freezer> {
    public:
      explicit FreezerRepository(PostgresTransaction& txn) : txn_(txn) {}

      [[nodiscard]] std::optional<core::Freezer>
      find_by_id(const core::FreezerId& entity_id) override {
        try {
          const auto result = txn_.work().exec(std::string("SELECT ") + freezer_columns +
                                                   " FROM freezers WHERE id = $1",
                                               pqxx::params{entity_id.to_string()});
          if (result.empty()) {
            return std::nullopt;
          }
          return read_freezer(result[0]);
        } catch (const pqxx::sql_error& err) {
          throw_pqxx_error(err);
        }
      }

      [[nodiscard]] std::vector<core::Freezer>
      query(const Query<core::Freezer>& query_spec) override {
        std::string sql = std::string("SELECT ") + freezer_columns + " FROM freezers";
        std::vector<nlohmann::json> parameters;
        const auto defaults = query_spec.includes_tombstoned()
                                  ? std::vector<std::string>{}
                                  : std::vector<std::string>{"archived_at_micros IS NULL"};
        detail::PostgresDialect dialect;
        detail::append_where(sql, parameters, defaults, query_spec.predicates(),
                             detail::freezer_column_name, dialect);
        detail::append_order_limit(sql, parameters, query_spec, detail::freezer_column_name,
                                   dialect);
        try {
          const auto result = txn_.work().exec(sql, pg_bind_params(parameters));
          std::vector<core::Freezer> results;
          for (pqxx::row_ref row : result) {
            results.push_back(read_freezer(row));
          }
          return results;
        } catch (const pqxx::sql_error& err) {
          throw_pqxx_error(err);
        }
      }

      void insert(const core::Freezer& entity, const MutationContext& context) override {
        validate_freezer(entity);
        try {
          txn_.work().exec(
              "INSERT INTO freezers (id, lab_id, name, location, model, temp_target_c, "
              "layout_root_id, created_at_micros, archived_at_micros) "
              "VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9)",
              bind_freezer(entity));
        } catch (const pqxx::sql_error& err) {
          throw_pqxx_error(err);
        }
        txn_.note_mutation(std::string(EntityTraits<core::Freezer>::entity_name()),
                           entity.id.to_string(), context, "insert", detail::audit_after(entity));
      }

      void update(const core::Freezer& entity, const MutationContext& context) override {
        validate_freezer(entity);
        try {
          const auto result = txn_.work().exec(
              "UPDATE freezers SET lab_id = $2, name = $3, location = $4, model = $5, "
              "temp_target_c = $6, layout_root_id = $7, created_at_micros = $8, "
              "archived_at_micros = $9 WHERE id = $1",
              bind_freezer(entity));
          if (result.affected_rows() == 0) {
            throw NotFound("freezer not found");
          }
        } catch (const pqxx::sql_error& err) {
          throw_pqxx_error(err);
        }
        txn_.note_mutation(std::string(EntityTraits<core::Freezer>::entity_name()),
                           entity.id.to_string(), context, "update", detail::audit_after(entity));
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
      PostgresTransaction& txn_;
    };

    // ---- StorageContainer ----

    constexpr const char* container_columns =
        "id, lab_id, parent_id, kind, name, label, ordering_index, capacity_hint_json, "
        "created_at_micros, archived_at_micros";

    [[nodiscard]] std::string capacity_hint_dump(const std::optional<core::CapacityHint>& hint) {
      if (!hint.has_value()) {
        return "null";
      }
      nlohmann::json json = hint.value();
      return json.dump();
    }

    [[nodiscard]] core::StorageContainer read_container(pqxx::row_ref row) {
      std::optional<core::CapacityHint> capacity_hint;
      const auto capacity_json =
          nlohmann::json::parse(row.at("capacity_hint_json").as<std::string>());
      if (!capacity_json.is_null()) {
        capacity_hint = capacity_json.get<core::CapacityHint>();
      }
      return core::StorageContainer{
          .id = core::StorageContainerId::parse(row.at("id").as<std::string>()),
          .lab_id = core::LabId::parse(row.at("lab_id").as<std::string>()),
          .parent_id = pg_optional_id<core::StorageContainerId>(row, "parent_id"),
          .kind = core::parse_container_kind(row.at("kind").as<std::string>()),
          .name = row.at("name").as<std::string>(),
          .label = row.at("label").as<std::string>(),
          .ordering_index = static_cast<int>(row.at("ordering_index").as<std::int64_t>()),
          .capacity_hint = capacity_hint,
          .created_at =
              core::Timestamp::from_unix_micros(row.at("created_at_micros").as<std::int64_t>()),
          .archived_at = pg_optional_timestamp(row, "archived_at_micros"),
      };
    }

    [[nodiscard]] pqxx::params bind_container(const core::StorageContainer& entity) {
      pqxx::params params;
      params.append(entity.id.to_string());
      params.append(entity.lab_id.to_string());
      params.append(entity.parent_id.has_value()
                        ? std::optional<std::string>{entity.parent_id->to_string()}
                        : std::optional<std::string>{});
      params.append(std::string(core::to_string(entity.kind)));
      params.append(entity.name);
      params.append(entity.label);
      params.append(static_cast<std::int64_t>(entity.ordering_index));
      params.append(capacity_hint_dump(entity.capacity_hint));
      params.append(entity.created_at.unix_micros());
      params.append(micros_or_null(entity.archived_at));
      return params;
    }

    class StorageContainerRepository final : public IRepository<core::StorageContainer> {
    public:
      explicit StorageContainerRepository(PostgresTransaction& txn) : txn_(txn) {}

      [[nodiscard]] std::optional<core::StorageContainer>
      find_by_id(const core::StorageContainerId& entity_id) override {
        return load(entity_id);
      }

      [[nodiscard]] std::vector<core::StorageContainer>
      query(const Query<core::StorageContainer>& query_spec) override {
        std::string sql = std::string("SELECT ") + container_columns + " FROM storage_containers";
        std::vector<nlohmann::json> parameters;
        const auto defaults = query_spec.includes_tombstoned()
                                  ? std::vector<std::string>{}
                                  : std::vector<std::string>{"archived_at_micros IS NULL"};
        detail::PostgresDialect dialect;
        detail::append_where(sql, parameters, defaults, query_spec.predicates(),
                             detail::container_column_name, dialect);
        detail::append_order_limit(sql, parameters, query_spec, detail::container_column_name,
                                   dialect);
        try {
          const auto result = txn_.work().exec(sql, pg_bind_params(parameters));
          std::vector<core::StorageContainer> results;
          for (pqxx::row_ref row : result) {
            results.push_back(read_container(row));
          }
          return results;
        } catch (const pqxx::sql_error& err) {
          throw_pqxx_error(err);
        }
      }

      void insert(const core::StorageContainer& entity, const MutationContext& context) override {
        validate_storage_container(entity);
        check_no_cycle(entity);
        try {
          txn_.work().exec(
              "INSERT INTO storage_containers (id, lab_id, parent_id, kind, name, label, "
              "ordering_index, capacity_hint_json, created_at_micros, archived_at_micros) "
              "VALUES ($1, $2, $3, $4, $5, $6, $7, $8::jsonb, $9, $10)",
              bind_container(entity));
        } catch (const pqxx::sql_error& err) {
          throw_pqxx_error(err);
        }
        txn_.note_mutation(std::string(EntityTraits<core::StorageContainer>::entity_name()),
                           entity.id.to_string(), context, "insert", detail::audit_after(entity));
      }

      void update(const core::StorageContainer& entity, const MutationContext& context) override {
        validate_storage_container(entity);
        check_no_cycle(entity);
        write_update(entity, context);
      }

      void soft_delete(const core::StorageContainerId& entity_id,
                       const MutationContext& context) override {
        auto entity = find_by_id(entity_id);
        if (!entity.has_value()) {
          throw NotFound("storage container not found");
        }
        entity->archived_at = now_timestamp();
        // soft_delete does not change parent_id, so the acyclic invariant cannot
        // be broken; skip the (round-trip-heavy) cycle walk.
        validate_storage_container(entity.value());
        write_update(entity.value(), context);
      }

    private:
      void write_update(const core::StorageContainer& entity, const MutationContext& context) {
        try {
          const auto result = txn_.work().exec(
              "UPDATE storage_containers SET lab_id = $2, parent_id = $3, kind = $4, name = $5, "
              "label = $6, ordering_index = $7, capacity_hint_json = $8::jsonb, "
              "created_at_micros = $9, archived_at_micros = $10 WHERE id = $1",
              bind_container(entity));
          if (result.affected_rows() == 0) {
            throw NotFound("storage container not found");
          }
        } catch (const pqxx::sql_error& err) {
          throw_pqxx_error(err);
        }
        txn_.note_mutation(std::string(EntityTraits<core::StorageContainer>::entity_name()),
                           entity.id.to_string(), context, "soft_delete",
                           detail::audit_after(entity));
      }

      // Walk the prospective parent's ancestor chain; if it ever reaches the
      // entity's own id, the proposed edge would form a cycle. Reads go through
      // the live transaction so in-flight inserts are visible.
      void check_no_cycle(const core::StorageContainer& entity) {
        if (!entity.parent_id.has_value()) {
          return;
        }
        std::set<core::StorageContainerId> visited;
        std::optional<core::StorageContainerId> cursor = entity.parent_id;
        while (cursor.has_value()) {
          // NOLINTNEXTLINE(bugprone-unchecked-optional-access): guaranteed by loop condition
          const core::StorageContainerId current = *cursor;
          if (current == entity.id) {
            throw ConstraintViolation("storage container parent chain forms a cycle");
          }
          if (!visited.insert(current).second) {
            throw ConstraintViolation("storage container parent chain forms a cycle");
          }
          const auto ancestor = load(current);
          if (!ancestor.has_value()) {
            return;
          }
          cursor = ancestor->parent_id;
        }
      }

      [[nodiscard]] std::optional<core::StorageContainer>
      load(const core::StorageContainerId& entity_id) {
        try {
          const auto result = txn_.work().exec(std::string("SELECT ") + container_columns +
                                                   " FROM storage_containers WHERE id = $1",
                                               pqxx::params{entity_id.to_string()});
          if (result.empty()) {
            return std::nullopt;
          }
          return read_container(result[0]);
        } catch (const pqxx::sql_error& err) {
          throw_pqxx_error(err);
        }
      }

      PostgresTransaction& txn_;
    };

  } // namespace

  void register_layout_repositories(PostgresBackend& backend) {
    backend.register_repository_factory<core::StorageContainer>(
        [](PostgresTransaction& txn) { return std::make_unique<StorageContainerRepository>(txn); });
    backend.register_repository_factory<core::Freezer>(
        [](PostgresTransaction& txn) { return std::make_unique<FreezerRepository>(txn); });
  }

} // namespace fmgr::storage
