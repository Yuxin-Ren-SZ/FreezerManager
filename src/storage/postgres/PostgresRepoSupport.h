// SPDX-License-Identifier: AGPL-3.0-or-later

#ifndef FMGR_STORAGE_POSTGRES_POSTGRESREPOSUPPORT_H
#define FMGR_STORAGE_POSTGRES_POSTGRESREPOSUPPORT_H

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
  [[noreturn]] inline void throw_pqxx_error(const pqxx::sql_error& error) {
    const std::string_view state = error.sqlstate();
    if (state == "23505") {
      throw UniqueViolation(error.what());
    }
    if (state == "23503") {
      throw ForeignKeyViolation(error.what());
    }
    if (state == "23514" || state == "23502" || state == "23000") {
      throw ConstraintViolation(error.what());
    }
    if (state == "40001" || state == "40P01") {
      throw SerializationFailure(error.what());
    }
    if (state.starts_with("08")) {
      throw Unavailable(error.what());
    }
    if (state == "57P01") {
      throw Unavailable(error.what());
    }
    throw ConstraintViolation(error.what());
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
