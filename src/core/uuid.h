// SPDX-License-Identifier: AGPL-3.0-or-later

// RFC 4122 UUID value type — parse and format only, no generation.
// Callers obtain UUID strings from the OS (e.g. libuuid, /proc/sys/kernel/random/uuid)
// or from the database DEFAULT and pass them here as strings.
// `to_string()` always emits lowercase hex; `parse()` accepts both cases.
// Stored internally as 16 raw bytes to avoid repeated string allocation at rest.
#ifndef FMGR_CORE_UUID_H
#define FMGR_CORE_UUID_H

#include <nlohmann/json.hpp>

#include <array>
#include <compare>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace fmgr::core {

  class Uuid {
  public:
    constexpr Uuid() = default;
    explicit constexpr Uuid(std::array<std::uint8_t, 16> bytes) : bytes_(bytes) {}

    [[nodiscard]] static Uuid parse(std::string_view text) {
      if (text.size() != 36) {
        throw std::invalid_argument("invalid UUID length");
      }

      constexpr std::array<std::size_t, 4> hyphen_positions{8, 13, 18, 23};
      for (const auto position : hyphen_positions) {
        if (text.at(position) != '-') {
          throw std::invalid_argument("invalid UUID hyphen placement");
        }
      }

      std::array<std::uint8_t, 16> bytes{};
      std::size_t byte_index = 0;
      for (std::size_t text_index = 0; text_index < text.size();) {
        if (text.at(text_index) == '-') {
          ++text_index;
          continue;
        }

        if (byte_index >= bytes.size() || text_index + 1 >= text.size()) {
          throw std::invalid_argument("invalid UUID byte sequence");
        }

        const auto high = hex_value(text.at(text_index));
        const auto low = hex_value(text.at(text_index + 1));
        bytes.at(byte_index) = static_cast<std::uint8_t>((high << 4U) | low);
        ++byte_index;
        text_index += 2;
      }

      if (byte_index != bytes.size()) {
        throw std::invalid_argument("invalid UUID byte count");
      }

      return Uuid(bytes);
    }

    [[nodiscard]] std::string to_string() const {
      constexpr std::string_view digits = "0123456789abcdef";
      std::string result;
      result.reserve(36);

      for (std::size_t index = 0; index < bytes_.size(); ++index) {
        if (index == 4 || index == 6 || index == 8 || index == 10) {
          result.push_back('-');
        }
        const auto byte = bytes_.at(index);
        result.push_back(digits[(byte >> 4U) & 0x0fU]);
        result.push_back(digits[byte & 0x0fU]);
      }

      return result;
    }

    [[nodiscard]] constexpr const std::array<std::uint8_t, 16>& bytes() const {
      return bytes_;
    }

    friend constexpr auto operator<=>(const Uuid&, const Uuid&) = default;

  private:
    [[nodiscard]] static std::uint8_t hex_value(char value) {
      if (value >= '0' && value <= '9') {
        return static_cast<std::uint8_t>(value - '0');
      }
      if (value >= 'a' && value <= 'f') {
        return static_cast<std::uint8_t>(value - 'a' + 10);
      }
      if (value >= 'A' && value <= 'F') {
        return static_cast<std::uint8_t>(value - 'A' + 10);
      }
      throw std::invalid_argument("invalid UUID hex digit");
    }

    std::array<std::uint8_t, 16> bytes_{};
  };

  inline void to_json(nlohmann::json& json, const Uuid& uuid) {
    json = uuid.to_string();
  }

  inline void from_json(const nlohmann::json& json, Uuid& uuid) {
    uuid = Uuid::parse(json.get<std::string>());
  }

} // namespace fmgr::core

#endif // FMGR_CORE_UUID_H
