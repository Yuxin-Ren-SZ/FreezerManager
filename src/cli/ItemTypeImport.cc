// SPDX-License-Identifier: AGPL-3.0-or-later

#include "cli/ItemTypeImport.h"

#include "core/uuid.h"

#include <optional>
#include <string>

namespace fmgr::cli {

  namespace {

    [[nodiscard]] core::ItemType row_to_item_type(const HeaderIndex& header,
                                                  const std::vector<std::string>& row,
                                                  core::LabId lab_id, core::Timestamp now) {
      const auto name = cell(header, row, "name");
      if (!name.has_value()) {
        throw RowError{"name: required and must be non-empty"};
      }

      core::ItemType item_type{};
      item_type.id = core::ItemTypeId::parse(core::generate_uuid_v4());
      item_type.lab_id = lab_id;
      item_type.name = name.value();
      item_type.created_at = now;

      if (const auto parent = cell(header, row, "parent_id"); parent.has_value()) {
        item_type.parent_id = parse_id<core::ItemTypeId>(parent.value(), "parent_id");
      }
      return item_type;
    }

  } // namespace

  EntityImportReport<core::ItemType>
  build_item_type_import(const std::vector<std::vector<std::string>>& records, core::LabId lab_id,
                         core::Timestamp now) {
    EntityImportReport<core::ItemType> report;
    if (records.empty()) {
      report.header_error = "empty CSV: no header row";
      return report;
    }
    const HeaderIndex header = index_header(records.front());
    if (!header.contains("name")) {
      report.header_error = "header must contain a 'name' column";
      return report;
    }

    for (std::size_t index = 1; index < records.size(); ++index) {
      EntityImportRow<core::ItemType> result;
      result.row_number = index;
      try {
        result.entity = row_to_item_type(header, records.at(index), lab_id, now);
        result.ok = true;
      } catch (const RowError& error) {
        result.error = error.message;
      }
      report.rows.push_back(std::move(result));
    }
    return report;
  }

} // namespace fmgr::cli
