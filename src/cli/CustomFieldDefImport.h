// SPDX-License-Identifier: AGPL-3.0-or-later

// Pure CSV-row -> core::CustomFieldDefinition mapping for
// `freezerctl custom-field-def import`. No I/O and no DB: structural validation
// only (required scope_kind/key/label/data_type; enum parse; well-formed optional
// item_type_id UUID; validation_json must be a JSON object; the PHI-fields-are-
// never-indexed invariant). Inheritance/key-uniqueness and cross-lab integrity
// are left to the repository insert. Server-managed columns (id, lab_id,
// created_at, archived_at) are ignored if present.
#ifndef FMGR_CLI_CUSTOMFIELDDEFIMPORT_H
#define FMGR_CLI_CUSTOMFIELDDEFIMPORT_H

#include "cli/CsvImport.h"
#include "core/ids.h"
#include "core/item_type.h"
#include "core/timestamp.h"

#include <string>
#include <vector>

namespace fmgr::cli {

  [[nodiscard]] EntityImportReport<core::CustomFieldDefinition>
  build_custom_field_def_import(const std::vector<std::vector<std::string>>& records,
                                core::LabId lab_id, core::Timestamp now);

} // namespace fmgr::cli

#endif // FMGR_CLI_CUSTOMFIELDDEFIMPORT_H
