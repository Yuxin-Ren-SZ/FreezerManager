// SPDX-License-Identifier: AGPL-3.0-or-later

// Pure CSV-row -> core::ItemType mapping for `freezerctl item-type import`. No
// I/O and no DB: structural validation only (required `name`, well-formed
// optional `parent_id` UUID). Liveness of the parent and cross-lab integrity are
// left to the repository insert. Server-managed columns (id, lab_id, created_at,
// archived_at) are ignored if present, so a file cannot forge ownership.
#ifndef FMGR_CLI_ITEMTYPEIMPORT_H
#define FMGR_CLI_ITEMTYPEIMPORT_H

#include "cli/CsvImport.h"
#include "core/ids.h"
#include "core/item_type.h"
#include "core/timestamp.h"

#include <string>
#include <vector>

namespace fmgr::cli {

  [[nodiscard]] EntityImportReport<core::ItemType>
  build_item_type_import(const std::vector<std::vector<std::string>>& records, core::LabId lab_id,
                         core::Timestamp now);

} // namespace fmgr::cli

#endif // FMGR_CLI_ITEMTYPEIMPORT_H
