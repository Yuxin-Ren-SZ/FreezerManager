// SPDX-License-Identifier: AGPL-3.0-or-later

#include "cli/CsvReader.h"

#include <cstddef>
#include <iterator>

namespace fmgr::cli {

  namespace {

    // RFC 4180 state machine. Each step_*() consumes one or more characters and
    // returns the next read position, keeping the per-branch logic small enough
    // to read (and within clang-tidy's cognitive-complexity budget).
    class CsvParser {
    public:
      explicit CsvParser(const CsvLimits& limits) : limits_(limits) {}

      std::vector<std::vector<std::string>> parse(const std::string& content) {
        const std::size_t size = content.size();
        std::size_t pos = 0;
        while (pos < size) {
          pos = in_quotes_ ? step_quoted(content, pos, size) : step_unquoted(content, pos, size);
        }
        finish();
        return std::move(rows_);
      }

    private:
      void push_field_char(char chr) {
        if (field_.size() >= limits_.max_field_length) {
          throw CsvParseError("CSV parse error: field exceeds maximum length");
        }
        field_.push_back(chr);
      }

      std::size_t step_quoted(const std::string& content, std::size_t pos, std::size_t size) {
        const char chr = content[pos];
        if (chr != '"') {
          push_field_char(chr);
          return pos + 1;
        }
        // Doubled quote inside a quoted field is a literal quote.
        if (pos + 1 < size && content[pos + 1] == '"') {
          push_field_char('"');
          return pos + 2;
        }
        in_quotes_ = false;
        return pos + 1;
      }

      std::size_t step_unquoted(const std::string& content, std::size_t pos, std::size_t size) {
        const char chr = content[pos];

        // A '#' at the very start of a record marks a comment line — skip past
        // the next LF without emitting a record.
        if (at_record_start_ && chr == '#') {
          std::size_t next = pos;
          while (next < size && content[next] != '\n') {
            ++next;
          }
          return next < size ? next + 1 : next; // stay at_record_start
        }

        at_record_start_ = false;

        if (chr == '"') {
          in_quotes_ = true;
          return pos + 1;
        }
        if (chr == ',') {
          end_field();
          return pos + 1;
        }
        if (chr == '\r' || chr == '\n') {
          const bool crlf = chr == '\r' && pos + 1 < size && content[pos + 1] == '\n';
          end_record();
          return pos + (crlf ? 2 : 1);
        }
        push_field_char(chr);
        return pos + 1;
      }

      void end_field() {
        if (record_.size() >= limits_.max_fields_per_row) {
          throw CsvParseError("CSV parse error: row exceeds maximum field count");
        }
        record_.push_back(field_);
        field_.clear();
      }

      void push_row() {
        if (rows_.size() >= limits_.max_rows) {
          throw CsvParseError("CSV parse error: document exceeds maximum row count");
        }
        rows_.push_back(record_);
      }

      void end_record() {
        end_field();
        push_row();
        record_.clear();
        at_record_start_ = true;
      }

      void finish() {
        if (in_quotes_) {
          throw CsvParseError("CSV parse error: unterminated quoted field");
        }
        // Flush a final record not terminated by a newline. A file that ended on
        // a record separator leaves empty buffers, so no spurious record is added.
        if (!at_record_start_ || !field_.empty() || !record_.empty()) {
          end_field();
          push_row();
        }
      }

      CsvLimits limits_;
      std::vector<std::vector<std::string>> rows_;
      std::vector<std::string> record_;
      std::string field_;
      bool in_quotes_{false};
      bool at_record_start_{true};
    };

  } // namespace

  std::vector<std::vector<std::string>> parse_csv(std::istream& input, const CsvLimits& limits) {
    const std::string content((std::istreambuf_iterator<char>(input)),
                              std::istreambuf_iterator<char>());
    return CsvParser{limits}.parse(content);
  }

} // namespace fmgr::cli
