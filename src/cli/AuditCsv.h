// SPDX-License-Identifier: AGPL-3.0-or-later

// Maps core::AuditEvent rows to the CSV column model used by `freezerctl audit
// export` (PRD §7.3). Pure and clock-free so it is fully unit-testable: the
// export timestamp is supplied by the caller.
//
// Unlike the sample export, the audit export deliberately includes the
// prev_hash / this_hash chain columns: a chain-of-custody audit export must let
// an external verifier re-walk the hash chain offline, independently of the
// live database.
#ifndef FMGR_CLI_AUDITCSV_H
#define FMGR_CLI_AUDITCSV_H

#include "core/audit_event.h"

#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace fmgr::cli {

  // Ordered CSV column names for an audit export. Drives both the header row and
  // each data row so they stay in lockstep. Mirrors core::to_json(AuditEvent)
  // key order and includes the hash-chain columns.
  [[nodiscard]] const std::vector<std::string>& audit_csv_columns();

  // The single data row for one audit event, in audit_csv_columns() order.
  // Absent optional fields (lab_id, entity_id) render as empty strings; `at`
  // renders as raw UTC microseconds for exact, reproducible re-hashing.
  [[nodiscard]] std::vector<std::string> audit_event_to_csv_row(const core::AuditEvent& event);

  // Chain-of-custody header comment block (PRD §13), emitted before the CSV
  // header row. `lab_filter` is the lab the export was scoped to, or "all" for a
  // global export. `signature` is "UNSIGNED" until KMS checkpoint signing lands.
  void write_audit_export_header(std::ostream& out, int schema_version,
                                 std::string_view lab_filter, std::string_view exported_at,
                                 std::size_t event_count, std::string_view signature = "UNSIGNED");

  // Full export: header comment block, CSV header row, then one row per event.
  void write_audit_csv(std::ostream& out, const std::vector<core::AuditEvent>& events,
                       int schema_version, std::string_view lab_filter,
                       std::string_view exported_at);

} // namespace fmgr::cli

#endif // FMGR_CLI_AUDITCSV_H
