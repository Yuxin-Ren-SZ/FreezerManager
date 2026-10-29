// SPDX-License-Identifier: AGPL-3.0-or-later

#ifndef FMGR_STORAGE_POSTGRES_POSTGRESREPOSUPPORT_H
#define FMGR_STORAGE_POSTGRES_POSTGRESREPOSUPPORT_H

#include "core/placement.h"
#include "core/timestamp.h"
#include "storage/IStorageBackend.h"

#include <pqxx/pqxx>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

// Shared helpers for the PostgreSQL domain repositories: SQLSTATE→exception
// mapping, conversion of the backend-neutral query-builder parameter list
// (std::vector<nlohmann::json>) into a pqxx::params, and small read helpers.
namespace fmgr::storage::detail {

  // Audit "after" snapshot for an insert/update: the post-write entity state.
  template <typename Entity> [[nodiscard]] AuditSnapshot audit_after(const Entity& entity) {
    return AuditSnapshot{.after = nlohmann::json(entity)};
  }

  // Audit "before"+"after" snapshot for an update/soft-delete. `before` is the
  // authoritative prior row (nullopt → no before image, e.g. it did not exist).
  template <typename Entity>
  [[nodiscard]] AuditSnapshot audit_change(const std::optional<Entity>& before,
                                           const Entity& after) {
    AuditSnapshot snapshot;
    if (before.has_value()) {
      snapshot.before = nlohmann::json(before.value());
    }
    snapshot.after = nlohmann::json(after);
    return snapshot;
  }

  // Audit "before"-only snapshot for a hard delete (no post-write state).
  template <typename Entity> [[nodiscard]] AuditSnapshot audit_before(const Entity& before) {
    return AuditSnapshot{.before = nlohmann::json(before)};
  }

  // Map a libpqxx SQL error onto the portable BackendError hierarchy. Mirrors the
  // mapping used by PostgresBackend's audit/migration paths so callers see the
  // same error codes regardless of which layer raised them.
  //
  // error.what() is PQresultErrorMessage(): a schema object name on the first
  // line and the colliding row values in DETAIL. It is engine text, so it goes
  // into BackendText and never onto the wire (#123).
  [[noreturn]] inline void throw_pqxx_error(const pqxx::sql_error& error) {
    const BackendText engine_text{error.what()};
    const std::string_view state = error.sqlstate();
    if (state == "23505") {
      throw UniqueViolation(engine_text);
    }
    if (state == "23503") {
      throw ForeignKeyViolation(engine_text);
    }
    if (state == "23514" || state == "23502" || state == "23000") {
      throw ConstraintViolation(engine_text);
    }
    if (state == "40001" || state == "40P01") {
      throw SerializationFailure(engine_text);
    }
    if (state.starts_with("08")) {
      throw Unavailable(engine_text);
    }
    if (state == "57P01" || state == "55P03") {
      throw Unavailable(engine_text);
    }
    throw ConstraintViolation(engine_text);
  }

  // Append one query-builder parameter (carried as nlohmann::json) to a
  // pqxx::params, preserving the SQL type. Null binds as a SQL NULL.
  inline void pg_append_json_param(pqxx::params& params, const nlohmann::json& value) {
    if (value.is_string()) {
      params.append(value.get<std::string>());
    } else if (value.is_number_integer()) {
      params.append(value.get<std::int64_t>());
    } else if (value.is_boolean()) {
      params.append(value.get<bool>());
    } else if (value.is_null()) {
      params.append(std::optional<std::string>{});
    } else {
      params.append(value.dump());
    }
  }

  // Convert the whole builder parameter list into a pqxx::params.
  [[nodiscard]] inline pqxx::params pg_bind_params(const std::vector<nlohmann::json>& parameters) {
    pqxx::params params;
    for (const auto& parameter : parameters) {
      pg_append_json_param(params, parameter);
    }
    return params;
  }

