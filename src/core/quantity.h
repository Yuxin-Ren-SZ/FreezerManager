// SPDX-License-Identifier: AGPL-3.0-or-later

#ifndef FMGR_CORE_QUANTITY_H
#define FMGR_CORE_QUANTITY_H

#include <nlohmann/json.hpp>

#include <compare>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace fmgr::core {

  enum class VolumeUnit : std::uint8_t { Milliliter, Microliter };
  enum class MassUnit : std::uint8_t { Milligram, Gram };

  [[nodiscard]] inline std::string_view to_string(VolumeUnit unit) {
    switch (unit) {
    case VolumeUnit::Milliliter:
      return "mL";
    case VolumeUnit::Microliter:
      return "uL";
    }
    throw std::invalid_argument("unknown volume unit");
  }

  [[nodiscard]] inline VolumeUnit parse_volume_unit(std::string_view text) {
    if (text == "mL") {
      return VolumeUnit::Milliliter;
    }
    if (text == "uL") {
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

    [[nodiscard]] static constexpr Volume from_microunits(std::int64_t value_microunits,
                                                          VolumeUnit unit) {
      return {value_microunits, unit};
    }

    [[nodiscard]] constexpr std::int64_t value_microunits() const {
      return value_microunits_;
    }
    [[nodiscard]] constexpr VolumeUnit unit() const {
      return unit_;
    }

    friend constexpr bool operator==(const Volume&, const Volume&) = default;

    [[nodiscard]] friend Volume operator+(const Volume& left, const Volume& right) {
      assert_same_unit(left, right);
      return {left.value_microunits_ + right.value_microunits_, left.unit_};
    }

    [[nodiscard]] friend Volume operator-(const Volume& left, const Volume& right) {
      assert_same_unit(left, right);
      return {left.value_microunits_ - right.value_microunits_, left.unit_};
    }

    [[nodiscard]] friend bool operator<(const Volume& left, const Volume& right) {
      assert_same_unit(left, right);
      return left.value_microunits_ < right.value_microunits_;
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
    constexpr Volume(std::int64_t value_microunits, VolumeUnit unit)
        : value_microunits_(value_microunits), unit_(unit) {}

    static void assert_same_unit(const Volume& left, const Volume& right) {
      if (left.unit_ != right.unit_) {
        throw std::invalid_argument("volume unit mismatch");
      }
    }

    std::int64_t value_microunits_{0};
    VolumeUnit unit_{VolumeUnit::Milliliter};
  };

  class Mass {
  public:
    constexpr Mass() = default;

    [[nodiscard]] static constexpr Mass from_microunits(std::int64_t value_microunits,
                                                        MassUnit unit) {
      return {value_microunits, unit};
    }

    [[nodiscard]] constexpr std::int64_t value_microunits() const {
      return value_microunits_;
    }
    [[nodiscard]] constexpr MassUnit unit() const {
      return unit_;
    }

    friend constexpr bool operator==(const Mass&, const Mass&) = default;

    [[nodiscard]] friend Mass operator+(const Mass& left, const Mass& right) {
      assert_same_unit(left, right);
      return {left.value_microunits_ + right.value_microunits_, left.unit_};
    }

    [[nodiscard]] friend Mass operator-(const Mass& left, const Mass& right) {
      assert_same_unit(left, right);
      return {left.value_microunits_ - right.value_microunits_, left.unit_};
    }

    [[nodiscard]] friend bool operator<(const Mass& left, const Mass& right) {
      assert_same_unit(left, right);
      return left.value_microunits_ < right.value_microunits_;
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
    constexpr Mass(std::int64_t value_microunits, MassUnit unit)
        : value_microunits_(value_microunits), unit_(unit) {}

    static void assert_same_unit(const Mass& left, const Mass& right) {
      if (left.unit_ != right.unit_) {
        throw std::invalid_argument("mass unit mismatch");
      }
    }

    std::int64_t value_microunits_{0};
    MassUnit unit_{MassUnit::Milligram};
  };

  inline void to_json(nlohmann::json& json, const Volume& volume) {
    json = nlohmann::json{
        {"value_microunits", volume.value_microunits()},
        {"unit", std::string(to_string(volume.unit()))},
    };
  }

  inline void from_json(const nlohmann::json& json, Volume& volume) {
    volume = Volume::from_microunits(json.at("value_microunits").get<std::int64_t>(),
                                     parse_volume_unit(json.at("unit").get<std::string>()));
  }

  inline void to_json(nlohmann::json& json, const Mass& mass) {
    json = nlohmann::json{
        {"value_microunits", mass.value_microunits()},
        {"unit", std::string(to_string(mass.unit()))},
    };
  }

  inline void from_json(const nlohmann::json& json, Mass& mass) {
    mass = Mass::from_microunits(json.at("value_microunits").get<std::int64_t>(),
                                 parse_mass_unit(json.at("unit").get<std::string>()));
  }

} // namespace fmgr::core

#endif // FMGR_CORE_QUANTITY_H
