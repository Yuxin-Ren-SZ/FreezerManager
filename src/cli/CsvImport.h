// SPDX-License-Identifier: AGPL-3.0-or-later

// Generic CSV-import machinery shared by the non-sample import nouns
// (`freezerctl <noun> import`), factored to mirror SampleImport's contract
// without re-deriving the transactional runner per entity (PRD §13).
//
// An entity supplies a pure `build_fn(records, ImportContext) ->
// EntityImportReport<T>` that maps/validates rows using only the row text
// (RowError per bad row, never throwing for row-level problems). The templated
// run_entity_import() then applies the same all-or-nothing / --dry-run policy as
// `sample import`: a structural gate first (any row error ⇒ nothing written),
// then either per-row rollback validation against committed DB state, or a single
// committed transaction. Lab and actor are server-supplied, never read from the
// CSV, so an import cannot smuggle a row into another lab or forge authorship.
#ifndef FMGR_CLI_CSVIMPORT_H
#define FMGR_CLI_CSVIMPORT_H

#include "cli/CsvReader.h"
#include "core/ids.h"
#include "core/timestamp.h"
#include "storage/IStorageBackend.h"

#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <istream>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fmgr::cli {

  // Row-local validation failure; importers throw it from their mapper and the
  // runner turns it into a per-row error so one bad row never aborts the file.
  struct RowError {
    std::string message;
  };

  // Lookup from header column name to its index. Duplicate headers keep the last.
  using HeaderIndex = std::unordered_map<std::string, std::size_t>;

  [[nodiscard]] inline HeaderIndex index_header(const std::vector<std::string>& header_row) {
    HeaderIndex header;
    for (std::size_t col = 0; col < header_row.size(); ++col) {
      header.emplace(header_row.at(col), col);
    }
    return header;
  }

  // Trimmed cell value for `column`, or nullopt when the column is absent or the
  // cell is empty. Throws RowError on a short row (fewer cells than header).
  [[nodiscard]] inline std::optional<std::string>
  cell(const HeaderIndex& header, const std::vector<std::string>& row, const std::string& column) {
    const auto iter = header.find(column);
    if (iter == header.end()) {
      return std::nullopt;
    }
    if (iter->second >= row.size()) {
      throw RowError{"row has fewer columns than the header"};
    }
    const std::string& raw = row.at(iter->second);
    if (raw.empty()) {
      return std::nullopt;
    }
    return raw;
  }

  // Parse a strong id from a cell, mapping any parse failure to a RowError naming
  // the column. T must expose a static T::parse(std::string).
  template <typename T>
  [[nodiscard]] T parse_id(const std::string& text, const std::string& column) {
    try {
      return T::parse(text);
    } catch (const std::exception&) {
      throw RowError{column + ": invalid UUID: '" + text + "'"};
    }
  }

  // Outcome for one CSV data row (1-based, excluding header and comments).
  template <typename T> struct EntityImportRow {
    std::size_t row_number{0};
    bool ok{false};
    std::string error;       // empty iff ok
    std::optional<T> entity; // set iff ok
  };

  // Full per-file report. `header_error` non-empty ⇒ document unusable, rows empty.
  template <typename T> struct EntityImportReport {
    std::string header_error;
    std::vector<EntityImportRow<T>> rows;

    [[nodiscard]] std::size_t error_count() const {
      std::size_t count = 0;
      for (const auto& row : rows) {
        if (!row.ok) {
          ++count;
        }
      }
      return count;
    }
    [[nodiscard]] bool has_errors() const {
      return !header_error.empty() || error_count() > 0;
    }
  };

  // Backend-facing options for a non-sample import command.
  struct EntityImportOptions {
    core::LabId lab_id;
    core::UserId actor;
    bool dry_run{false};
  };

  namespace import_detail {

    [[nodiscard]] inline core::Timestamp now_timestamp() {
      const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(
                              std::chrono::system_clock::now().time_since_epoch())
                              .count();
      return core::Timestamp::from_unix_micros(static_cast<std::int64_t>(micros));
    }

    [[nodiscard]] inline storage::MutationContext import_context(const EntityImportOptions& opts) {
      return storage::MutationContext{
          .actor_user_id = opts.actor,
          .actor_session_id = "freezerctl-import",
          .request_id = "",
          .reason = "csv_import",
          .lab_id = opts.lab_id.to_string(),
      };
    }

  } // namespace import_detail

  // Transactional runner shared by every non-sample import noun. `build_fn` maps
  // parsed records to an EntityImportReport<T>; the runner enforces the structural
  // gate, prints a per-row report, and persists per the dry-run / commit policy.
  // Returns the process exit code. Mirrors run_sample_import.
  // `noun_plural` is the verbatim plural shown in the success line (e.g.
  // "box(es)", "item-type(s)") — English pluralization is the caller's call.
  template <typename T, typename BuildFn>
  int run_entity_import(storage::IStorageBackend& backend, const EntityImportOptions& options,
                        BuildFn build_fn, std::istream& input, std::ostream& out,
                        const char* noun_plural) {
    const auto records = parse_csv(input); // CsvParseError propagates to run_cli's err handler
    const EntityImportReport<T> report =
        build_fn(records, options.lab_id, import_detail::now_timestamp());

    if (!report.header_error.empty()) {
      out << "error: " << report.header_error << '\n';
      return 1;
    }

    for (const auto& row : report.rows) {
      if (row.ok) {
        out << "row " << row.row_number << ": OK\n";
      } else {
        out << "row " << row.row_number << ": ERROR " << row.error << '\n';
      }
    }
    if (report.has_errors()) {
      out << report.error_count() << " row(s) failed validation; nothing written\n";
      return 1;
    }

    const auto inject_lab = [&](storage::ITransaction& txn) {
      txn.set_session_var("current_lab_ids", options.lab_id.to_string());
    };
    const auto ctx = import_detail::import_context(options);

    if (options.dry_run) {
      // Validate against committed state without persisting: each row in its own
      // rolled-back transaction so one failing row never poisons the others.
      std::size_t db_errors = 0;
      for (const auto& row : report.rows) {
        if (!row.entity.has_value()) {
          continue;
        }
        try {
          auto txn = backend.begin(storage::IsolationLevel::Serializable);
          inject_lab(*txn);
          txn->template repo<T>().insert(row.entity.value(), ctx);
          // No commit(): the transaction rolls back on destruction.
        } catch (const std::exception& error) {
          ++db_errors;
          out << "row " << row.row_number << ": ERROR " << error.what() << '\n';
        }
      }
      if (db_errors > 0) {
        out << db_errors << " row(s) failed validation; dry-run, nothing written\n";
        return 1;
      }
      out << "dry-run OK: " << report.rows.size() << " row(s) valid; nothing written\n";
      return 0;
    }

    // Real import: all-or-nothing in a single transaction.
    auto txn = backend.begin(storage::IsolationLevel::Serializable);
    inject_lab(*txn);
    for (const auto& row : report.rows) {
      if (!row.entity.has_value()) {
        continue;
      }
      txn->template repo<T>().insert(row.entity.value(), ctx);
    }
    txn->commit();
    out << "imported " << report.rows.size() << ' ' << noun_plural << '\n';
    return 0;
  }

} // namespace fmgr::cli

#endif // FMGR_CLI_CSVIMPORT_H
