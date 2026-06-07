// SPDX-License-Identifier: AGPL-3.0-or-later

#include "storage/postgres/BoxGeometryRepositories.h"

#include "core/box.h"
#include "storage/BoxGeometryTraits.h"
#include "storage/detail/BoxGeometryColumns.h"
#include "storage/detail/QuerySqlBuilder.h"
#include "storage/postgres/PostgresRepoSupport.h"

#include <pqxx/pqxx>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace fmgr::storage {
  namespace {

    using detail::dimensions_dump;
    using detail::micros_or_null;
    using detail::parse_dimensions;
    using detail::pg_bind_params;
    using detail::pg_optional_string;
    using detail::pg_optional_timestamp;
    using detail::throw_pqxx_error;
    using detail::validate_box_type_shape;
    using detail::validate_container_type;

    [[nodiscard]] core::Timestamp now_timestamp() {
      const auto now =
          std::chrono::time_point_cast<std::chrono::microseconds>(std::chrono::system_clock::now());
      return core::Timestamp::from_unix_micros(now.time_since_epoch().count());
    }

    // ---- ContainerType ----

    constexpr const char* container_type_columns =
        "id, lab_id, name, size_class, outer_dimensions_json, material, supplier_sku, "
        "created_at_micros, archived_at_micros";

    [[nodiscard]] core::ContainerType read_container_type(pqxx::row_ref row) {
      return core::ContainerType{
          .id = core::ContainerTypeId::parse(row.at("id").as<std::string>()),
          .lab_id = core::LabId::parse(row.at("lab_id").as<std::string>()),
          .name = row.at("name").as<std::string>(),
          .size_class = row.at("size_class").as<std::string>(),
          .outer_dimensions_mm =
              parse_dimensions(row.at("outer_dimensions_json").as<std::string>()),
          .material = row.at("material").as<std::string>(),
          .supplier_sku = row.at("supplier_sku").as<std::string>(),
          .created_at =
              core::Timestamp::from_unix_micros(row.at("created_at_micros").as<std::int64_t>()),
          .archived_at = pg_optional_timestamp(row, "archived_at_micros"),
      };
    }

    [[nodiscard]] pqxx::params bind_container_type(const core::ContainerType& entity) {
      pqxx::params params;
      params.append(entity.id.to_string());
      params.append(entity.lab_id.to_string());
      params.append(entity.name);
      params.append(entity.size_class);
      params.append(dimensions_dump(entity.outer_dimensions_mm));
      params.append(entity.material);
      params.append(entity.supplier_sku);
      params.append(entity.created_at.unix_micros());
      params.append(micros_or_null(entity.archived_at));
      return params;
    }

    class ContainerTypeRepository final : public IRepository<core::ContainerType> {
    public:
      explicit ContainerTypeRepository(PostgresTransaction& txn) : txn_(txn) {}

      [[nodiscard]] std::optional<core::ContainerType>
      find_by_id(const core::ContainerTypeId& entity_id) override {
        try {
          const auto result = txn_.work().exec(std::string("SELECT ") + container_type_columns +
                                                   " FROM container_types WHERE id = $1",
                                               pqxx::params{entity_id.to_string()});
          if (result.empty()) {
            return std::nullopt;
          }
          return read_container_type(result[0]);
        } catch (const pqxx::sql_error& err) {
          throw_pqxx_error(err);
        }
      }

      [[nodiscard]] std::vector<core::ContainerType>
      query(const Query<core::ContainerType>& query_spec) override {
        std::string sql = std::string("SELECT ") + container_type_columns + " FROM container_types";
        std::vector<nlohmann::json> parameters;
        const auto defaults = query_spec.includes_tombstoned()
                                  ? std::vector<std::string>{}
                                  : std::vector<std::string>{"archived_at_micros IS NULL"};
        detail::PostgresDialect dialect;
        detail::append_where(sql, parameters, defaults, query_spec.predicates(),
                             detail::container_type_column_name, dialect);
        detail::append_order_limit(sql, parameters, query_spec, detail::container_type_column_name,
                                   dialect);
        try {
          const auto result = txn_.work().exec(sql, pg_bind_params(parameters));
          std::vector<core::ContainerType> results;
          for (pqxx::row_ref row : result) {
            results.push_back(read_container_type(row));
          }
          return results;
        } catch (const pqxx::sql_error& err) {
          throw_pqxx_error(err);
        }
      }

      void insert(const core::ContainerType& entity, const MutationContext& context) override {
        validate_container_type(entity);
        try {
          txn_.work().exec("INSERT INTO container_types (id, lab_id, name, size_class, "
                           "outer_dimensions_json, material, supplier_sku, created_at_micros, "
                           "archived_at_micros) VALUES ($1, $2, $3, $4, $5::jsonb, $6, $7, $8, $9)",
                           bind_container_type(entity));
        } catch (const pqxx::sql_error& err) {
          throw_pqxx_error(err);
        }
        txn_.note_mutation(std::string(EntityTraits<core::ContainerType>::entity_name()),
                           entity.id.to_string(), context);
      }

      void update(const core::ContainerType& entity, const MutationContext& context) override {
        validate_container_type(entity);
        try {
          const auto result = txn_.work().exec(
              "UPDATE container_types SET lab_id = $2, name = $3, size_class = $4, "
              "outer_dimensions_json = $5::jsonb, material = $6, supplier_sku = $7, "
              "created_at_micros = $8, archived_at_micros = $9 WHERE id = $1",
              bind_container_type(entity));
          if (result.affected_rows() == 0) {
            throw NotFound("container type not found");
          }
        } catch (const pqxx::sql_error& err) {
          throw_pqxx_error(err);
        }
        txn_.note_mutation(std::string(EntityTraits<core::ContainerType>::entity_name()),
                           entity.id.to_string(), context);
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
      PostgresTransaction& txn_;
    };

    // ---- BoxType (parent row + position / accepts child rows) ----

    constexpr const char* box_type_columns =
        "id, lab_id, name, manufacturer, sku, created_at_micros, archived_at_micros";

    class BoxTypeRepository final : public IRepository<core::BoxType> {
    public:
      explicit BoxTypeRepository(PostgresTransaction& txn) : txn_(txn) {}

      [[nodiscard]] std::optional<core::BoxType>
      find_by_id(const core::BoxTypeId& entity_id) override {
        return load(entity_id);
      }

      [[nodiscard]] std::vector<core::BoxType>
      query(const Query<core::BoxType>& query_spec) override {
        std::string sql = std::string("SELECT ") + box_type_columns + " FROM box_types";
        std::vector<nlohmann::json> parameters;
        const auto defaults = query_spec.includes_tombstoned()
                                  ? std::vector<std::string>{}
                                  : std::vector<std::string>{"archived_at_micros IS NULL"};
        detail::PostgresDialect dialect;
        detail::append_where(sql, parameters, defaults, query_spec.predicates(),
                             detail::box_type_column_name, dialect);
        detail::append_order_limit(sql, parameters, query_spec, detail::box_type_column_name,
                                   dialect);
        try {
          const auto result = txn_.work().exec(sql, pg_bind_params(parameters));
          std::vector<core::BoxType> results;
          for (pqxx::row_ref row : result) {
            results.push_back(read_box_type_row(row));
          }
          return results;
        } catch (const pqxx::sql_error& err) {
          throw_pqxx_error(err);
        }
      }

      void insert(const core::BoxType& entity, const MutationContext& context) override {
        validate(entity);
        try {
          txn_.work().exec("INSERT INTO box_types (id, lab_id, name, manufacturer, sku, "
                           "created_at_micros, archived_at_micros) "
                           "VALUES ($1, $2, $3, $4, $5, $6, $7)",
                           bind_box_type(entity));
          replace_positions(entity);
        } catch (const pqxx::sql_error& err) {
          throw_pqxx_error(err);
        }
        txn_.note_mutation(std::string(EntityTraits<core::BoxType>::entity_name()),
                           entity.id.to_string(), context);
      }

      void update(const core::BoxType& entity, const MutationContext& context) override {
        validate(entity);
        try {
          const auto result =
              txn_.work().exec("UPDATE box_types SET lab_id = $2, name = $3, manufacturer = $4, "
                               "sku = $5, created_at_micros = $6, archived_at_micros = $7 "
                               "WHERE id = $1",
                               bind_box_type(entity));
          if (result.affected_rows() == 0) {
            throw NotFound("box type not found");
          }
          replace_positions(entity);
        } catch (const pqxx::sql_error& err) {
          throw_pqxx_error(err);
        }
        txn_.note_mutation(std::string(EntityTraits<core::BoxType>::entity_name()),
                           entity.id.to_string(), context);
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
      void validate(const core::BoxType& entity) {
        validate_box_type_shape(entity);
        for (const auto& position : entity.positions) {
          for (const auto& size_class : position.accepts) {
            if (!live_size_class_exists(entity.lab_id, size_class)) {
              throw ConstraintViolation("box type position accepts unknown size_class");
            }
          }
        }
      }

      [[nodiscard]] bool live_size_class_exists(core::LabId lab_id, const std::string& size_class) {
        const auto result = txn_.work().exec(
            "SELECT 1 FROM container_types "
            "WHERE lab_id = $1 AND size_class = $2 AND archived_at_micros IS NULL LIMIT 1",
            pqxx::params{lab_id.to_string(), size_class});
        return !result.empty();
      }

      [[nodiscard]] std::vector<std::string> load_accepts(const core::BoxTypeId& box_type_id,
                                                          const std::string& label) {
        const auto result =
            txn_.work().exec("SELECT size_class FROM box_type_position_accepts "
                             "WHERE box_type_id = $1 AND position_label = $2 ORDER BY size_class",
                             pqxx::params{box_type_id.to_string(), label});
        std::vector<std::string> accepts;
        for (pqxx::row_ref row : result) {
          accepts.push_back(row.at("size_class").as<std::string>());
        }
        return accepts;
      }

      [[nodiscard]] std::vector<core::Position> load_positions(const core::BoxTypeId& box_type_id) {
        const auto result =
            txn_.work().exec("SELECT label, row_index, col_index, z_index FROM box_type_positions "
                             "WHERE box_type_id = $1 ORDER BY row_index, col_index, z_index, label",
                             pqxx::params{box_type_id.to_string()});
        std::vector<core::Position> positions;
        for (pqxx::row_ref row : result) {
          core::Position position{
              .label = row.at("label").as<std::string>(),
              .row = row.at("row_index").as<int>(),
              .col = row.at("col_index").as<int>(),
              .z = row.at("z_index").is_null()
                       ? std::optional<std::int32_t>{}
                       : std::optional<std::int32_t>{row.at("z_index").as<std::int32_t>()},
          };
          position.accepts = load_accepts(box_type_id, position.label);
          positions.push_back(std::move(position));
        }
        return positions;
      }

      [[nodiscard]] core::BoxType read_box_type_row(pqxx::row_ref row) {
        const auto box_type_id = core::BoxTypeId::parse(row.at("id").as<std::string>());
        return core::BoxType{
            .id = box_type_id,
            .lab_id = core::LabId::parse(row.at("lab_id").as<std::string>()),
            .name = row.at("name").as<std::string>(),
            .manufacturer = row.at("manufacturer").as<std::string>(),
            .sku = row.at("sku").as<std::string>(),
            .positions = load_positions(box_type_id),
            .created_at =
                core::Timestamp::from_unix_micros(row.at("created_at_micros").as<std::int64_t>()),
            .archived_at = pg_optional_timestamp(row, "archived_at_micros"),
        };
      }

      [[nodiscard]] std::optional<core::BoxType> load(const core::BoxTypeId& entity_id) {
        try {
          const auto result = txn_.work().exec(std::string("SELECT ") + box_type_columns +
                                                   " FROM box_types WHERE id = $1",
                                               pqxx::params{entity_id.to_string()});
          if (result.empty()) {
            return std::nullopt;
          }
          return read_box_type_row(result[0]);
        } catch (const pqxx::sql_error& err) {
          throw_pqxx_error(err);
        }
      }

      [[nodiscard]] static pqxx::params bind_box_type(const core::BoxType& entity) {
        pqxx::params params;
        params.append(entity.id.to_string());
        params.append(entity.lab_id.to_string());
        params.append(entity.name);
        params.append(entity.manufacturer);
        params.append(entity.sku);
        params.append(entity.created_at.unix_micros());
        params.append(micros_or_null(entity.archived_at));
        return params;
      }

      void replace_positions(const core::BoxType& entity) {
        // The accepts child rows cascade-delete with their positions.
        txn_.work().exec("DELETE FROM box_type_positions WHERE box_type_id = $1",
                         pqxx::params{entity.id.to_string()});
        for (const auto& position : entity.positions) {
          txn_.work().exec(
              "INSERT INTO box_type_positions (box_type_id, label, row_index, col_index, z_index) "
              "VALUES ($1, $2, $3, $4, $5)",
              pqxx::params{entity.id.to_string(), position.label,
                           static_cast<std::int64_t>(position.row),
                           static_cast<std::int64_t>(position.col),
                           position.z.has_value() ? std::optional<std::int64_t>{position.z.value()}
                                                  : std::optional<std::int64_t>{}});
          for (const auto& size_class : position.accepts) {
            txn_.work().exec("INSERT INTO box_type_position_accepts "
                             "(box_type_id, position_label, size_class) VALUES ($1, $2, $3)",
                             pqxx::params{entity.id.to_string(), position.label, size_class});
          }
        }
      }

      PostgresTransaction& txn_;
    };

    // ---- Box ----

    constexpr const char* box_columns =
        "id, lab_id, box_type_id, storage_container_id, label, serial, barcode, "
        "created_at_micros, archived_at_micros";

    [[nodiscard]] core::Box read_box(pqxx::row_ref row) {
      return core::Box{
          .id = core::BoxId::parse(row.at("id").as<std::string>()),
          .lab_id = core::LabId::parse(row.at("lab_id").as<std::string>()),
          .box_type_id = core::BoxTypeId::parse(row.at("box_type_id").as<std::string>()),
          .storage_container_id =
              core::StorageContainerId::parse(row.at("storage_container_id").as<std::string>()),
          .label = row.at("label").as<std::string>(),
          .serial = pg_optional_string(row, "serial"),
          .barcode = pg_optional_string(row, "barcode"),
          .created_at =
              core::Timestamp::from_unix_micros(row.at("created_at_micros").as<std::int64_t>()),
          .archived_at = pg_optional_timestamp(row, "archived_at_micros"),
      };
    }

    [[nodiscard]] pqxx::params bind_box(const core::Box& entity) {
      pqxx::params params;
      params.append(entity.id.to_string());
      params.append(entity.lab_id.to_string());
      params.append(entity.box_type_id.to_string());
      params.append(entity.storage_container_id.to_string());
      params.append(entity.label);
      params.append(entity.serial);
      params.append(entity.barcode);
      params.append(entity.created_at.unix_micros());
      params.append(micros_or_null(entity.archived_at));
      return params;
    }

    class BoxRepository final : public IRepository<core::Box> {
    public:
      explicit BoxRepository(PostgresTransaction& txn) : txn_(txn) {}

      [[nodiscard]] std::optional<core::Box> find_by_id(const core::BoxId& entity_id) override {
        try {
          const auto result =
              txn_.work().exec(std::string("SELECT ") + box_columns + " FROM boxes WHERE id = $1",
                               pqxx::params{entity_id.to_string()});
          if (result.empty()) {
            return std::nullopt;
          }
          return read_box(result[0]);
        } catch (const pqxx::sql_error& err) {
          throw_pqxx_error(err);
        }
      }

      [[nodiscard]] std::vector<core::Box> query(const Query<core::Box>& query_spec) override {
        std::string sql = std::string("SELECT ") + box_columns + " FROM boxes";
        std::vector<nlohmann::json> parameters;
        const auto defaults = query_spec.includes_tombstoned()
                                  ? std::vector<std::string>{}
                                  : std::vector<std::string>{"archived_at_micros IS NULL"};
        detail::PostgresDialect dialect;
        detail::append_where(sql, parameters, defaults, query_spec.predicates(),
                             detail::box_column_name, dialect);
        detail::append_order_limit(sql, parameters, query_spec, detail::box_column_name, dialect);
        try {
          const auto result = txn_.work().exec(sql, pg_bind_params(parameters));
          std::vector<core::Box> results;
          for (pqxx::row_ref row : result) {
            results.push_back(read_box(row));
          }
          return results;
        } catch (const pqxx::sql_error& err) {
          throw_pqxx_error(err);
        }
      }

      void insert(const core::Box& entity, const MutationContext& context) override {
        validate_box(entity);
        try {
          txn_.work().exec("INSERT INTO boxes (id, lab_id, box_type_id, storage_container_id, "
                           "label, serial, barcode, created_at_micros, archived_at_micros) "
                           "VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9)",
                           bind_box(entity));
        } catch (const pqxx::sql_error& err) {
          throw_pqxx_error(err);
        }
        txn_.note_mutation(std::string(EntityTraits<core::Box>::entity_name()),
                           entity.id.to_string(), context);
      }

      void update(const core::Box& entity, const MutationContext& context) override {
        validate_box(entity);
        try {
          const auto result = txn_.work().exec(
              "UPDATE boxes SET lab_id = $2, box_type_id = $3, storage_container_id = $4, "
              "label = $5, serial = $6, barcode = $7, created_at_micros = $8, "
              "archived_at_micros = $9 WHERE id = $1",
              bind_box(entity));
          if (result.affected_rows() == 0) {
            throw NotFound("box not found");
          }
        } catch (const pqxx::sql_error& err) {
          throw_pqxx_error(err);
        }
        txn_.note_mutation(std::string(EntityTraits<core::Box>::entity_name()),
                           entity.id.to_string(), context);
      }

      void soft_delete(const core::BoxId& entity_id, const MutationContext& context) override {
        auto entity = find_by_id(entity_id);
        if (!entity.has_value()) {
          throw NotFound("box not found");
        }
        entity->archived_at = now_timestamp();
        update(entity.value(), context);
      }

    private:
      void validate_box(const core::Box& box) {
        if (box.label.empty()) {
          throw ConstraintViolation("box label is required");
        }
        const auto box_type = txn_.work().exec(
            "SELECT 1 FROM box_types WHERE id = $1 AND lab_id = $2 AND archived_at_micros IS NULL "
            "LIMIT 1",
            pqxx::params{box.box_type_id.to_string(), box.lab_id.to_string()});
        if (box_type.empty()) {
          throw ConstraintViolation("box_type_id does not reference a live BoxType in this lab");
        }
        const auto container = txn_.work().exec(
            "SELECT 1 FROM storage_containers WHERE id = $1 AND lab_id = $2 AND "
            "archived_at_micros IS NULL LIMIT 1",
            pqxx::params{box.storage_container_id.to_string(), box.lab_id.to_string()});
        if (container.empty()) {
          throw ConstraintViolation(
              "storage_container_id does not reference a live StorageContainer in this lab");
        }
      }

      PostgresTransaction& txn_;
    };

  } // namespace

  void register_box_geometry_repositories(PostgresBackend& backend) {
    backend.register_repository_factory<core::ContainerType>(
        [](PostgresTransaction& txn) { return std::make_unique<ContainerTypeRepository>(txn); });
    backend.register_repository_factory<core::BoxType>(
        [](PostgresTransaction& txn) { return std::make_unique<BoxTypeRepository>(txn); });
  }

  void register_box_repositories(PostgresBackend& backend) {
    backend.register_repository_factory<core::Box>(
        [](PostgresTransaction& txn) { return std::make_unique<BoxRepository>(txn); });
  }

} // namespace fmgr::storage
