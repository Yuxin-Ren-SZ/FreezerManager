// SPDX-License-Identifier: AGPL-3.0-or-later

#include "cli/SampleImport.h"

#include "core/enums.h"
#include "core/quantity.h"
#include "core/uuid.h"

#include <nlohmann/json.hpp>

#include <charconv>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace fmgr::cli {

  namespace {

    // Row-local validation failure; caught once per row and turned into an
    // ImportRowResult error so one bad row never aborts the whole file.
    struct RowError {
      std::string message;
    };

    [[nodiscard]] std::int64_t parse_int64(std::string_view text, std::string_view field) {
      std::int64_t value = 0;
      const auto* begin = text.data();
      const auto* end = text.data() + text.size();
      const auto [ptr, errc] = std::from_chars(begin, end, value);
      if (errc != std::errc{} || ptr != end) {
        throw RowError{std::string(field) + ": not an integer: '" + std::string(text) + "'"};
      }
      return value;
    }

    // Lookup table from header column name to its index. Duplicate headers keep
    // the last occurrence — harmless for our single-valued columns.
    using HeaderIndex = std::unordered_map<std::string, std::size_t>;

    // Trimmed cell value for `column`, or nullopt when the column is absent or
    // the cell is empty. Throws RowError on a short row (fewer cells than header).
    [[nodiscard]] std::optional<std::string> cell(const HeaderIndex& header,
                                                  const std::vector<std::string>& row,
                                                  const std::string& column) {
      const auto iter = header.find(column);
      if (iter == header.end()) {
        return std::nullopt;
      }
      if (iter->second >= row.size()) {
        throw RowError{"row has fewer columns than the header"};
      }
      const std::string& raw = row.at(iter->second);
      if (raw.empty()) {
        return std::nullopt;
      }
      return raw;
    }

    // Map one data row to a Sample, applying every pure structural rule. Throws
    // RowError on the first violation.
    [[nodiscard]] core::Sample row_to_sample(const HeaderIndex& header,
                                             const std::vector<std::string>& row,
                                             const ImportContext& context) {
      const auto name = cell(header, row, "name");
      if (!name.has_value()) {
        throw RowError{"name: required and must be non-empty"};
      }

      const auto item_type_text = cell(header, row, "item_type_id");
      if (!item_type_text.has_value()) {
        throw RowError{"item_type_id: required"};
      }

      core::Sample sample{};
      sample.id = core::SampleId::parse(core::generate_uuid_v4());
      sample.lab_id = context.lab_id;
      sample.name = name.value();
      sample.status = core::SampleStatus::Active;
      sample.created_by = context.actor;
      sample.created_at = context.now;
      sample.last_modified_by = context.actor;
      sample.last_modified_at = context.now;

      try {
        sample.item_type_id = core::ItemTypeId::parse(item_type_text.value());
      } catch (const std::exception&) {
        throw RowError{"item_type_id: invalid UUID: '" + item_type_text.value() + "'"};
      }

      sample.barcode = cell(header, row, "barcode");

      if (const auto text = cell(header, row, "container_type_id"); text.has_value()) {
        try {
          sample.container_type_id = core::ContainerTypeId::parse(text.value());
        } catch (const std::exception&) {
          throw RowError{"container_type_id: invalid UUID: '" + text.value() + "'"};
        }
      }

      if (const auto text = cell(header, row, "parent_sample_id"); text.has_value()) {
        try {
          sample.parent_sample_id = core::SampleId::parse(text.value());
        } catch (const std::exception&) {
          throw RowError{"parent_sample_id: invalid UUID: '" + text.value() + "'"};
        }
      }

      // Box placement: box_id and position_label must both be present or both
      // absent (mirrors the samples CHECK constraint).
      const auto box_text = cell(header, row, "box_id");
      const auto position = cell(header, row, "position_label");
      if (box_text.has_value() != position.has_value()) {
        throw RowError{"box_id and position_label must both be set or both empty"};
      }
      if (box_text.has_value()) {
        try {
          sample.box_id = core::BoxId::parse(box_text.value());
        } catch (const std::exception&) {
          throw RowError{"box_id: invalid UUID: '" + box_text.value() + "'"};
        }
        sample.position_label = position.value();
      }

      // Volume: value and unit travel together.
      const auto volume_value = cell(header, row, "volume_value");
      const auto volume_unit = cell(header, row, "volume_unit");
      if (volume_value.has_value() != volume_unit.has_value()) {
        throw RowError{"volume_value and volume_unit must both be set or both empty"};
      }
      if (volume_value.has_value()) {
        sample.volume_value = parse_int64(volume_value.value(), "volume_value");
        try {
          sample.volume_unit = core::parse_volume_unit(volume_unit.value());
        } catch (const std::exception&) {
          throw RowError{"volume_unit: unknown unit: '" + volume_unit.value() + "'"};
        }
      }

      // Mass: value and unit travel together.
      const auto mass_value = cell(header, row, "mass_value");
      const auto mass_unit = cell(header, row, "mass_unit");
      if (mass_value.has_value() != mass_unit.has_value()) {
        throw RowError{"mass_value and mass_unit must both be set or both empty"};
      }
      if (mass_value.has_value()) {
        sample.mass_value = parse_int64(mass_value.value(), "mass_value");
        try {
          sample.mass_unit = core::parse_mass_unit(mass_unit.value());
        } catch (const std::exception&) {
          throw RowError{"mass_unit: unknown unit: '" + mass_unit.value() + "'"};
        }
      }

      if (const auto text = cell(header, row, "custom_fields_json"); text.has_value()) {
        auto parsed = nlohmann::json::parse(text.value(), nullptr, false);
        if (parsed.is_discarded() || !parsed.is_object()) {
          throw RowError{"custom_fields_json: must be a JSON object"};
        }
        sample.custom_fields_json = text.value();
      }

      return sample;
    }

  } // namespace

  std::size_t ImportReport::ok_count() const {
    std::size_t count = 0;
    for (const auto& row : rows) {
      if (row.ok) {
        ++count;
      }
    }
    return count;
  }

  std::size_t ImportReport::error_count() const {
    return rows.size() - ok_count();
  }

  bool ImportReport::has_errors() const {
    return !header_error.empty() || error_count() > 0;
  }

  ImportReport build_import(const std::vector<std::vector<std::string>>& records,
                            const ImportContext& context) {
    ImportReport report;

    if (records.empty()) {
      report.header_error = "empty CSV: no header row";
      return report;
    }

    HeaderIndex header;
    for (std::size_t col = 0; col < records.front().size(); ++col) {
      header.emplace(records.front().at(col), col);
    }
    if (!header.contains("name") || !header.contains("item_type_id")) {
      report.header_error = "header must contain at least 'name' and 'item_type_id' columns";
      return report;
    }

    // Track (box_id, position_label) pairs already claimed within this file so a
    // self-conflicting import is reported up front rather than failing at commit.
    std::unordered_set<std::string> claimed_positions;

    for (std::size_t index = 1; index < records.size(); ++index) {
      ImportRowResult result;
      result.row_number = index; // 1-based across data rows
      try {
        core::Sample sample = row_to_sample(header, records.at(index), context);
        if (sample.box_id.has_value() && sample.position_label.has_value()) {
          const std::string box = sample.box_id.value().to_string();
          const std::string position = sample.position_label.value();
          std::string key = box;
          key += '\x1f';
          key += position;
          if (!claimed_positions.insert(key).second) {
            std::string message = "duplicate position in import file: box ";
            message += box;
            message += " position ";
            message += position;
            throw RowError{message};
          }
        }
        result.ok = true;
        result.sample = std::move(sample);
      } catch (const RowError& error) {
        result.ok = false;
        result.error = error.message;
      }
      report.rows.push_back(std::move(result));
    }

    return report;
  }

} // namespace fmgr::cli
