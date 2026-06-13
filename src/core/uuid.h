// SPDX-License-Identifier: AGPL-3.0-or-later

// RFC 4122 UUID value type — parse and format. The `Uuid` class itself does no
// generation; `generate_uuid_v4()` below mints a random v4 string for service-layer
// entity IDs (unguessable but not required to be cryptographically secret; the
// storage layer uses a libsodium-backed generator for tokens/session IDs).
// `to_string()` always emits lowercase hex; `parse()` accepts both cases.
// Stored internally as 16 raw bytes to avoid repeated string allocation at rest.
#ifndef FMGR_CORE_UUID_H
#define FMGR_CORE_UUID_H

#include <nlohmann/json.hpp>

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <random>
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

  // Mint a fresh RFC 4122 version-4 UUID string from a non-deterministic source.
  [[nodiscard]] inline std::string generate_uuid_v4() {
    std::random_device rng;
    std::array<std::uint8_t, 16> bytes{};
    for (std::size_t i = 0; i < bytes.size(); i += 4) {
      const std::uint32_t word = rng();
      bytes[i] = static_cast<std::uint8_t>(word & 0xFFU);
      bytes[i + 1] = static_cast<std::uint8_t>((word >> 8U) & 0xFFU);
      bytes[i + 2] = static_cast<std::uint8_t>((word >> 16U) & 0xFFU);
      bytes[i + 3] = static_cast<std::uint8_t>((word >> 24U) & 0xFFU);
    }
    bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0FU) | 0x40U); // version 4
    bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3FU) | 0x80U); // variant
    std::array<char, 37> buf{};
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    std::snprintf(buf.data(), buf.size(),
                  "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", bytes[0],
                  bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7], bytes[8],
                  bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
    return {buf.data()};
  }

} // namespace fmgr::core

#endif // FMGR_CORE_UUID_H
