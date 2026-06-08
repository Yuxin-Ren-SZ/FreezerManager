// SPDX-License-Identifier: AGPL-3.0-or-later

// Maps core::Sample rows to the CSV column model used by `freezerctl sample
// export`. Pure and clock-free so it is fully unit-testable: the export
// timestamp is supplied by the caller.
#ifndef FMGR_CLI_SAMPLECSV_H
#define FMGR_CLI_SAMPLECSV_H

#include "core/sample.h"

#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace fmgr::cli {

  // Ordered CSV column names for a sample export. Drives both the header row and
  // each data row so they stay in lockstep. The encrypted PHI blob
  // (phi_fields_enc_json) is intentionally excluded — PHI must not travel in a
  // report-grade CSV even as ciphertext.
  [[nodiscard]] const std::vector<std::string>& sample_csv_columns();

  // The single data row for one sample, in sample_csv_columns() order. Absent
  // optional fields render as empty strings.
  [[nodiscard]] std::vector<std::string> sample_to_csv_row(const core::Sample& sample);

  // Chain-of-custody header comment block (PRD §13), emitted before the CSV
  // header row. `signature` is "UNSIGNED" until KMS signing lands (M5).
  void write_export_header(std::ostream& out, int schema_version, std::string_view lab_id,
                           std::string_view exported_at, std::string_view signature = "UNSIGNED");

  // Full export: header comment block, CSV header row, then one row per sample.
  void write_sample_csv(std::ostream& out, const std::vector<core::Sample>& samples,
                        int schema_version, std::string_view lab_id, std::string_view exported_at);

} // namespace fmgr::cli

#endif // FMGR_CLI_SAMPLECSV_H
