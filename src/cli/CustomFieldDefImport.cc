// SPDX-License-Identifier: AGPL-3.0-or-later

#include "cli/CustomFieldDefImport.h"

#include "core/enums.h"
#include "core/uuid.h"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>

namespace fmgr::cli {

  namespace {

    [[nodiscard]] core::CustomFieldDefinition row_to_cfd(const HeaderIndex& header,
                                                         const std::vector<std::string>& row,
                                                         core::LabId lab_id, core::Timestamp now) {
      const auto scope = cell(header, row, "scope_kind");
      if (!scope.has_value()) {
        throw RowError{"scope_kind: required"};
      }
      const auto key = cell(header, row, "key");
      if (!key.has_value()) {
        throw RowError{"key: required and must be non-empty"};
      }
      const auto label = cell(header, row, "label");
      if (!label.has_value()) {
        throw RowError{"label: required and must be non-empty"};
      }
      const auto data_type = cell(header, row, "data_type");
      if (!data_type.has_value()) {
        throw RowError{"data_type: required"};
      }

      core::CustomFieldDefinition cfd{};
      cfd.id = core::CustomFieldDefinitionId::parse(core::generate_uuid_v4());
      cfd.lab_id = lab_id;
      cfd.key = key.value();
      cfd.label = label.value();
      cfd.created_at = now;

      try {
        cfd.scope_kind = core::parse_scope_kind(scope.value());
      } catch (const std::exception&) {
        throw RowError{"scope_kind: unknown value: '" + scope.value() + "'"};
      }
      try {
        cfd.data_type = core::parse_field_data_type(data_type.value());
      } catch (const std::exception&) {
        throw RowError{"data_type: unknown value: '" + data_type.value() + "'"};
      }

      if (const auto text = cell(header, row, "item_type_id"); text.has_value()) {
        cfd.item_type_id = parse_id<core::ItemTypeId>(text.value(), "item_type_id");
      }
      if (const auto text = cell(header, row, "required"); text.has_value()) {
        cfd.required = parse_bool(text.value(), "required");
      }
      if (const auto text = cell(header, row, "indexed"); text.has_value()) {
        cfd.indexed = parse_bool(text.value(), "indexed");
      }
      if (const auto text = cell(header, row, "is_phi"); text.has_value()) {
        cfd.is_phi = parse_bool(text.value(), "is_phi");
      }
      if (const auto text = cell(header, row, "validation_json"); text.has_value()) {
        const auto parsed = nlohmann::json::parse(text.value(), nullptr, false);
        if (parsed.is_discarded() || !parsed.is_object()) {
          throw RowError{"validation_json: must be a JSON object"};
        }
        cfd.validation_json = text.value();
      }

      // Mirror the domain invariant up front so the importer reports it per-row
      // rather than surfacing it as an opaque DB constraint failure.
      if (cfd.is_phi && cfd.indexed) {
        throw RowError{"is_phi and indexed are mutually exclusive (PHI must never be indexed)"};
      }
      return cfd;
    }

  } // namespace

  EntityImportReport<core::CustomFieldDefinition>
  build_custom_field_def_import(const std::vector<std::vector<std::string>>& records,
                                core::LabId lab_id, core::Timestamp now) {
    EntityImportReport<core::CustomFieldDefinition> report;
    if (records.empty()) {
      report.header_error = "empty CSV: no header row";
      return report;
    }
    const HeaderIndex header = index_header(records.front());
    if (!header.contains("scope_kind") || !header.contains("key") || !header.contains("label") ||
        !header.contains("data_type")) {
      report.header_error =
          "header must contain 'scope_kind', 'key', 'label', and 'data_type' columns";
      return report;
    }

    for (std::size_t index = 1; index < records.size(); ++index) {
      EntityImportRow<core::CustomFieldDefinition> result;
      result.row_number = index;
      try {
        result.entity = row_to_cfd(header, records.at(index), lab_id, now);
        result.ok = true;
      } catch (const RowError& error) {
        result.error = error.message;
      }
      report.rows.push_back(std::move(result));
    }
    return report;
  }

} // namespace fmgr::cli
