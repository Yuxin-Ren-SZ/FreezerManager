// SPDX-License-Identifier: AGPL-3.0-or-later

// Pure CSV-row -> core::Sample mapping for `freezerctl sample import`. No I/O and
// no DB access: build_import() performs only the structural validation that can
// be decided from the row text alone (required fields, UUID/enum parse,
// box/position pairing, well-formed custom_fields_json, and intra-file
// duplicate-position detection). Semantic checks that need committed state
// (item-type liveness, box existence, size-class compatibility, cross-row
// position uniqueness against the live DB) are left to the repository insert.
//
// Clock-free and id-injectable: the caller supplies `now`; sample ids are minted
// with core::generate_uuid_v4() so a successful row yields a ready-to-insert
// Sample. Server-managed CSV columns (id, lab_id, status, created_*,
// last_modified_*, phi_fields_enc_json) are ignored if present in the header.
#ifndef FMGR_CLI_SAMPLEIMPORT_H
#define FMGR_CLI_SAMPLEIMPORT_H

#include "core/ids.h"
#include "core/sample.h"
#include "core/timestamp.h"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace fmgr::cli {

  // Lab/actor/clock context applied to every imported row. The lab and actor are
  // server-supplied (CLI flags), never read from the CSV, so an import cannot
  // smuggle a row into another lab or forge authorship.
  struct ImportContext {
    core::LabId lab_id;
    core::UserId actor;
    core::Timestamp now;
  };

  // Outcome for one CSV data row (1-based, excluding the header and comments).
  struct ImportRowResult {
    std::size_t row_number{0};
    bool ok{false};
    std::string error;                  // empty iff ok
    std::optional<core::Sample> sample; // set iff ok
  };

  // Full per-file report. `header_error` is non-empty when the document is
  // unusable as a whole (no rows, or a header missing a required column); in that
  // case `rows` is empty.
  struct ImportReport {
    std::string header_error;
    std::vector<ImportRowResult> rows;

    [[nodiscard]] std::size_t ok_count() const;
    [[nodiscard]] std::size_t error_count() const;
    [[nodiscard]] bool has_errors() const; // header_error set OR any row failed
  };

  // Validate and map parsed CSV records. `records[0]` is the header row; the
  // remaining records are data rows. Returns one ImportRowResult per data row in
  // input order. Never throws for row-level problems — they become row errors.
  [[nodiscard]] ImportReport build_import(const std::vector<std::vector<std::string>>& records,
                                          const ImportContext& context);

} // namespace fmgr::cli

#endif // FMGR_CLI_SAMPLEIMPORT_H