  // Read a nullable text column as std::optional<std::string>.
  [[nodiscard]] inline std::optional<std::string> pg_optional_string(pqxx::row_ref row,
                                                                     const char* column) {
    const auto field = row.at(column);
    if (field.is_null()) {
      return std::nullopt;
    }
    return field.as<std::string>();
  }

  // Read a nullable strong-id column (stored as TEXT) as std::optional<StrongId>.
  template <typename StrongId>
  [[nodiscard]] std::optional<StrongId> pg_optional_id(pqxx::row_ref row, const char* column) {
    const auto text = pg_optional_string(row, column);
    if (!text.has_value()) {
      return std::nullopt;
    }
    return StrongId::parse(text.value());
  }

  // Read a nullable microsecond-timestamp column as std::optional<Timestamp>.
  [[nodiscard]] inline std::optional<core::Timestamp> pg_optional_timestamp(pqxx::row_ref row,
                                                                            const char* column) {
    const auto field = row.at(column);
    if (field.is_null()) {
      return std::nullopt;
    }
    return core::Timestamp::from_unix_micros(field.as<std::int64_t>());
  }

  // Read the nullable placement_level/_column/_depth triple (ADR 0003). The schema
  // CHECK keeps it all-null or all-set, so the level decides.
  [[nodiscard]] inline std::optional<core::Placement> pg_optional_placement(pqxx::row_ref row) {
    const auto level = row.at("placement_level");
    if (level.is_null()) {
      return std::nullopt;
    }
    return core::Placement{
        .level = level.as<std::int32_t>(),
        .column = row.at("placement_column").as<std::int32_t>(),
        .depth = row.at("placement_depth").as<std::int32_t>(),
    };
  }

  // Read the nullable grid_levels/_columns/_depths triple.
  [[nodiscard]] inline std::optional<core::GridSpec> pg_optional_grid(pqxx::row_ref row) {
    const auto levels = row.at("grid_levels");
    if (levels.is_null()) {
      return std::nullopt;
    }
    return core::GridSpec{
        .levels = levels.as<std::int32_t>(),
        .columns = row.at("grid_columns").as<std::int32_t>(),
        .depths = row.at("grid_depths").as<std::int32_t>(),
    };
  }

  // Append a placement as three parameters (three NULLs when unset).
  inline void pg_append_placement(pqxx::params& params,
                                  const std::optional<core::Placement>& placement) {
    const auto unset = std::optional<std::int32_t>{};
    params.append(placement.has_value() ? std::optional{placement->level} : unset);
    params.append(placement.has_value() ? std::optional{placement->column} : unset);
    params.append(placement.has_value() ? std::optional{placement->depth} : unset);
  }

  // Append a grid as three parameters (three NULLs when unset).
  inline void pg_append_grid(pqxx::params& params, const std::optional<core::GridSpec>& grid) {
    const auto unset = std::optional<std::int32_t>{};
    params.append(grid.has_value() ? std::optional{grid->levels} : unset);
    params.append(grid.has_value() ? std::optional{grid->columns} : unset);
    params.append(grid.has_value() ? std::optional{grid->depths} : unset);
  }

  // Convert an optional timestamp to its microsecond value for binding (NULL if empty).
  [[nodiscard]] inline std::optional<std::int64_t>
  micros_or_null(const std::optional<core::Timestamp>& timestamp) {
    if (!timestamp.has_value()) {
      return std::nullopt;
    }
    return timestamp->unix_micros();
  }

  // Convert an optional strong id to its string value for binding (NULL if empty).
  template <typename StrongId>
  [[nodiscard]] std::optional<std::string> id_or_null(const std::optional<StrongId>& strong_id) {
    if (!strong_id.has_value()) {
      return std::nullopt;
    }
    return strong_id->to_string();
  }

} // namespace fmgr::storage::detail

#endif // FMGR_STORAGE_POSTGRES_POSTGRESREPOSUPPORT_H
