// SPDX-License-Identifier: AGPL-3.0-or-later

// Integer-based physical quantity types for volume and mass.
// `raw_value_` stores the quantity in the declared unit with no implicit conversion,
// avoiding floating-point precision loss in cumulative operations on small volumes
// (e.g. repeated subtraction of 0.5 µL). Arithmetic across different units is
// currently rejected at runtime; unit conversion will canonicalize both operands
// once the full unit set is defined (see TODOs below).
#ifndef FMGR_CORE_QUANTITY_H
#define FMGR_CORE_QUANTITY_H

#include <nlohmann/json.hpp>

#include <compare>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace fmgr::core {

  enum class VolumeUnit : std::uint8_t { Milliliter, Microliter }; // TODO Add more volume units
  enum class MassUnit : std::uint8_t { Milligram, Gram };          // TODO Add more mass units

  // TODO Add conversion between different units, e.g. 1 mL = 1000 µL, 1 g = 1000 mg

  [[nodiscard]] inline std::string_view to_string(VolumeUnit unit) {
    switch (unit) {
    case VolumeUnit::Milliliter:
      return "mL";
    case VolumeUnit::Microliter:
      return "µL";
    }
    throw std::invalid_argument("unknown volume unit");
  }

  [[nodiscard]] inline VolumeUnit parse_volume_unit(std::string_view text) {
    if (text == "mL") {
      return VolumeUnit::Milliliter;
    }
    if (text == "µL") {
      return VolumeUnit::Microliter;
    }
    throw std::invalid_argument("unknown volume unit");
  }

  [[nodiscard]] inline std::string_view to_string(MassUnit unit) {
    switch (unit) {
    case MassUnit::Milligram:
      return "mg";
    case MassUnit::Gram:
      return "g";
    }
    throw std::invalid_argument("unknown mass unit");
  }

  [[nodiscard]] inline MassUnit parse_mass_unit(std::string_view text) {
    if (text == "mg") {
      return MassUnit::Milligram;
    }
    if (text == "g") {
      return MassUnit::Gram;
    }
    throw std::invalid_argument("unknown mass unit");
  }

  class Volume {
  public:
    constexpr Volume() = default;

    // Sole construction path. Forces callers to be explicit about the unit so
    // that quantities are never created with an ambiguous default unit.
    [[nodiscard]] static constexpr Volume from_raw(std::int64_t raw_value, VolumeUnit unit) {
      return {raw_value, unit};
    }

    [[nodiscard]] constexpr std::int64_t raw_value() const {
      return raw_value_;
    }
    [[nodiscard]] constexpr VolumeUnit unit() const {
      return unit_;
    }

    friend constexpr bool operator==(const Volume&, const Volume&) = default;

    [[nodiscard]] friend Volume operator+(const Volume& left, const Volume& right) {
      assert_same_unit(left, right);
      return {left.raw_value_ + right.raw_value_, left.unit_};
    }

    [[nodiscard]] friend Volume operator-(const Volume& left, const Volume& right) {
      assert_same_unit(left, right);
      return {left.raw_value_ - right.raw_value_, left.unit_};
    }

    [[nodiscard]] friend bool operator<(const Volume& left, const Volume& right) {
      assert_same_unit(left, right);
      return left.raw_value_ < right.raw_value_;
    }

    [[nodiscard]] friend bool operator>(const Volume& left, const Volume& right) {
      return right < left;
    }
    [[nodiscard]] friend bool operator<=(const Volume& left, const Volume& right) {
      return !(right < left);
    }
    [[nodiscard]] friend bool operator>=(const Volume& left, const Volume& right) {
      return !(left < right);
    }

  private:
    constexpr Volume(std::int64_t raw_value, VolumeUnit unit)
        : raw_value_(raw_value), unit_(unit) {}

    // Throws until cross-unit arithmetic is implemented. At that point this
    // helper will canonicalize both operands to a common unit before returning.
    static void assert_same_unit(const Volume& left, const Volume& right) {
      if (left.unit_ != right.unit_) {
        throw std::invalid_argument("volume unit mismatch");
      }
    }

    std::int64_t raw_value_{0};
    VolumeUnit unit_{VolumeUnit::Milliliter};
  };

  class Mass {
  public:
    constexpr Mass() = default;

    // Sole construction path. See `Volume::from_raw` for rationale.
    [[nodiscard]] static constexpr Mass from_raw(std::int64_t raw_value, MassUnit unit) {
      return {raw_value, unit};
    }

    [[nodiscard]] constexpr std::int64_t raw_value() const {
      return raw_value_;
    }
    [[nodiscard]] constexpr MassUnit unit() const {
      return unit_;
    }

    friend constexpr bool operator==(const Mass&, const Mass&) = default;

    [[nodiscard]] friend Mass operator+(const Mass& left, const Mass& right) {
      assert_same_unit(left, right);
      return {left.raw_value_ + right.raw_value_, left.unit_};
    }

    [[nodiscard]] friend Mass operator-(const Mass& left, const Mass& right) {
      assert_same_unit(left, right);
      return {left.raw_value_ - right.raw_value_, left.unit_};
    }

    [[nodiscard]] friend bool operator<(const Mass& left, const Mass& right) {
      assert_same_unit(left, right);
      return left.raw_value_ < right.raw_value_;
    }

    [[nodiscard]] friend bool operator>(const Mass& left, const Mass& right) {
      return right < left;
    }
    [[nodiscard]] friend bool operator<=(const Mass& left, const Mass& right) {
      return !(right < left);
    }
    [[nodiscard]] friend bool operator>=(const Mass& left, const Mass& right) {
      return !(left < right);
    }

  private:
    constexpr Mass(std::int64_t raw_value, MassUnit unit) : raw_value_(raw_value), unit_(unit) {}

    // See `Volume::assert_same_unit` for rationale.
    static void assert_same_unit(const Mass& left, const Mass& right) {
      if (left.unit_ != right.unit_) {
        throw std::invalid_argument("mass unit mismatch");
      }
    }

    std::int64_t raw_value_{0};
    MassUnit unit_{MassUnit::Milligram};
  };

  inline void to_json(nlohmann::json& json, const Volume& volume) {
    json = nlohmann::json{
        {"value", volume.raw_value()},
        {"unit", std::string(to_string(volume.unit()))},
    };
  }

  inline void from_json(const nlohmann::json& json, Volume& volume) {
    volume = Volume::from_raw(json.at("value").get<std::int64_t>(),
                              parse_volume_unit(json.at("unit").get<std::string>()));
  }

  inline void to_json(nlohmann::json& json, const Mass& mass) {
    json = nlohmann::json{
        {"value", mass.raw_value()},
        {"unit", std::string(to_string(mass.unit()))},
    };
  }

  inline void from_json(const nlohmann::json& json, Mass& mass) {
    mass = Mass::from_raw(json.at("value").get<std::int64_t>(),
                          parse_mass_unit(json.at("unit").get<std::string>()));
  }

} // namespace fmgr::core

#endif // FMGR_CORE_QUANTITY_H
