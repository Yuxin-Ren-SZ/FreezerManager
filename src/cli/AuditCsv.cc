// SPDX-License-Identifier: AGPL-3.0-or-later

#include "cli/AuditCsv.h"

#include "cli/CsvWriter.h"

#include <nlohmann/json.hpp>

namespace fmgr::cli {

  namespace {

    // Stringify a JSON scalar for a CSV cell: null -> empty, string -> raw value,
    // everything else -> compact dump. `at` is an integer (UTC micros) and
    // before_json/after_json are already JSON-string columns, so each renders as
    // its literal text and round-trips for an external re-hash.
    [[nodiscard]] std::string json_scalar_to_string(const nlohmann::json& value) {
      if (value.is_null()) {
        return {};
      }
      if (value.is_string()) {
        return value.get<std::string>();
      }
      return value.dump();
    }

  } // namespace

  const std::vector<std::string>& audit_csv_columns() {
    // Mirrors core::to_json(AuditEvent) key order, including the hash-chain
    // columns so an external verifier can re-walk the chain from the CSV alone.
    static const std::vector<std::string> columns = {
        "id",          "at",          "actor_user_id", "actor_session_id",
        "lab_id",      "action",      "entity_kind",   "entity_id",
        "before_json", "after_json",  "request_id",    "prev_hash",
        "this_hash"};
    return columns;
  }

  std::vector<std::string> audit_event_to_csv_row(const core::AuditEvent& event) {
    const nlohmann::json json = event;
    std::vector<std::string> row;
    row.reserve(audit_csv_columns().size());
    for (const auto& column : audit_csv_columns()) {
      row.push_back(json_scalar_to_string(json.at(column)));
    }
    return row;
  }

  // Field order here is the CSV header contract, not accidental.
  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
  void write_audit_export_header(std::ostream& out, int schema_version,
                                 std::string_view lab_filter, std::string_view exported_at,
                                 std::size_t event_count, std::string_view signature) {
    out << "# freezermanager-audit-export schema_version=" << schema_version << "\r\n";
    out << "# lab_filter=" << lab_filter << " event_count=" << event_count << "\r\n";
    out << "# exported_at=" << exported_at << "\r\n";
    out << "# signature=" << signature << "\r\n";
  }

  void write_audit_csv(std::ostream& out, const std::vector<core::AuditEvent>& events,
                       int schema_version, std::string_view lab_filter,
                       std::string_view exported_at) {
    write_audit_export_header(out, schema_version, lab_filter, exported_at, events.size());
    write_csv_row(out, audit_csv_columns());
    for (const auto& event : events) {
      write_csv_row(out, audit_event_to_csv_row(event));
    }
  }

} // namespace fmgr::cli
