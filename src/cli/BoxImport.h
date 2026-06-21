// SPDX-License-Identifier: AGPL-3.0-or-later

// Pure CSV-row -> core::Box mapping for `freezerctl box import`. No I/O and no DB:
// structural validation only (required `label`, `box_type_id`,
// `storage_container_id`; well-formed UUIDs; optional serial/barcode). Existence
// of the box-type and container, and cross-lab integrity, are left to the
// repository insert. Server-managed columns (id, lab_id, created_at, archived_at)
// are ignored if present, so a file cannot forge ownership.
#ifndef FMGR_CLI_BOXIMPORT_H
#define FMGR_CLI_BOXIMPORT_H

#include "cli/CsvImport.h"
#include "core/box.h"
#include "core/ids.h"
#include "core/timestamp.h"

#include <string>
#include <vector>

namespace fmgr::cli {

  [[nodiscard]] EntityImportReport<core::Box>
  build_box_import(const std::vector<std::vector<std::string>>& records, core::LabId lab_id,
                   core::Timestamp now);

} // namespace fmgr::cli

#endif // FMGR_CLI_BOXIMPORT_H
