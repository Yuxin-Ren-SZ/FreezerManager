// SPDX-License-Identifier: AGPL-3.0-or-later

// RFC 4180 CSV parsing — the read counterpart to CsvWriter. Kept domain-free so
// it can be unit-tested and fuzzed in isolation (the PRD lists the CSV importer
// as a fuzz target). Lines whose first character is '#' at the start of a record
// are skipped, so a file produced by `freezerctl sample export` (which prefixes
// a chain-of-custody comment block) round-trips through the importer unchanged.
#ifndef FMGR_CLI_CSVREADER_H
#define FMGR_CLI_CSVREADER_H

#include <cstddef>
#include <istream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fmgr::cli {

  // Thrown when the input is not well-formed CSV (an unterminated quoted field)
  // or when it exceeds a configured resource limit. The message is safe to
  // surface to the operator.
  class CsvParseError : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
  };

  // Resource limits guarding the parser against memory exhaustion from a
  // maliciously crafted document (review F-8 / security-audit M-1). Defaults are
  // generous enough not to reject a legitimate `freezerctl sample export`; tune
  // per call site if a deployment needs tighter bounds.
  struct CsvLimits {
    std::size_t max_field_length = std::size_t{1} << 20; // 1 MiB per field
    std::size_t max_fields_per_row = 1024;
    std::size_t max_rows = 1'000'000;
  };

  // Parse a full CSV document into records of fields. Honours RFC 4180 quoting
  // (double-quoted fields, doubled embedded quotes, embedded commas/CR/LF),
  // accepts CRLF or bare-LF record separators, and skips comment lines that
  // begin with '#'. A trailing record separator does not yield a spurious empty
  // final record. Throws CsvParseError on a malformed document or when `limits`
  // are exceeded.
  [[nodiscard]] std::vector<std::vector<std::string>> parse_csv(std::istream& input,
                                                                const CsvLimits& limits = {});

} // namespace fmgr::cli

#endif // FMGR_CLI_CSVREADER_H
