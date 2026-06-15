// SPDX-License-Identifier: AGPL-3.0-or-later

#include "cli/BoxImport.h"

#include "core/uuid.h"

#include <optional>
#include <string>

namespace fmgr::cli {

  namespace {

    [[nodiscard]] core::Box row_to_box(const HeaderIndex& header,
                                       const std::vector<std::string>& row, core::LabId lab_id,
                                       core::Timestamp now) {
      const auto label = cell(header, row, "label");
      if (!label.has_value()) {
        throw RowError{"label: required and must be non-empty"};
      }
      const auto box_type = cell(header, row, "box_type_id");
      if (!box_type.has_value()) {
        throw RowError{"box_type_id: required"};
      }
      const auto container = cell(header, row, "storage_container_id");
      if (!container.has_value()) {
        throw RowError{"storage_container_id: required"};
      }

      core::Box box{};
      box.id = core::BoxId::parse(core::generate_uuid_v4());
      box.lab_id = lab_id;
      box.box_type_id = parse_id<core::BoxTypeId>(box_type.value(), "box_type_id");
      box.storage_container_id =
          parse_id<core::StorageContainerId>(container.value(), "storage_container_id");
      box.label = label.value();
      box.serial = cell(header, row, "serial");
      box.barcode = cell(header, row, "barcode");
      box.created_at = now;
      return box;
    }

  } // namespace

  EntityImportReport<core::Box>
  build_box_import(const std::vector<std::vector<std::string>>& records, core::LabId lab_id,
                   core::Timestamp now) {
    EntityImportReport<core::Box> report;
    if (records.empty()) {
      report.header_error = "empty CSV: no header row";
      return report;
    }
    const HeaderIndex header = index_header(records.front());
    if (!header.contains("label") || !header.contains("box_type_id") ||
        !header.contains("storage_container_id")) {
      report.header_error =
          "header must contain 'label', 'box_type_id', and 'storage_container_id' columns";
      return report;
    }

    for (std::size_t index = 1; index < records.size(); ++index) {
      EntityImportRow<core::Box> result;
      result.row_number = index;
      try {
        result.entity = row_to_box(header, records.at(index), lab_id, now);
        result.ok = true;
      } catch (const RowError& error) {
        result.error = error.message;
      }
      report.rows.push_back(std::move(result));
    }
    return report;
  }

} // namespace fmgr::cli
