// SPDX-License-Identifier: AGPL-3.0-or-later

#include "backup/BackupFilename.h"

#include <array>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <optional>
#include <string>
#include <string_view>

namespace fmgr::backup {

  namespace {

    constexpr std::string_view k_prefix = "fmgr-";
    constexpr std::string_view k_suffix = ".fmgrbak";
    constexpr std::int64_t k_micros_per_second = 1'000'000;
    // "YYYYMMDDThhmmssZ"
    constexpr std::size_t k_stamp_len = 16;

  } // namespace

  std::string make_backup_filename(std::int64_t created_micros) {
    const auto seconds = static_cast<std::time_t>(created_micros / k_micros_per_second);
    std::tm tm_utc{};
    ::gmtime_r(&seconds, &tm_utc);

    std::array<char, k_stamp_len + 1> stamp{};
    std::snprintf(stamp.data(), stamp.size(), "%04d%02d%02dT%02d%02d%02dZ", tm_utc.tm_year + 1900,
                  tm_utc.tm_mon + 1, tm_utc.tm_mday, tm_utc.tm_hour, tm_utc.tm_min, tm_utc.tm_sec);
    return std::string(k_prefix) + stamp.data() + std::string(k_suffix);
  }

  std::optional<std::int64_t> parse_backup_timestamp(std::string_view name) {
    if (name.size() != k_prefix.size() + k_stamp_len + k_suffix.size()) {
      return std::nullopt;
    }
    if (name.substr(0, k_prefix.size()) != k_prefix ||
        name.substr(name.size() - k_suffix.size()) != k_suffix) {
      return std::nullopt;
    }
    const std::string_view stamp = name.substr(k_prefix.size(), k_stamp_len);

    // Fixed-layout check: digits everywhere except 'T' at [8] and 'Z' at [15].
    for (std::size_t i = 0; i < k_stamp_len; ++i) {
      const char c = stamp[i];
      if (i == 8) {
        if (c != 'T') {
          return std::nullopt;
        }
      } else if (i == 15) {
        if (c != 'Z') {
          return std::nullopt;
        }
      } else if (std::isdigit(static_cast<unsigned char>(c)) == 0) {
        return std::nullopt;
      }
    }

    const auto digits = [&](std::size_t off, std::size_t len) {
      int value = 0;
      for (std::size_t i = 0; i < len; ++i) {
        value = value * 10 + (stamp[off + i] - '0');
      }
      return value;
    };

    std::tm tm_utc{};
    tm_utc.tm_year = digits(0, 4) - 1900;
    tm_utc.tm_mon = digits(4, 2) - 1;
    tm_utc.tm_mday = digits(6, 2);
    tm_utc.tm_hour = digits(9, 2);
    tm_utc.tm_min = digits(11, 2);
    tm_utc.tm_sec = digits(13, 2);

    // timegm interprets the broken-down time as UTC. It normalizes out-of-range
    // fields, so re-format and require an exact round-trip to reject values like
    // month 13 or day 32 that a forgiving timegm would silently roll over.
    const std::time_t seconds = ::timegm(&tm_utc);
    if (seconds == static_cast<std::time_t>(-1)) {
      return std::nullopt;
    }
    if (make_backup_filename(static_cast<std::int64_t>(seconds) * k_micros_per_second) != name) {
      return std::nullopt;
    }
    return static_cast<std::int64_t>(seconds) * k_micros_per_second;
  }

} // namespace fmgr::backup
