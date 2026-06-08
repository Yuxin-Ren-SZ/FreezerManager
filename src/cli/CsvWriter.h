// SPDX-License-Identifier: AGPL-3.0-or-later

// RFC 4180 CSV field/row serialization. Kept as a standalone unit (no domain
// types) so it can be unit-tested and fuzzed in isolation — the PRD lists the
// CSV path as a fuzz target.
#ifndef FMGR_CLI_CSVWRITER_H
#define FMGR_CLI_CSVWRITER_H

#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace fmgr::cli {

  // Quote and escape a single field per RFC 4180: a field is wrapped in double
  // quotes when it contains a comma, a double quote, CR, or LF; embedded double
  // quotes are doubled. Other fields are returned unchanged.
  [[nodiscard]] std::string csv_escape_field(std::string_view field);

  // Write one CSV record (comma-separated escaped fields) followed by CRLF.
  void write_csv_row(std::ostream& out, const std::vector<std::string>& fields);

} // namespace fmgr::cli

#endif // FMGR_CLI_CSVWRITER_H
