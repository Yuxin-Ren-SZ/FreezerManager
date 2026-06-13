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
      std::size_t step_quoted(const std::string& content, std::size_t pos, std::size_t size) {
        const char chr = content[pos];
        if (chr != '"') {
          field_.push_back(chr);
          return pos + 1;
        }
        // Doubled quote inside a quoted field is a literal quote.
        if (pos + 1 < size && content[pos + 1] == '"') {
          field_.push_back('"');
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
        field_.push_back(chr);
        return pos + 1;
      }

      void end_field() {
        record_.push_back(field_);
        field_.clear();
      }

      void end_record() {
        end_field();
        rows_.push_back(record_);
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
          rows_.push_back(record_);
        }
      }

      std::vector<std::vector<std::string>> rows_;
      std::vector<std::string> record_;
      std::string field_;
      bool in_quotes_{false};
      bool at_record_start_{true};
    };

  } // namespace

  std::vector<std::vector<std::string>> parse_csv(std::istream& input) {
    const std::string content((std::istreambuf_iterator<char>(input)),
                              std::istreambuf_iterator<char>());
    return CsvParser{}.parse(content);
  }

} // namespace fmgr::cli
