// SPDX-License-Identifier: AGPL-3.0-or-later

#include "cli/SampleCsv.h"

#include "cli/CsvWriter.h"

#include <nlohmann/json.hpp>

namespace fmgr::cli {

  namespace {

    // Stringify a JSON scalar for a CSV cell: null -> empty, string -> raw value,
    // everything else -> compact dump (numbers, booleans). custom_fields_json is
    // already a JSON string, so it round-trips as its literal text.
    [[nodiscard]] std::string json_scalar_to_string(const nlohmann::json& value) {
      if (value.is_null()) {
        return {};
      }
      if (value.is_string()) {
        return value.get<std::string>();
      }
      return value.dump();
    }

  } // namespace

  const std::vector<std::string>& sample_csv_columns() {
    // Mirrors core::to_json(Sample) key order, minus phi_fields_enc_json.
    static const std::vector<std::string> columns = {"id",
                                                     "lab_id",
                                                     "item_type_id",
                                                     "name",
                                                     "barcode",
                                                     "container_type_id",
                                                     "box_id",
                                                     "position_label",
                                                     "volume_value",
                                                     "volume_unit",
                                                     "mass_value",
                                                     "mass_unit",
                                                     "status",
                                                     "parent_sample_id",
                                                     "created_by",
                                                     "created_at",
                                                     "last_modified_by",
                                                     "last_modified_at",
                                                     "custom_fields_json"};
    return columns;
  }

  std::vector<std::string> sample_to_csv_row(const core::Sample& sample) {
    const nlohmann::json json = sample;
    std::vector<std::string> row;
    row.reserve(sample_csv_columns().size());
    for (const auto& column : sample_csv_columns()) {
      row.push_back(json_scalar_to_string(json.at(column)));
    }
    return row;
  }

  void write_export_header(std::ostream& out, int schema_version, std::string_view lab_id,
                           std::string_view exported_at, std::string_view signature) {
    out << "# freezermanager-export schema_version=" << schema_version << "\r\n";
    out << "# lab_id=" << lab_id << "\r\n";
    out << "# exported_at=" << exported_at << "\r\n";
    out << "# signature=" << signature << "\r\n";
  }

  void write_sample_csv(std::ostream& out, const std::vector<core::Sample>& samples,
                        int schema_version, std::string_view lab_id, std::string_view exported_at) {
    write_export_header(out, schema_version, lab_id, exported_at);
    write_csv_row(out, sample_csv_columns());
    for (const auto& sample : samples) {
      write_csv_row(out, sample_to_csv_row(sample));
    }
  }

} // namespace fmgr::cli
