// SPDX-License-Identifier: AGPL-3.0-or-later

// RFC 4180 CSV parsing — the read counterpart to CsvWriter. Kept domain-free so
// it can be unit-tested and fuzzed in isolation (the PRD lists the CSV importer
// as a fuzz target). Lines whose first character is '#' at the start of a record
// are skipped, so a file produced by `freezerctl sample export` (which prefixes
// a chain-of-custody comment block) round-trips through the importer unchanged.
#ifndef FMGR_CLI_CSVREADER_H
#define FMGR_CLI_CSVREADER_H

#include <istream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fmgr::cli {

  // Thrown when the input is not well-formed CSV (currently: an unterminated
  // quoted field). The message is safe to surface to the operator.
  class CsvParseError : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
  };

  // Parse a full CSV document into records of fields. Honours RFC 4180 quoting
  // (double-quoted fields, doubled embedded quotes, embedded commas/CR/LF),
  // accepts CRLF or bare-LF record separators, and skips comment lines that
  // begin with '#'. A trailing record separator does not yield a spurious empty
  // final record. Throws CsvParseError on a malformed document.
  [[nodiscard]] std::vector<std::vector<std::string>> parse_csv(std::istream& input);

} // namespace fmgr::cli

#endif // FMGR_CLI_CSVREADER_H
