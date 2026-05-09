// SPDX-License-Identifier: AGPL-3.0-or-later

// UTC microsecond-precision timestamp. `int64_t` covers dates through year 2262,
// which exceeds any plausible biospecimen retention requirement.
// JSON serialization is a bare integer (not ISO-8601) to eliminate timezone
// ambiguity and parsing overhead; callers convert to display strings at the API layer.
// `from_unix_micros` is the sole construction path so default-initialized timestamps
// are always the Unix epoch (0), never an indeterminate value.
#ifndef FMGR_CORE_TIMESTAMP_H
#define FMGR_CORE_TIMESTAMP_H

#include <nlohmann/json.hpp>

#include <compare>
#include <cstdint>

namespace fmgr::core {

  class Timestamp {
  public:
    constexpr Timestamp() = default;

    [[nodiscard]] static constexpr Timestamp from_unix_micros(std::int64_t unix_micros) {
      return Timestamp(unix_micros);
    }

    [[nodiscard]] constexpr std::int64_t unix_micros() const {
      return unix_micros_;
    }

    friend constexpr auto operator<=>(const Timestamp&, const Timestamp&) = default;

  private:
    explicit constexpr Timestamp(std::int64_t unix_micros) : unix_micros_(unix_micros) {}

    std::int64_t unix_micros_{0};
  };

  inline void to_json(nlohmann::json& json, const Timestamp& timestamp) {
    json = timestamp.unix_micros();
  }

  inline void from_json(const nlohmann::json& json, Timestamp& timestamp) {
    timestamp = Timestamp::from_unix_micros(json.get<std::int64_t>());
  }

} // namespace fmgr::core

#endif // FMGR_CORE_TIMESTAMP_H
