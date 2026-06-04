// SPDX-License-Identifier: AGPL-3.0-or-later

#ifndef FMGR_STORAGE_DETAIL_QUERYSQLBUILDER_H
#define FMGR_STORAGE_DETAIL_QUERYSQLBUILDER_H

#include "storage/IStorageBackend.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// Backend-neutral translation of the typed Query<Entity> DSL into a SQL string
// plus an ordered list of bind parameters (carried as nlohmann::json, the same
// representation both backends already use). Everything that differs between
// SQLite and PostgreSQL is delegated to a SqlDialect:
//   - parameter placeholder syntax (positional "?" vs ordinal "$N"),
//   - JSON-path equality rendering (json_extract vs jsonb_extract_path_text),
//   - LIMIT/OFFSET rendering.
// Each backend binds the returned parameters with its own driver.
namespace fmgr::storage::detail {

  // Dialect strategy. One instance is constructed per query; ordinal dialects
  // (Postgres) keep their own running placeholder counter, so placeholders must
  // be requested in the exact order their parameters are appended.
  class SqlDialect {
  public:
    SqlDialect() = default;
    virtual ~SqlDialect() = default;
    SqlDialect(const SqlDialect&) = delete;
    SqlDialect& operator=(const SqlDialect&) = delete;
    SqlDialect(SqlDialect&&) = delete;
    SqlDialect& operator=(SqlDialect&&) = delete;

    // Token for the next bind parameter (e.g. "?" or "$3"). Advances the
    // dialect's internal counter where applicable.
    [[nodiscard]] virtual std::string placeholder() = 0;

    // Render a JSON-path equality predicate `column <path> = value`. Appends the
    // bind value(s) it needs to `params` in placeholder order and returns the
    // SQL clause.
    [[nodiscard]] virtual std::string json_path_equal(const std::string& column,
                                                      const std::vector<std::string>& path,
                                                      const nlohmann::json& value,
                                                      std::vector<nlohmann::json>& params) = 0;

    // Append LIMIT / OFFSET to `sql`, pushing their bind values onto `params`.
    virtual void append_limit_offset(std::string& sql, std::vector<nlohmann::json>& params,
                                     std::optional<std::size_t> limit,
                                     std::optional<std::size_t> offset) = 0;
  };

  // SQLite dialect: positional "?" placeholders; JSON-path via json1's
  // json_extract(col, '$.a.b'); LIMIT/OFFSET with the "LIMIT -1" trick when an
  // offset is requested without a limit. Reproduces the historical SQL exactly.
  class SqliteDialect final : public SqlDialect {
  public:
    [[nodiscard]] std::string placeholder() override {
      return "?";
    }

    [[nodiscard]] std::string json_path_equal(const std::string& column,
                                              const std::vector<std::string>& path,
                                              const nlohmann::json& value,
                                              std::vector<nlohmann::json>& params) override {
      std::string json_pointer = "$";
      for (const auto& segment : path) {
        json_pointer += ".";
        json_pointer += segment;
      }
      params.emplace_back(json_pointer);
      params.push_back(value);
      return "json_extract(" + column + ", ?) = ?";
    }

    void append_limit_offset(std::string& sql, std::vector<nlohmann::json>& params,
                             std::optional<std::size_t> limit,
                             std::optional<std::size_t> offset) override {
      if (limit.has_value()) {
        sql += " LIMIT ?";
        params.emplace_back(static_cast<std::int64_t>(limit.value()));
      }
      if (offset.has_value()) {
        if (!limit.has_value()) {
          sql += " LIMIT -1";
        }
        sql += " OFFSET ?";
        params.emplace_back(static_cast<std::int64_t>(offset.value()));
      }
    }
  };

  // PostgreSQL dialect: ordinal "$N" placeholders; JSON-path via
  // jsonb_extract_path_text(col, 'a', 'b') (injection-safe, multi-level);
  // OFFSET is valid without a preceding LIMIT.
  class PostgresDialect final : public SqlDialect {
  public:
    [[nodiscard]] std::string placeholder() override {
      return "$" + std::to_string(++ordinal_);
    }

    [[nodiscard]] std::string json_path_equal(const std::string& column,
                                              const std::vector<std::string>& path,
                                              const nlohmann::json& value,
                                              std::vector<nlohmann::json>& params) override {
      std::string clause = "jsonb_extract_path_text(" + column;
      for (const auto& segment : path) {
        clause += ", " + placeholder();
        params.emplace_back(segment);
      }
      clause += ") = " + placeholder();
      params.push_back(value);
      return clause;
    }

    void append_limit_offset(std::string& sql, std::vector<nlohmann::json>& params,
                             std::optional<std::size_t> limit,
                             std::optional<std::size_t> offset) override {
      if (limit.has_value()) {
        sql += " LIMIT " + placeholder();
        params.emplace_back(static_cast<std::int64_t>(limit.value()));
      }
      if (offset.has_value()) {
        sql += " OFFSET " + placeholder();
        params.emplace_back(static_cast<std::int64_t>(offset.value()));
      }
    }

  private:
    int ordinal_{0};
  };

  // Append a " WHERE ..." clause built from the soft-delete default predicates
  // (already rendered SQL fragments) plus the typed DSL predicates. `column_name`
  // maps an Entity::Field to its column string (shared by both backends).
  template <typename Entity, typename ColumnName>
  void append_where(std::string& sql, std::vector<nlohmann::json>& params,
                    const std::vector<std::string>& default_clauses,
                    const std::vector<Predicate<Entity>>& predicates, ColumnName column_name,
                    SqlDialect& dialect) {
    std::vector<std::string> clauses = default_clauses;
    for (const auto& predicate : predicates) {
      const auto column = column_name(predicate.field);
      switch (predicate.op) {
      case PredicateOperator::Equal:
        clauses.push_back(column + " = " + dialect.placeholder());
        params.push_back(predicate.value);
        break;
      case PredicateOperator::GreaterThanOrEqual:
        clauses.push_back(column + " >= " + dialect.placeholder());
        params.push_back(predicate.value);
        break;
      case PredicateOperator::LessThanOrEqual:
        clauses.push_back(column + " <= " + dialect.placeholder());
        params.push_back(predicate.value);
        break;
      case PredicateOperator::Between: {
        std::string clause = column;
        clause += " BETWEEN ";
        clause += dialect.placeholder();
        clause += " AND ";
        clause += dialect.placeholder();
        clauses.push_back(std::move(clause));
        params.push_back(predicate.lower);
        params.push_back(predicate.upper);
        break;
      }
      case PredicateOperator::In: {
        std::string clause = column + " IN (";
        for (std::size_t index = 0; index < predicate.values.size(); ++index) {
          if (index != 0) {
            clause += ", ";
          }
          clause += dialect.placeholder();
          params.push_back(predicate.values.at(index));
        }
        clause += ")";
        clauses.push_back(std::move(clause));
        break;
      }
      case PredicateOperator::JsonPathEqual:
        clauses.push_back(
            dialect.json_path_equal(column, predicate.json_path, predicate.value, params));
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

  // Append " ORDER BY ... LIMIT ... OFFSET ..." for the query's sort and
  // pagination spec.
  template <typename Entity, typename ColumnName>
  void append_order_limit(std::string& sql, std::vector<nlohmann::json>& params,
                          const Query<Entity>& query_spec, ColumnName column_name,
                          SqlDialect& dialect) {
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
    dialect.append_limit_offset(sql, params, query_spec.limit_count(), query_spec.offset_count());
  }

} // namespace fmgr::storage::detail

#endif // FMGR_STORAGE_DETAIL_QUERYSQLBUILDER_H
