// SPDX-License-Identifier: AGPL-3.0-or-later

#include "storage/postgres/SampleRepositories.h"

#include "core/sample.h"
#include "storage/SampleTraits.h"
#include "storage/detail/QuerySqlBuilder.h"
#include "storage/detail/SampleColumns.h"
#include "storage/postgres/PostgresRepoSupport.h"

#include <pqxx/pqxx>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace fmgr::storage {
  namespace {

    using detail::id_or_null;
    using detail::pg_bind_params;
    using detail::pg_optional_id;
    using detail::pg_optional_string;
    using detail::throw_pqxx_error;
    using detail::validate_project;
    using detail::validate_sample_shape;

    [[nodiscard]] core::Timestamp now_timestamp() {
      const auto now =
          std::chrono::time_point_cast<std::chrono::microseconds>(std::chrono::system_clock::now());
      return core::Timestamp::from_unix_micros(now.time_since_epoch().count());
    }

    [[nodiscard]] std::optional<std::int64_t> pg_optional_int64(pqxx::row_ref row,
                                                                const char* column) {
      const auto field = row.at(column);
      if (field.is_null()) {
        return std::nullopt;
      }
      return field.as<std::int64_t>();
    }

    [[nodiscard]] std::optional<std::string>
    volume_unit_text(const std::optional<core::VolumeUnit>& unit) {
      if (!unit.has_value()) {
        return std::nullopt;
      }
      return std::string(core::to_string(unit.value()));
    }

    [[nodiscard]] std::optional<std::string>
    mass_unit_text(const std::optional<core::MassUnit>& unit) {
      if (!unit.has_value()) {
        return std::nullopt;
      }
      return std::string(core::to_string(unit.value()));
    }

    [[nodiscard]] std::optional<core::VolumeUnit> read_volume_unit(pqxx::row_ref row,
                                                                   const char* column) {
      const auto text = pg_optional_string(row, column);
      if (!text.has_value()) {
        return std::nullopt;
      }
      return core::parse_volume_unit(text.value());
    }

    [[nodiscard]] std::optional<core::MassUnit> read_mass_unit(pqxx::row_ref row,
                                                               const char* column) {
      const auto text = pg_optional_string(row, column);
      if (!text.has_value()) {
        return std::nullopt;
      }
      return core::parse_mass_unit(text.value());
    }

    // ---- Sample ----

    constexpr const char* sample_columns =
        "id, lab_id, item_type_id, name, barcode, container_type_id, box_id, position_label, "
        "volume_value, volume_unit, mass_value, mass_unit, status, parent_sample_id, created_by, "
        "created_at_micros, last_modified_by, last_modified_at_micros, custom_fields_json, "
        "phi_fields_enc_json";

    [[nodiscard]] core::Sample read_sample(pqxx::row_ref row) {
      return core::Sample{
          .id = core::SampleId::parse(row.at("id").as<std::string>()),
          .lab_id = core::LabId::parse(row.at("lab_id").as<std::string>()),
          .item_type_id = core::ItemTypeId::parse(row.at("item_type_id").as<std::string>()),
          .name = row.at("name").as<std::string>(),
          .barcode = pg_optional_string(row, "barcode"),
          .container_type_id = pg_optional_id<core::ContainerTypeId>(row, "container_type_id"),
          .box_id = pg_optional_id<core::BoxId>(row, "box_id"),
          .position_label = pg_optional_string(row, "position_label"),
          .volume_value = pg_optional_int64(row, "volume_value"),
          .volume_unit = read_volume_unit(row, "volume_unit"),
          .mass_value = pg_optional_int64(row, "mass_value"),
          .mass_unit = read_mass_unit(row, "mass_unit"),
          .status = core::parse_sample_status(row.at("status").as<std::string>()),
          .parent_sample_id = pg_optional_id<core::SampleId>(row, "parent_sample_id"),
          .created_by = core::UserId::parse(row.at("created_by").as<std::string>()),
          .created_at =
              core::Timestamp::from_unix_micros(row.at("created_at_micros").as<std::int64_t>()),
          .last_modified_by = core::UserId::parse(row.at("last_modified_by").as<std::string>()),
          .last_modified_at = core::Timestamp::from_unix_micros(
              row.at("last_modified_at_micros").as<std::int64_t>()),
          .custom_fields_json = row.at("custom_fields_json").as<std::string>(),
          .phi_fields_enc_json = row.at("phi_fields_enc_json").as<std::string>(),
      };
    }

    [[nodiscard]] pqxx::params bind_sample(const core::Sample& entity) {
      pqxx::params params;
      params.append(entity.id.to_string());
      params.append(entity.lab_id.to_string());
      params.append(entity.item_type_id.to_string());
      params.append(entity.name);
      params.append(entity.barcode);
      params.append(id_or_null(entity.container_type_id));
      params.append(id_or_null(entity.box_id));
      params.append(entity.position_label);
      params.append(entity.volume_value);
      params.append(volume_unit_text(entity.volume_unit));
      params.append(entity.mass_value);
      params.append(mass_unit_text(entity.mass_unit));
      params.append(std::string(core::to_string(entity.status)));
      params.append(id_or_null(entity.parent_sample_id));
      params.append(entity.created_by.to_string());
      params.append(entity.created_at.unix_micros());
      params.append(entity.last_modified_by.to_string());
      params.append(entity.last_modified_at.unix_micros());
      params.append(entity.custom_fields_json);
      params.append(entity.phi_fields_enc_json);
      return params;
    }

    class SampleRepository final : public IRepository<core::Sample> {
    public:
      explicit SampleRepository(PostgresTransaction& txn) : txn_(txn) {}

      [[nodiscard]] std::optional<core::Sample>
      find_by_id(const core::SampleId& entity_id) override {
        return load(entity_id);
      }

      [[nodiscard]] std::vector<core::Sample>
      query(const Query<core::Sample>& query_spec) override {
        std::string sql = std::string("SELECT ") + sample_columns + " FROM samples";
        std::vector<nlohmann::json> parameters;
        // Sample tombstone uses status != 'tombstoned', not archived_at_micros.
        const auto defaults = query_spec.includes_tombstoned()
                                  ? std::vector<std::string>{}
                                  : std::vector<std::string>{"status != 'tombstoned'"};
        detail::PostgresDialect dialect;
        detail::append_where(sql, parameters, defaults, query_spec.predicates(),
                             detail::sample_column_name, dialect);
        detail::append_order_limit(sql, parameters, query_spec, detail::sample_column_name,
                                   dialect);
        try {
          const auto result = txn_.work().exec(sql, pg_bind_params(parameters));
          std::vector<core::Sample> results;
          for (pqxx::row_ref row : result) {
            results.push_back(read_sample(row));
          }
          return results;
        } catch (const pqxx::sql_error& err) {
          throw_pqxx_error(err);
        }
      }

      void insert(const core::Sample& entity, const MutationContext& context) override {
        validate_sample(entity);
        try {
          txn_.work().exec(
              "INSERT INTO samples (id, lab_id, item_type_id, name, barcode, container_type_id, "
              "box_id, position_label, volume_value, volume_unit, mass_value, mass_unit, status, "
              "parent_sample_id, created_by, created_at_micros, last_modified_by, "
              "last_modified_at_micros, custom_fields_json, phi_fields_enc_json) "
              "VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12, $13, $14, $15, $16, $17, "
              "$18, $19::jsonb, $20::jsonb)",
              bind_sample(entity));
        } catch (const pqxx::sql_error& err) {
          throw_pqxx_error(err);
        }
        txn_.note_mutation(std::string(EntityTraits<core::Sample>::entity_name()),
                           entity.id.to_string(), context, "insert", detail::audit_after(entity));
      }

      void update(const core::Sample& entity, const MutationContext& context) override {
        // Missing-row takes precedence over cross-entity validation so update of a
        // nonexistent sample reports NotFound (matching the SQLite backend and the
        // repository contract) rather than a ConstraintViolation about its refs.
        const auto before = find_by_id(entity.id);
        if (!before.has_value()) {
          throw NotFound("sample not found");
        }
        validate_sample(entity);
        write_update(entity, context, before, "update");
      }

      void soft_delete(const core::SampleId& entity_id, const MutationContext& context) override {
        auto entity = find_by_id(entity_id);
        if (!entity.has_value()) {
          throw NotFound("sample not found");
        }
        const auto before = entity; // authoritative pre-tombstone state for the audit image
        entity->status = core::SampleStatus::Tombstoned;
        entity->last_modified_by = context.actor_user_id;
        entity->last_modified_at = now_timestamp();
        // Tombstone does not change structural fields; skip cross-entity validation.
        write_update(entity.value(), context, before, "soft_delete");
      }

    private:
      void write_update(const core::Sample& entity, const MutationContext& context,
                        const std::optional<core::Sample>& before, std::string action) {
        try {
          const auto result = txn_.work().exec(
              "UPDATE samples SET lab_id = $2, item_type_id = $3, name = $4, barcode = $5, "
              "container_type_id = $6, box_id = $7, position_label = $8, volume_value = $9, "
              "volume_unit = $10, mass_value = $11, mass_unit = $12, status = $13, "
              "parent_sample_id = $14, created_by = $15, created_at_micros = $16, "
              "last_modified_by = $17, last_modified_at_micros = $18, "
              "custom_fields_json = $19::jsonb, phi_fields_enc_json = $20::jsonb WHERE id = $1",
              bind_sample(entity));
          if (result.affected_rows() == 0) {
            throw NotFound("sample not found");
          }
        } catch (const pqxx::sql_error& err) {
          throw_pqxx_error(err);
        }
        txn_.note_mutation(std::string(EntityTraits<core::Sample>::entity_name()),
                           entity.id.to_string(), context, std::move(action),
                           detail::audit_change(before, entity));
      }

      void validate_sample(const core::Sample& sample) {
        validate_sample_shape(sample);
        if (sample.box_id.has_value()) {
          // box_id and position_label are paired by validate_sample_shape above.
          // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
          const std::string& position_label = sample.position_label.value();
          if (txn_.work()
                  .exec("SELECT 1 FROM boxes WHERE id = $1 AND lab_id = $2 AND archived_at_micros "
                        "IS NULL LIMIT 1",
                        pqxx::params{sample.box_id->to_string(), sample.lab_id.to_string()})
                  .empty()) {
            throw ConstraintViolation("box_id does not reference a live Box in this lab");
          }
          if (txn_.work()
                  .exec("SELECT 1 FROM box_type_positions btp "
                        "JOIN boxes b ON b.box_type_id = btp.box_type_id "
                        "WHERE b.id = $1 AND btp.label = $2 LIMIT 1",
                        pqxx::params{sample.box_id->to_string(), position_label})
                  .empty()) {
            throw ConstraintViolation("position_label does not exist in this box's BoxType");
          }
          if (sample.container_type_id.has_value()) {
            if (txn_.work()
                    .exec("SELECT 1 FROM box_type_position_accepts bpa "
                          "JOIN boxes b ON b.box_type_id = bpa.box_type_id "
                          "JOIN container_types ct ON ct.size_class = bpa.size_class "
                          "WHERE b.id = $1 AND bpa.position_label = $2 AND ct.id = $3 "
                          "AND ct.lab_id = $4 LIMIT 1",
                          pqxx::params{sample.box_id->to_string(), position_label,
                                       sample.container_type_id->to_string(),
                                       sample.lab_id.to_string()})
                    .empty()) {
              throw ConstraintViolation(
                  "container_type size_class is not accepted at this box position");
            }
          }
        }
        // container_type_id (when set) must reference a live ContainerType in the
        // same lab, even for an unplaced sample — otherwise a Lab-B container type
        // whose size_class token matches could slip in.
        if (sample.container_type_id.has_value()) {
          if (txn_.work()
                  .exec("SELECT 1 FROM container_types WHERE id = $1 AND lab_id = $2 AND "
                        "archived_at_micros IS NULL LIMIT 1",
                        pqxx::params{sample.container_type_id->to_string(),
                                     sample.lab_id.to_string()})
                  .empty()) {
            throw ConstraintViolation(
                "container_type_id does not reference a live ContainerType in this lab");
          }
        }
        if (txn_.work()
                .exec("SELECT 1 FROM item_types WHERE id = $1 AND lab_id = $2 AND "
                      "archived_at_micros IS NULL LIMIT 1",
                      pqxx::params{sample.item_type_id.to_string(), sample.lab_id.to_string()})
                .empty()) {
          throw ConstraintViolation("item_type_id does not reference a live ItemType in this lab");
        }
        // parent_sample_id must be in the same lab; self-reference is forbidden.
        if (sample.parent_sample_id.has_value()) {
          if (*sample.parent_sample_id == sample.id) {
            throw ConstraintViolation("sample cannot be its own parent");
          }
          if (txn_.work()
                  .exec(
                      "SELECT 1 FROM samples WHERE id = $1 AND lab_id = $2 LIMIT 1",
                      pqxx::params{sample.parent_sample_id->to_string(), sample.lab_id.to_string()})
                  .empty()) {
            throw ForeignKeyViolation("parent_sample_id does not reference a sample in this lab");
          }
          // Reject a multi-hop lineage cycle: if this sample's id appears in the
          // ancestry chain starting at the proposed parent, the edge closes a
          // loop. The depth bound guards against an unexpected pre-existing cycle.
          if (!txn_.work()
                   .exec("WITH RECURSIVE anc(id, depth) AS ("
                         "  SELECT $1::text, 1 "
                         "  UNION ALL "
                         "  SELECT s.parent_sample_id, anc.depth + 1 "
                         "  FROM samples s JOIN anc ON s.id = anc.id "
                         "  WHERE s.parent_sample_id IS NOT NULL AND anc.depth < 10000) "
                         "SELECT 1 FROM anc WHERE id = $2 LIMIT 1",
                         pqxx::params{sample.parent_sample_id->to_string(), sample.id.to_string()})
                   .empty()) {
            throw ConstraintViolation("parent_sample_id would create a lineage cycle");
          }
        }
      }

      [[nodiscard]] std::optional<core::Sample> load(const core::SampleId& entity_id) {
        try {
          const auto result = txn_.work().exec(std::string("SELECT ") + sample_columns +
                                                   " FROM samples WHERE id = $1",
                                               pqxx::params{entity_id.to_string()});
          if (result.empty()) {
            return std::nullopt;
          }
          return read_sample(result[0]);
        } catch (const pqxx::sql_error& err) {
          throw_pqxx_error(err);
        }
      }

      PostgresTransaction& txn_;
    };

    // ---- Project ----

    constexpr const char* project_columns =
        "id, lab_id, name, owner_user_id, created_at_micros, archived_at_micros";

    [[nodiscard]] core::Project read_project(pqxx::row_ref row) {
      return core::Project{
          .id = core::ProjectId::parse(row.at("id").as<std::string>()),
          .lab_id = core::LabId::parse(row.at("lab_id").as<std::string>()),
          .name = row.at("name").as<std::string>(),
          .owner_user_id = core::UserId::parse(row.at("owner_user_id").as<std::string>()),
          .created_at =
              core::Timestamp::from_unix_micros(row.at("created_at_micros").as<std::int64_t>()),
          .archived_at = detail::pg_optional_timestamp(row, "archived_at_micros"),
      };
    }

    [[nodiscard]] pqxx::params bind_project(const core::Project& entity) {
      pqxx::params params;
      params.append(entity.id.to_string());
      params.append(entity.lab_id.to_string());
      params.append(entity.name);
      params.append(entity.owner_user_id.to_string());
      params.append(entity.created_at.unix_micros());
      params.append(detail::micros_or_null(entity.archived_at));
      return params;
    }

    class ProjectRepository final : public IRepository<core::Project> {
    public:
      explicit ProjectRepository(PostgresTransaction& txn) : txn_(txn) {}

      [[nodiscard]] std::optional<core::Project>
      find_by_id(const core::ProjectId& entity_id) override {
        try {
          const auto result = txn_.work().exec(std::string("SELECT ") + project_columns +
                                                   " FROM projects WHERE id = $1",
                                               pqxx::params{entity_id.to_string()});
          if (result.empty()) {
            return std::nullopt;
          }
          return read_project(result[0]);
        } catch (const pqxx::sql_error& err) {
          throw_pqxx_error(err);
        }
      }

      [[nodiscard]] std::vector<core::Project>
      query(const Query<core::Project>& query_spec) override {
        std::string sql = std::string("SELECT ") + project_columns + " FROM projects";
        std::vector<nlohmann::json> parameters;
        const auto defaults = query_spec.includes_tombstoned()
                                  ? std::vector<std::string>{}
                                  : std::vector<std::string>{"archived_at_micros IS NULL"};
        detail::PostgresDialect dialect;
        detail::append_where(sql, parameters, defaults, query_spec.predicates(),
                             detail::project_column_name, dialect);
        detail::append_order_limit(sql, parameters, query_spec, detail::project_column_name,
                                   dialect);
        try {
          const auto result = txn_.work().exec(sql, pg_bind_params(parameters));
          std::vector<core::Project> results;
          for (pqxx::row_ref row : result) {
            results.push_back(read_project(row));
          }
          return results;
        } catch (const pqxx::sql_error& err) {
          throw_pqxx_error(err);
        }
      }

      void insert(const core::Project& entity, const MutationContext& context) override {
        validate_project(entity);
        try {
          txn_.work().exec("INSERT INTO projects (id, lab_id, name, owner_user_id, "
                           "created_at_micros, archived_at_micros) VALUES ($1, $2, $3, $4, $5, $6)",
                           bind_project(entity));
        } catch (const pqxx::sql_error& err) {
          throw_pqxx_error(err);
        }
        txn_.note_mutation(std::string(EntityTraits<core::Project>::entity_name()),
                           entity.id.to_string(), context, "insert", detail::audit_after(entity));
      }

      void update(const core::Project& entity, const MutationContext& context) override {
        validate_project(entity);
        const auto before = find_by_id(entity.id);
        try {
          const auto result =
              txn_.work().exec("UPDATE projects SET lab_id = $2, name = $3, owner_user_id = $4, "
                               "created_at_micros = $5, archived_at_micros = $6 WHERE id = $1",
                               bind_project(entity));
          if (result.affected_rows() == 0) {
            throw NotFound("project not found");
          }
        } catch (const pqxx::sql_error& err) {
          throw_pqxx_error(err);
        }
        txn_.note_mutation(std::string(EntityTraits<core::Project>::entity_name()),
                           entity.id.to_string(), context, "update",
                           detail::audit_change(before, entity));
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
      PostgresTransaction& txn_;
    };

    // ---- SampleProject (composite-key link, hard delete) ----

    class SampleProjectRepository final : public IRepository<core::SampleProject> {
    public:
      explicit SampleProjectRepository(PostgresTransaction& txn) : txn_(txn) {}

      [[nodiscard]] std::optional<core::SampleProject>
      find_by_id(const core::SampleProjectId& entity_id) override {
        try {
          const auto result = txn_.work().exec(
              "SELECT sample_id, project_id FROM sample_projects "
              "WHERE sample_id = $1 AND project_id = $2",
              pqxx::params{entity_id.sample_id.to_string(), entity_id.project_id.to_string()});
          if (result.empty()) {
            return std::nullopt;
          }
          return core::SampleProject{
              .sample_id = core::SampleId::parse(result[0].at("sample_id").as<std::string>()),
              .project_id = core::ProjectId::parse(result[0].at("project_id").as<std::string>()),
          };
        } catch (const pqxx::sql_error& err) {
          throw_pqxx_error(err);
        }
      }

      [[nodiscard]] std::vector<core::SampleProject>
      query(const Query<core::SampleProject>& query_spec) override {
        std::string sql = "SELECT sample_id, project_id FROM sample_projects";
        std::vector<nlohmann::json> parameters;
        detail::PostgresDialect dialect;
        detail::append_where(sql, parameters, {}, query_spec.predicates(),
                             detail::sample_project_column_name, dialect);
        detail::append_order_limit(sql, parameters, query_spec, detail::sample_project_column_name,
                                   dialect);
        try {
          const auto result = txn_.work().exec(sql, pg_bind_params(parameters));
          std::vector<core::SampleProject> results;
          for (pqxx::row_ref row : result) {
            results.push_back(core::SampleProject{
                .sample_id = core::SampleId::parse(row.at("sample_id").as<std::string>()),
                .project_id = core::ProjectId::parse(row.at("project_id").as<std::string>()),
            });
          }
          return results;
        } catch (const pqxx::sql_error& err) {
          throw_pqxx_error(err);
        }
      }

      void insert(const core::SampleProject& entity, const MutationContext& context) override {
        // Sample and project must belong to the same lab.
        if (txn_.work()
                .exec("SELECT 1 FROM samples s JOIN projects p ON s.lab_id = p.lab_id "
                      "WHERE s.id = $1 AND p.id = $2 LIMIT 1",
                      pqxx::params{entity.sample_id.to_string(), entity.project_id.to_string()})
                .empty()) {
          throw ForeignKeyViolation("sample and project do not belong to the same lab");
        }
        try {
          txn_.work().exec(
              "INSERT INTO sample_projects (sample_id, project_id) VALUES ($1, $2)",
              pqxx::params{entity.sample_id.to_string(), entity.project_id.to_string()});
        } catch (const pqxx::sql_error& err) {
          throw_pqxx_error(err);
        }
        txn_.note_mutation("sample_project",
                           entity.sample_id.to_string() + ":" + entity.project_id.to_string(),
                           context, "insert", detail::audit_after(entity));
      }

      void update(const core::SampleProject& /*entity*/,
                  const MutationContext& /*context*/) override {
        throw UnsupportedOperation("SampleProject links cannot be updated; remove and re-add");
      }

      void soft_delete(const core::SampleProjectId& entity_id,
                       const MutationContext& context) override {
        try {
          const auto result = txn_.work().exec(
              "DELETE FROM sample_projects WHERE sample_id = $1 AND project_id = $2",
              pqxx::params{entity_id.sample_id.to_string(), entity_id.project_id.to_string()});
          if (result.affected_rows() == 0) {
            throw NotFound("sample_project link not found");
          }
        } catch (const pqxx::sql_error& err) {
          throw_pqxx_error(err);
        }
        const core::SampleProject removed{.sample_id = entity_id.sample_id,
                                          .project_id = entity_id.project_id};
        txn_.note_mutation("sample_project",
                           entity_id.sample_id.to_string() + ":" + entity_id.project_id.to_string(),
                           context, "soft_delete", detail::audit_before(removed));
      }

    private:
      PostgresTransaction& txn_;
    };

    // ---- CheckoutEvent (append-only) ----

    constexpr const char* event_columns =
        "id, sample_id, lab_id, user_id, action, reason, at_micros, volume_delta, volume_unit, "
        "location_after";

    [[nodiscard]] core::CheckoutEvent read_event(pqxx::row_ref row) {
      return core::CheckoutEvent{
          .id = core::CheckoutEventId::parse(row.at("id").as<std::string>()),
          .sample_id = core::SampleId::parse(row.at("sample_id").as<std::string>()),
          .lab_id = core::LabId::parse(row.at("lab_id").as<std::string>()),
          .user_id = core::UserId::parse(row.at("user_id").as<std::string>()),
          .action = core::parse_checkout_action(row.at("action").as<std::string>()),
          .reason = pg_optional_string(row, "reason"),
          .at = core::Timestamp::from_unix_micros(row.at("at_micros").as<std::int64_t>()),
          .volume_delta = pg_optional_int64(row, "volume_delta"),
          .volume_unit = read_volume_unit(row, "volume_unit"),
          .location_after = pg_optional_string(row, "location_after"),
      };
    }

    class CheckoutEventRepository final : public IRepository<core::CheckoutEvent> {
    public:
      explicit CheckoutEventRepository(PostgresTransaction& txn) : txn_(txn) {}

      [[nodiscard]] std::optional<core::CheckoutEvent>
      find_by_id(const core::CheckoutEventId& entity_id) override {
        try {
          const auto result = txn_.work().exec(std::string("SELECT ") + event_columns +
                                                   " FROM checkout_events WHERE id = $1",
                                               pqxx::params{entity_id.to_string()});
          if (result.empty()) {
            return std::nullopt;
          }
          return read_event(result[0]);
        } catch (const pqxx::sql_error& err) {
          throw_pqxx_error(err);
        }
      }

      [[nodiscard]] std::vector<core::CheckoutEvent>
      query(const Query<core::CheckoutEvent>& query_spec) override {
        std::string sql = std::string("SELECT ") + event_columns + " FROM checkout_events";
        std::vector<nlohmann::json> parameters;
        detail::PostgresDialect dialect;
        detail::append_where(sql, parameters, {}, query_spec.predicates(),
                             detail::checkout_event_column_name, dialect);
        detail::append_order_limit(sql, parameters, query_spec, detail::checkout_event_column_name,
                                   dialect);
        try {
          const auto result = txn_.work().exec(sql, pg_bind_params(parameters));
          std::vector<core::CheckoutEvent> results;
          for (pqxx::row_ref row : result) {
            results.push_back(read_event(row));
          }
          return results;
        } catch (const pqxx::sql_error& err) {
          throw_pqxx_error(err);
        }
      }

      void insert(const core::CheckoutEvent& entity, const MutationContext& context) override {
        // lab_id must match the referenced sample's lab.
        if (txn_.work()
                .exec("SELECT 1 FROM samples WHERE id = $1 AND lab_id = $2 LIMIT 1",
                      pqxx::params{entity.sample_id.to_string(), entity.lab_id.to_string()})
                .empty()) {
          throw ConstraintViolation("checkout_event lab_id does not match the sample's lab");
        }
        try {
          pqxx::params params;
          params.append(entity.id.to_string());
          params.append(entity.sample_id.to_string());
          params.append(entity.lab_id.to_string());
          params.append(entity.user_id.to_string());
          params.append(std::string(core::to_string(entity.action)));
          params.append(entity.reason);
          params.append(entity.at.unix_micros());
          params.append(entity.volume_delta);
          params.append(volume_unit_text(entity.volume_unit));
          params.append(entity.location_after);
          txn_.work().exec("INSERT INTO checkout_events (id, sample_id, lab_id, user_id, action, "
                           "reason, at_micros, volume_delta, volume_unit, location_after) "
                           "VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10)",
                           params);
        } catch (const pqxx::sql_error& err) {
          throw_pqxx_error(err);
        }
        txn_.note_mutation("checkout_event", entity.id.to_string(), context, "insert",
                           detail::audit_after(entity));
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
      PostgresTransaction& txn_;
    };

  } // namespace

  void register_sample_repositories(PostgresBackend& backend) {
    backend.register_repository_factory<core::Sample>(
        [](PostgresTransaction& txn) { return std::make_unique<SampleRepository>(txn); });
    backend.register_repository_factory<core::Project>(
        [](PostgresTransaction& txn) { return std::make_unique<ProjectRepository>(txn); });
    backend.register_repository_factory<core::SampleProject>(
        [](PostgresTransaction& txn) { return std::make_unique<SampleProjectRepository>(txn); });
    backend.register_repository_factory<core::CheckoutEvent>(
        [](PostgresTransaction& txn) { return std::make_unique<CheckoutEventRepository>(txn); });
  }

} // namespace fmgr::storage
