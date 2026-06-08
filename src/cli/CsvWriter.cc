// SPDX-License-Identifier: AGPL-3.0-or-later

#include "cli/CsvWriter.h"

namespace fmgr::cli {

  std::string csv_escape_field(std::string_view field) {
    const bool needs_quoting = field.find_first_of(",\"\r\n") != std::string_view::npos;
    if (!needs_quoting) {
      return std::string(field);
    }

    std::string escaped;
    escaped.reserve(field.size() + 2);
    escaped.push_back('"');
    for (const char character : field) {
      if (character == '"') {
        escaped.push_back('"'); // double an embedded quote
      }
      escaped.push_back(character);
    }
    escaped.push_back('"');
    return escaped;
  }

  void write_csv_row(std::ostream& out, const std::vector<std::string>& fields) {
    bool first = true;
    for (const auto& field : fields) {
      if (!first) {
        out << ',';
      }
      out << csv_escape_field(field);
      first = false;
    }
    out << "\r\n";
  }

} // namespace fmgr::cli
